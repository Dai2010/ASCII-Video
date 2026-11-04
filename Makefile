# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion
#
# miku 构建脚本。常用目标：
#   make                                编译 $(BUILD_DIR)/miku（默认 build/miku）
#   make test                           编译并运行 tests/ 下的单元测试
#   make clean                          删除本构建目录里的目标文件与可执行文件
#   make help                           列出目标与可覆盖变量
#   make BUILD_DIR=build/debug OPT="-O0 -g3"   另起一个调试构建目录
#
# 可覆盖变量：
#   CC         编译器，默认 cc
#   OPT        优化与调试选项，默认 -O2 -g（不要用 CFLAGS 覆盖，会丢掉警告设置）
#   WERROR     默认 -Werror；未知编译器上可设为空以忽略新出现的警告
#   BUILD_DIR  产物目录，默认 build；所有变体共用同一套源文件，只换目录与 OPT
#   PYTHON     tests/style.py 使用的解释器，默认优先 .venv 中的 Python

CC        ?= cc
OPT       ?= -O2 -g
WERROR    ?= -Werror
BUILD_DIR ?= build
PYTHON    ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
CFLAGS   += -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	    -Wstrict-prototypes -Wmissing-prototypes \
	    -Wdeclaration-after-statement $(WERROR)
LDLIBS   += -lm

SOURCES     := $(wildcard src/*.c)
OBJECTS     := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDS     := $(OBJECTS:.o=.d)
# 测试不链接程序入口 main.o，也不链接音频后端 audio.o（需要时单独加入）
LIB_OBJECTS := $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/audio.o,$(OBJECTS))
PROGRAM     := $(BUILD_DIR)/miku
TESTS       := $(BUILD_DIR)/playback-test $(BUILD_DIR)/render-test \
	       $(BUILD_DIR)/buffer-test $(BUILD_DIR)/video-test
# 测试超时保护：有 timeout（或 macOS 上的 gtimeout）就用，没有则直接运行
TIMEOUT := $(firstword $(shell command -v timeout gtimeout 2>/dev/null))

.DEFAULT_GOAL := all
.PHONY: all test format-check clean help

all: $(PROGRAM)

$(BUILD_DIR):
	mkdir -p $@

# 每个源文件单独编译；-MMD -MP 生成头文件依赖，改动 include/ 时自动重编
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) -MMD -MP -c $< -o $@

$(PROGRAM): $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/playback-test: tests/playback.c $(LIB_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(LDFLAGS) $< $(LIB_OBJECTS) \
		$(LDLIBS) -o $@

$(BUILD_DIR)/render-test: tests/render.c $(LIB_OBJECTS) $(BUILD_DIR)/audio.o \
		| $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(LDFLAGS) $< $(LIB_OBJECTS) \
		$(BUILD_DIR)/audio.o $(LDLIBS) -o $@

$(BUILD_DIR)/buffer-test: tests/buffer.c $(BUILD_DIR)/strbuf.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/video-test: tests/video.c $(LIB_OBJECTS) $(BUILD_DIR)/audio.o \
		| $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPT) $(LDFLAGS) $< $(LIB_OBJECTS) \
		$(BUILD_DIR)/audio.o $(LDLIBS) -o $@

test: $(TESTS)
	$(if $(TIMEOUT),$(TIMEOUT) 5s) $(BUILD_DIR)/playback-test
	$(if $(TIMEOUT),$(TIMEOUT) 10s) $(BUILD_DIR)/render-test
	$(if $(TIMEOUT),$(TIMEOUT) 5s) $(BUILD_DIR)/buffer-test
	$(PYTHON) tests/video.py $(BUILD_DIR)/video-test

# 格式检查需要 clang-format；Python 脚本检查本项目的额外风格规则
format-check:
	clang-format --dry-run --Werror src/*.c include/*.h tests/*.c
	$(PYTHON) tests/style.py

# 只删除本目录确定会生成的文件，不触碰媒体文件或其他目录
clean:
	rm -f $(OBJECTS) $(DEPENDS) $(PROGRAM) $(TESTS)

help:
	@echo "用法: make [目标] [变量=值]"
	@echo "目标:"
	@echo "  all           编译 $(PROGRAM)（默认）"
	@echo "  test          编译并运行单元测试"
	@echo "  format-check  clang-format 与风格脚本检查"
	@echo "  clean         删除 $(BUILD_DIR)/ 中的目标文件与可执行文件"
	@echo "变量: CC=$(CC)  OPT='$(OPT)'  WERROR='$(WERROR)'  BUILD_DIR=$(BUILD_DIR)"

-include $(DEPENDS)
