#pragma once

#include <stdint.h>
#include "font_mono.h"

/* Text drawn in portrait (480x800, as the screen is mounted) into a landscape 800x480
 * RGB565 frame buffer. */
#define CONSOLE_COLS (480 / FONT_MONO_W)
#define CONSOLE_ROWS (800 / FONT_MONO_H)

#define CONSOLE_WHITE  0xFFFF
#define CONSOLE_GREY   0x9CF3
#define CONSOLE_GREEN  0x47E8
#define CONSOLE_YELLOW 0xFFE0
#define CONSOLE_RED    0xF9E7

void console_clear(uint16_t *fb);

/* Prints UTF-8 text from the start of `row`, wrapping at CONSOLE_COLS. Characters outside
 * Latin-1 show as '?'. Returns the number of rows the text takes; nothing is drawn past
 * CONSOLE_ROWS. */
int console_print(uint16_t *fb, int row, uint16_t color, const char *text);

/* Number of rows console_print would use for `text`. */
int console_rows(const char *text);
