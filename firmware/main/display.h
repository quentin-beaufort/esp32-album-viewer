#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);

/* Decode a baseline 800x480 JPEG into the hidden frame buffer, switch to it on the next VSYNC
 * and turn the backlight on. Returns ESP_ERR_INVALID_SIZE when the image is not 800x480. */
esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len);

/* Backlight off; the next display_show_jpeg turns it back on. Ends the diagnostic screen. */
void display_off(void);

/* True from boot until the first image is shown or display_off is called. */
bool display_diag_active(void);

/* While the diagnostic screen is active: lets `paint` draw a whole 800x480 RGB565 frame
 * buffer, shows it and turns the backlight on. ESP_ERR_INVALID_STATE once it has ended. */
esp_err_t display_show_diag(void (*paint)(uint16_t *fb, void *ctx), void *ctx);
