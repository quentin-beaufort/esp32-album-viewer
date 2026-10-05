#include "display.h"

#include <string.h>
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
static SemaphoreHandle_t s_lock; /* guards s_front, s_opts, s_bar_px and frame buffer writes */
static SemaphoreHandle_t s_vsync;
static display_frame_opts_t s_opts;
static int s_bar_px = -1;        /* filled width currently drawn, -1 when unknown */
static bool s_has_frame;
static bool s_backlight;

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static inline uint16_t rgb565(uint32_t rgb)
{
    return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

/* Portrait (px, py) to the index of the physical pixel, for the rotation the phone applied. */
static inline size_t fb_index(int rotation, int px, int py)
{
    int x, y;
    if (rotation == 270) {
        x = py;
        y = BOARD_LCD_V_RES - 1 - px;
    } else {
        x = BOARD_LCD_H_RES - 1 - py;
        y = px;
    }
    return (size_t)y * BOARD_LCD_H_RES + x;
}

static void draw_bar(uint16_t *fb, const display_frame_opts_t *opts, int filled_px)
{
    uint16_t fg = rgb565(opts->bar_fg);
    uint16_t bg = rgb565(opts->bar_bg);
    for (int py = BAR_Y; py < BAR_Y + BAR_H; py++) {
        for (int px = BAR_X; px < BAR_X + BAR_W; px++) {
            fb[fb_index(opts->rotation, px, py)] = (px - BAR_X < filled_px) ? fg : bg;
        }
    }
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

esp_err_t display_show_jpeg(const uint8_t *jpg, size_t len, const display_frame_opts_t *opts)
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

    s_opts = *opts;
    draw_bar(s_fb[back], &s_opts, 0);
    s_bar_px = 0;

    /* Switch right after a VSYNC so the panel does not show half of each image. */
    xSemaphoreTake(s_vsync, 0);
    xSemaphoreTake(s_vsync, pdMS_TO_TICKS(100));
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb[back]);
    s_front = back;
    s_has_frame = true;
    if (!s_backlight) {
        board_backlight(true);
        s_backlight = true;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "frame shown, %u bytes, decoded in %lld ms", (unsigned)len, (t1 - t0) / 1000);
    return ESP_OK;
}

void display_set_progress(float fraction)
{
    if (fraction < 0) {
        fraction = 0;
    } else if (fraction > 1) {
        fraction = 1;
    }
    int filled = (int)(fraction * BAR_W + 0.5f);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_has_frame && filled != s_bar_px) {
        /* The bounce buffers are filled by the CPU through the cache, so a direct write to the
         * visible frame buffer needs no cache sync. A tear on an 8 px bar is not noticeable. */
        draw_bar(s_fb[s_front], &s_opts, filled);
        s_bar_px = filled;
    }
    xSemaphoreGive(s_lock);
}

void display_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    board_backlight(false);
    s_backlight = false;
    xSemaphoreGive(s_lock);
}
