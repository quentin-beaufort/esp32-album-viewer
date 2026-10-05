#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Progress bar rectangle, in portrait coordinates (480x800). See docs/protocol.md. */
#define BAR_X 40
#define BAR_Y 720
#define BAR_W 400
#define BAR_H 8

typedef struct {
    int rotation;      /* 90 or 270: how the phone rotated its portrait image */
    uint32_t bar_fg;   /* 0xRRGGBB */
    uint32_t bar_bg;   /* 0xRRGGBB */
} display_frame_opts_t;

esp_err_t display_init(void);

/* Decode a baseline 800x480 JPEG into the hidden frame buffer, draw an empty progress bar,
 * then switch to it on the next VSYNC and turn the backlight on.
 * Returns ESP_ERR_INVALID_SIZE when the image is not 800x480. */
esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len, const display_frame_opts_t *opts);

/* Redraw the progress bar on the visible frame buffer. fraction is clamped to [0, 1]. */
void display_set_progress(float fraction);

/* Backlight off; the next display_show_jpeg turns it back on. */
void display_off(void);
