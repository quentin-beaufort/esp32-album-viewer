#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

/* Initialise the I2C bus, the CH422G IO expander and the RGB panel (two frame buffers in PSRAM).
 * The backlight stays off. */
esp_err_t board_init(esp_lcd_panel_handle_t *panel);

esp_err_t board_backlight(bool on);
