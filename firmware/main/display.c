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
#include "sdkconfig.h"

static const char *TAG = "display";

#define FB_PIXELS (BOARD_LCD_H_RES * BOARD_LCD_V_RES)
#define FB_BYTES  (FB_PIXELS * sizeof(uint16_t))

static esp_lcd_panel_handle_t s_panel;
#define NUM_FBS   3

/* One frame buffer is scanned out, one keeps the latest image while the diagnostic shows and
 * the third is drawn into. */
static uint16_t *s_fb[NUM_FBS];
static int s_front;              /* index of the frame buffer being scanned out */
static int s_image = -1;         /* index of the frame buffer holding the latest image, or -1 */
static SemaphoreHandle_t s_lock; /* guards everything below and frame buffer writes */
static SemaphoreHandle_t s_vsync;
static bool s_backlight;
static bool s_diag = CONFIG_NP_DIAG_SCREEN; /* the diagnostic shows */
static bool s_boot_diag = CONFIG_NP_DIAG_SCREEN; /* ... and it is still the one from boot */

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
    void *fb0, *fb1, *fb2;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1, &fb2), TAG, "frame buffers");
    s_fb[0] = fb0;
    s_fb[1] = fb1;
    s_fb[2] = fb2;
    s_front = 0;

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL), TAG, "callbacks");
    return ESP_OK;
}

/* A frame buffer that is neither scanned out nor `keep`. Called with s_lock held. */
static int spare_fb(int keep)
{
    for (int i = 0; i < NUM_FBS; i++) {
        if (i != s_front && i != keep) {
            return i;
        }
    }
    return -1; /* not reached with three frame buffers */
}

static void set_backlight(bool on)
{
    if (s_backlight != on) {
        board_backlight(on);
        s_backlight = on;
    }
}

/* Shows frame buffer `back` and turns the backlight on. Called with s_lock held. */
static void present(int back)
{
    /* Switch right after a VSYNC so the panel does not show half of each image. */
    xSemaphoreTake(s_vsync, 0);
    xSemaphoreTake(s_vsync, pdMS_TO_TICKS(100));
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb[back]);
    s_front = back;
    set_backlight(true);
}

/* Hides the diagnostic: back to the image, or to a dark screen. Called with s_lock held. */
static void hide_diag(void)
{
    s_diag = false;
    s_boot_diag = false;
    if (s_image >= 0) {
        present(s_image);
    } else {
        set_backlight(false);
    }
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
    int back = spare_fb(-1);
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

    s_image = back;
    if (s_boot_diag) {
        hide_diag();
    } else if (!s_diag) {
        present(back);
    }
    bool shown = !s_diag;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "frame %s, %u bytes, decoded in %lld ms", shown ? "shown" : "kept behind the diagnostic",
             (unsigned)len, (t1 - t0) / 1000);
    return ESP_OK;
}

void display_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_image = -1;
    if (s_boot_diag || !s_diag) {
        hide_diag();
    }
    xSemaphoreGive(s_lock);
}

bool display_diag_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_diag;
    xSemaphoreGive(s_lock);
    return active;
}

void display_toggle_diag(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_diag) {
        hide_diag();
    } else {
        s_diag = true;
    }
    bool shown = s_diag;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "diagnostic %s by touch", shown ? "shown" : "hidden");
}

void display_end_boot_diag(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ended = s_boot_diag;
    if (ended) {
        hide_diag();
    }
    xSemaphoreGive(s_lock);
    if (ended) {
        ESP_LOGI(TAG, "no image yet, boot diagnostic hidden");
    }
}

esp_err_t display_show_diag(void (*paint)(uint16_t *fb, void *ctx), void *ctx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_diag) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int back = spare_fb(s_image);
    paint(s_fb[back], ctx);
    present(back);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
