/*
 * GT911 touch controller, read by polling over I2C: only "a finger came down" matters, so the
 * coordinates are ignored. Registers from the GT911 programming guide.
 */
#include "touch.h"

#include "board.h"
#include "diag.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define REG_PRODUCT_ID   0x8140
#define REG_STATUS       0x814E
#define STATUS_READY     0x80
#define STATUS_POINTS    0x0F

#define POLL_MS          30
#define RELEASE_US       (150 * 1000) /* no touch report for this long: the finger is up */
#define DEBOUNCE_US      (400 * 1000) /* ignore a new touch this soon after a toggle */

static i2c_master_dev_handle_t s_dev;

static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len, 50);
}

static esp_err_t clear_status(void)
{
    uint8_t cmd[3] = {REG_STATUS >> 8, REG_STATUS & 0xFF, 0};
    return i2c_master_transmit(s_dev, cmd, sizeof(cmd), 50);
}

static void touch_task(void *arg)
{
    bool down = false;
    int64_t last_touch_us = 0, last_toggle_us = -DEBOUNCE_US;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint8_t status;
        if (read_reg(REG_STATUS, &status, 1) != ESP_OK) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (status & STATUS_READY) {
            clear_status();
            if (status & STATUS_POINTS) {
                if (!down && now - last_toggle_us >= DEBOUNCE_US) {
                    display_toggle_diag();
                    diag_refresh();
                    last_toggle_us = now;
                }
                down = true;
                last_touch_us = now;
                continue;
            }
            down = false;
        }
        if (down && now - last_touch_us >= RELEASE_US) {
            down = false;
        }
    }
}

esp_err_t touch_start(void)
{
    char id[5] = {0};
    if (board_touch_init(&s_dev) != ESP_OK || read_reg(REG_PRODUCT_ID, (uint8_t *)id, 4) != ESP_OK) {
        ESP_LOGW(TAG, "GT911 does not answer, touch disabled");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "GT911 ready (product id %s), touch the screen to show or hide the diagnostic", id);
    clear_status();
    return xTaskCreate(touch_task, "touch", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
