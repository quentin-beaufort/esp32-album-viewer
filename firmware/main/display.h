#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);

/*
 * The screen shows either the image or the diagnostic text. The diagnostic shows from boot
 * until the first image, /off or its timeout; after that, touching the screen shows it or
 * hides it again. While it shows, images are decoded but kept aside.
 */

/* Decode a baseline 800x480 JPEG into a hidden frame buffer, switch to it on the next VSYNC
 * and turn the backlight on. Returns ESP_ERR_INVALID_SIZE when the image is not 800x480. */
esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len);

/* No image any more: backlight off, unless the diagnostic shows. Ends the boot diagnostic. */
void display_off(void);

/* True while the diagnostic text should show. */
bool display_diag_active(void);

/* Shows the diagnostic when it is hidden and hides it otherwise. */
void display_toggle_diag(void);

/* Hides the diagnostic if it is still the one shown at boot (not opened by a touch). */
void display_end_boot_diag(void);

/* While the diagnostic is active: lets `paint` draw a whole 800x480 RGB565 frame buffer, shows
 * it and turns the backlight on. ESP_ERR_INVALID_STATE when it is hidden. */
esp_err_t display_show_diag(void (*paint)(uint16_t *fb, void *ctx), void *ctx);
