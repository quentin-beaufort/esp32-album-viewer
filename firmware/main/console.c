#include "console.h"

#include <string.h>
#include "board.h"

static void draw_glyph(uint16_t *fb, int col, int row, uint32_t cp, const uint16_t shades[16])
{
    int index;
    if (cp >= 0x20 && cp < 0x7F) {
        index = cp - 0x20;
    } else if (cp >= 0xA0 && cp <= 0xFF) {
        index = 95 + cp - 0xA0;
    } else {
        index = '?' - 0x20;
    }
    const uint8_t *glyph = font_mono_glyphs[index];
    int px0 = col * FONT_MONO_W, py0 = row * FONT_MONO_H;
    for (int gy = 0; gy < FONT_MONO_H; gy++) {
        for (int gx = 0; gx < FONT_MONO_W; gx++) {
            int i = gy * FONT_MONO_W + gx;
            uint8_t alpha = (i & 1) ? glyph[i / 2] & 0x0F : glyph[i / 2] >> 4;
            if (alpha) {
                /* Portrait (px, py) is landscape (799 - py, px): the image is turned 90° clockwise. */
                int x = BOARD_LCD_H_RES - 1 - (py0 + gy);
                int y = px0 + gx;
                fb[y * BOARD_LCD_H_RES + x] = shades[alpha];
            }
        }
    }
}

/* Decodes one UTF-8 character; invalid bytes come out as themselves. */
static uint32_t next_cp(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp = *p++;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) {
        cp &= 0x3F >> extra;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
    }
    *s = (const char *)p;
    return cp;
}

void console_clear(uint16_t *fb)
{
    memset(fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
}

static int layout(uint16_t *fb, int row, uint16_t color, const char *text)
{
    uint16_t shades[16];
    if (fb) {
        /* Blend each channel towards the black background. */
        int r = color >> 11, g = (color >> 5) & 0x3F, b = color & 0x1F;
        for (int a = 0; a < 16; a++) {
            shades[a] = (uint16_t)(((r * a / 15) << 11) | ((g * a / 15) << 5) | (b * a / 15));
        }
    }
    int col = 0, rows = 1;
    while (*text) {
        uint32_t cp = next_cp(&text);
        if (cp == '\n') {
            col = 0;
            rows++;
            continue;
        }
        if (col == CONSOLE_COLS) {
            col = 0;
            rows++;
        }
        int r = row + rows - 1;
        if (fb && r >= 0 && r < CONSOLE_ROWS && cp != ' ') {
            draw_glyph(fb, col, r, cp, shades);
        }
        col++;
    }
    return rows;
}

int console_print(uint16_t *fb, int row, uint16_t color, const char *text)
{
    return layout(fb, row, color, text);
}

int console_rows(const char *text)
{
    return layout(NULL, 0, 0, text);
}
