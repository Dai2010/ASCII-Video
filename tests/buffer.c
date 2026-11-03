/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Kuimuztion */

#include "strbuf.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 覆盖空文本、正常追加及长度溢出，溢出不得破坏已经取得的存储。
 * 粘性错误必须阻止后续追加，避免界面输出看似成功的半帧。
 * 所有资源由本测试释放，不启动外部进程或访问用户配置。
 */
int main(void)
{
	struct strbuf buffer = {0};
	char *original;
	size_t capacity;

	assert(miku_sb_addf(&buffer, "%s", "") == 0);
	assert(buffer.len == 0);
	assert(miku_sb_addf(&buffer, "%s %d", "测试", 42) == 0);
	assert(strcmp(buffer.buf, "测试 42") == 0);
	original = buffer.buf;
	capacity = buffer.cap;
	assert(miku_sb_reserve(&buffer, SIZE_MAX) == -EOVERFLOW);
	assert(buffer.buf == original);
	assert(buffer.cap == capacity);
	assert(strcmp(buffer.buf, "测试 42") == 0);
	assert(miku_sb_addf(&buffer, "%s", "ignored") == -EOVERFLOW);
	free(buffer.buf);
	puts("buffer boundaries: PASS");
	return 0;
}
