#include "display.h"

#include <stdbool.h>
#include "board.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"

static const char *TAG = "display";

#define FB_PIXELS (BOARD_LCD_H_RES * BOARD_LCD_V_RES)
#define FB_BYTES  (FB_PIXELS * sizeof(uint16_t))

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];
static int s_front;              /* index of the frame buffer being scanned out */
static SemaphoreHandle_t s_lock; /* guards s_front, s_backlight and frame buffer writes */
static SemaphoreHandle_t s_vsync;
static bool s_backlight;

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_vsync = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lock && s_vsync, ESP_ERR_NO_MEM, TAG, "semaphores");

    ESP_RETURN_ON_ERROR(board_init(&s_panel), TAG, "board");
    void *fb0, *fb1;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1), TAG, "frame buffers");
    s_fb[0] = fb0;
    s_fb[1] = fb1;
    s_front = 0;

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL), TAG, "callbacks");
    return ESP_OK;
}

esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    ESP_RETURN_ON_ERROR(esp_jpeg_get_image_info(&cfg, &info), TAG, "jpeg header");
    if (info.width != BOARD_LCD_H_RES || info.height != BOARD_LCD_V_RES) {
        ESP_LOGW(TAG, "rejecting %ux%u image", info.width, info.height);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int back = 1 - s_front;
    int64_t t0 = esp_timer_get_time();
    cfg.outbuf = (uint8_t *)s_fb[back];
    cfg.outbuf_size = FB_BYTES;
    cfg.priv.read = 0;
    esp_err_t err = esp_jpeg_decode(&cfg, &info);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return err;
    }
    int64_t t1 = esp_timer_get_time();

    /* Switch right after a VSYNC so the panel does not show half of each image. */
    xSemaphoreTake(s_vsync, 0);
    xSemaphoreTake(s_vsync, pdMS_TO_TICKS(100));
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb[back]);
    s_front = back;
    if (!s_backlight) {
        board_backlight(true);
        s_backlight = true;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "frame shown, %u bytes, decoded in %lld ms", (unsigned)len, (t1 - t0) / 1000);
    return ESP_OK;
}

void display_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    board_backlight(false);
    s_backlight = false;
    xSemaphoreGive(s_lock);
}
