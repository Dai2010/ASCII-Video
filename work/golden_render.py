from pathlib import Path
import subprocess
prefix='''#define main original_program_main
#include "miku-before-modules.c"
#undef main
#include <inttypes.h>

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t bytes)
{
        const uint8_t *p = data;
        size_t i;

        for (i = 0; i < bytes; i++) {
                hash ^= p[i];
                hash *= UINT64_C(1099511628211);
        }
        return hash;
}

int main(void)
{
        GlyphAtlas atlas = {0};
        uint8_t cells[8 * 8 * 3];
        uint8_t *canvas;
        uint64_t hash;
        size_t bytes;
        size_t i;
        int widths[] = {4, 8, 16, 24, 32};
        int width;
        int height;
        int n;

        build_gamma_lut();
        for (i = 0; i < sizeof(cells); i++)
                cells[i] = (uint8_t)(i * 37 + 11);
        for (n = 0; n < 5; n++) {
                width = widths[n];
                height = width * 2;
                if (!atlas_build(&atlas, width, height))
                        return 1;
                bytes = (size_t)8 * width * 4 * height * 3;
                canvas = calloc(bytes, 1);
                if (!canvas)
                        return 1;
                hash = hash_bytes(UINT64_C(14695981039346656037),
                                  atlas.masks, width * height * FONT_CHAR_COUNT);
                hash = hash_bytes(hash, atlas.chars, sizeof(atlas.chars));
                render_frame(&atlas, cells, 8, 4, canvas, 8 * width * 3);
                hash = hash_bytes(hash, canvas, bytes);
                render_block_cells(cells, 8, 4, canvas, 8 * width * 3,
                                   width, height);
                hash = hash_bytes(hash, canvas, bytes);
                printf("%" PRIu64 "\\n", hash);
                free(canvas);
                free(atlas.masks);
        }
        return 0;
}
'''
Path('work/render-baseline.c').write_text(prefix)
subprocess.run(['gcc','-O2','-std=c17','-o','work/render-baseline','work/render-baseline.c','-lm'],check=True)
result=subprocess.check_output(['work/render-baseline'],text=True)
Path('work/render-golden.txt').write_text(result)
expected=[int(x) for x in result.splitlines()]
test=prefix.replace('#define main original_program_main\n#include "miku-before-modules.c"\n#undef main', '#include "glyph.h"\n#include "render.h"\n#include "util.h"\n#include <assert.h>\n#include <stdlib.h>').replace('GlyphAtlas atlas','struct glyph_atlas atlas').replace('build_gamma_lut();','miku_build_gamma_lut();').replace('if (!atlas_build(&atlas, width, height))','if (miku_atlas_build(&atlas, width, height) != 0)').replace('        int n;','        int n;\n        struct render_grid grid = {0};\n        const uint64_t expected[] = {'+','.join('UINT64_C('+str(v)+')' for v in expected)+'};')
test=test.replace('render_frame(&atlas, cells, 8, 4, canvas, 8 * width * 3);','''grid = (struct render_grid) {
                        .cells = cells, .output = canvas,
                        .stride = 8 * width * 3, .cols = 8, .rows = 4,
                        .cell_w = width, .cell_h = height,
                };
                miku_render_frame(&atlas, &grid);''').replace('render_block_cells(cells, 8, 4, canvas, 8 * width * 3,\n                                   width, height);','miku_render_block_cells(&grid);').replace('printf("%" PRIu64 "\\n", hash);','assert(hash == expected[n]);').replace('        return 0;\n}\n','        puts("render equivalence: PASS");\n        return 0;\n}\n')
Path('tests/render.c').write_text('/* SPDX-License-Identifier: GPL-3.0-only */\n/* Copyright (C) 2026 Kuimuztion */\n/* 五种字形尺寸与字符/色块渲染的基线校验，期望值来自重构前实现。 */\n'+test)
print(result)
