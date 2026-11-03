#define main original_program_main
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
                printf("%" PRIu64 "\n", hash);
                free(canvas);
                free(atlas.masks);
        }
        return 0;
}
