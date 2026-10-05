#include "player.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static char s_track[PLAYER_TRACK_ID_MAX + 1];
static bool s_playing;

esp_err_t player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void player_new_track(const char *track_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_track, track_id, sizeof(s_track));
    s_playing = false;
    xSemaphoreGive(s_lock);
}

bool player_update(const char *track_id, bool playing)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool known = s_track[0] && strcmp(s_track, track_id) == 0;
    if (known) {
        s_playing = playing;
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
    xSemaphoreGive(s_lock);
}
