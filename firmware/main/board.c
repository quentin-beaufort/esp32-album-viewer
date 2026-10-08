/*
 * Waveshare ESP32-S3-Touch-LCD-4.3B: pins and timings from Waveshare's ESP-IDF example
 * (examples/ESP-IDF/09_lvgl_v9_demo/components/waveshare_rgb_lcd_port.*), ported to the
 * i2c_master driver. The GT911 touch controller shares the I2C bus, see touch.c.
 */
#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define I2C_SDA           8
#define I2C_SCL           9
#define I2C_FREQ_HZ       400000

/* The CH422G has no real I2C address: each register is addressed as its own device. */
#define CH422G_ADDR_MODE  0x24
#define CH422G_ADDR_OUT   0x38
#define CH422G_MODE_IO_OE 0x01

/* CH422G outputs: IO1 touch reset, IO2 backlight, IO3 LCD reset, IO4 SD card CS. */
#define EXIO_TP_RST       (1 << 1)
#define EXIO_BL           (1 << 2)
#define EXIO_LCD_RST      (1 << 3)
#define EXIO_SD_CS        (1 << 4)
#define EXIO_IDLE         (EXIO_TP_RST | EXIO_LCD_RST | EXIO_SD_CS)

#define TOUCH_INT         4
#define GT911_ADDR        0x5D /* chosen by holding INT low while the reset is released */

#define LCD_PCLK_HZ       (16 * 1000 * 1000)
#define LCD_BOUNCE_LINES  10

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exio_mode;
static i2c_master_dev_handle_t s_exio_out;
static uint8_t s_exio_state = EXIO_IDLE;

static esp_err_t exio_write(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, 100);
}

static esp_err_t io_expander_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    dev_cfg.device_address = CH422G_ADDR_MODE;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_exio_mode), TAG, "ch422g mode");
    dev_cfg.device_address = CH422G_ADDR_OUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_exio_out), TAG, "ch422g out");

    ESP_RETURN_ON_ERROR(exio_write(s_exio_mode, CH422G_MODE_IO_OE), TAG, "ch422g output mode");
    return exio_write(s_exio_out, s_exio_state);
}

static esp_err_t exio_set(uint8_t bit, bool on)
{
    s_exio_state = on ? (s_exio_state | bit) : (s_exio_state & ~bit);
    return exio_write(s_exio_out, s_exio_state);
}

esp_err_t board_backlight(bool on)
{
    return exio_set(EXIO_BL, on);
}

esp_err_t board_touch_init(i2c_master_dev_handle_t *dev)
{
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "touch int");
    gpio_set_level(TOUCH_INT, 0);
    ESP_RETURN_ON_ERROR(exio_set(EXIO_TP_RST, false), TAG, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(exio_set(EXIO_TP_RST, true), TAG, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, dev);
}

esp_err_t board_init(esp_lcd_panel_handle_t *panel)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "io expander");

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 3,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * LCD_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40},
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");
    ESP_LOGI(TAG, "RGB panel %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}
