#include "player.h"

#include <string.h>
#include "display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static SemaphoreHandle_t s_lock;
static char s_track[PLAYER_TRACK_ID_MAX + 1];
static bool s_playing;
static int64_t s_position_ms;   /* position at s_received_us */
static int64_t s_duration_ms;
static float s_speed = 1.0f;
static int64_t s_received_us;

/* Must be called with s_lock held. */
static int64_t position_now_ms(void)
{
    int64_t pos = s_position_ms;
    if (s_playing) {
        pos += (int64_t)((esp_timer_get_time() - s_received_us) / 1000 * s_speed);
    }
    if (s_duration_ms > 0 && pos > s_duration_ms) {
        pos = s_duration_ms;
    }
    return pos < 0 ? 0 : pos;
}

static void progress_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_NP_PROGRESS_HZ);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, period);
        float fraction = -1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_track[0] && s_duration_ms > 0) {
            fraction = (float)position_now_ms() / (float)s_duration_ms;
        }
        xSemaphoreGive(s_lock);
        if (fraction >= 0) {
            display_set_progress(fraction);
        }
    }
}

esp_err_t player_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(progress_task, "progress", 3072, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void player_new_track(const char *track_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_track, track_id, sizeof(s_track));
    s_playing = false;
    s_position_ms = 0;
    s_duration_ms = 0;
    s_speed = 1.0f;
    s_received_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

bool player_update(const char *track_id, bool playing, int64_t position_ms, int64_t duration_ms, float speed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool known = s_track[0] && strcmp(s_track, track_id) == 0;
    if (known) {
        s_playing = playing;
        s_position_ms = position_ms;
        s_duration_ms = duration_ms;
        s_speed = speed;
        s_received_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    return known;
}

void player_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_track[0] = '\0';
    s_playing = false;
    xSemaphoreGive(s_lock);
}

void player_get(player_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out->track_id, s_track, sizeof(out->track_id));
    out->playing = s_playing;
    out->position_ms = s_track[0] ? position_now_ms() : 0;
    out->duration_ms = s_duration_ms;
    xSemaphoreGive(s_lock);
}
