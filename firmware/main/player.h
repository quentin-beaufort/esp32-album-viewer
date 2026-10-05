#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define PLAYER_TRACK_ID_MAX 64

typedef struct {
    char track_id[PLAYER_TRACK_ID_MAX + 1]; /* empty when nothing is shown */
    bool playing;
} player_status_t;

esp_err_t player_init(void);

/* A new frame is on screen: remember its track, paused until the next state. */
void player_new_track(const char *track_id);

/* Returns false when track_id is not the track on screen (the phone must resend /frame). */
bool player_update(const char *track_id, bool playing);

/* Forget the current track. */
void player_off(void);

void player_get(player_status_t *out);
