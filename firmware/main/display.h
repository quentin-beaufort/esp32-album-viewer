#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);

/* Decode a baseline 800x480 JPEG into the hidden frame buffer, switch to it on the next VSYNC
 * and turn the backlight on. Returns ESP_ERR_INVALID_SIZE when the image is not 800x480. */
esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len);

/* Backlight off; the next display_show_jpeg turns it back on. */
void display_off(void);
