#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

/* Initialise the I2C bus, the CH422G IO expander and the RGB panel (three frame buffers in
 * PSRAM). The backlight stays off. */
esp_err_t board_init(esp_lcd_panel_handle_t *panel);

esp_err_t board_backlight(bool on);

/* Resets the GT911 touch controller so that it answers at 0x5D and returns it as a device on
 * the shared I2C bus. Call after board_init. */
esp_err_t board_touch_init(i2c_master_dev_handle_t *dev);
