#include "api.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"
#include "player.h"
#include "sdkconfig.h"

static const char *TAG = "api";

#define MAX_JPEG_BYTES (CONFIG_NP_MAX_JPEG_KB * 1024)
#define MAX_STATE_BYTES 512

static uint8_t *s_jpeg; /* receive buffer in PSRAM, reused for every frame */

/* Compare without returning early, so response time does not leak the token. */
static bool token_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < la && i < lb; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t send_no_content(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Returns true when the request carries the right X-Token; otherwise sends the error. */
static bool authorized(httpd_req_t *req)
{
    if (CONFIG_NP_TOKEN[0] == '\0') {
        send_status(req, "503 Service Unavailable", "no token configured on the device");
        return false;
    }
    char token[128];
    if (httpd_req_get_hdr_value_str(req, "X-Token", token, sizeof(token)) != ESP_OK ||
        !token_equal(token, CONFIG_NP_TOKEN)) {
        send_status(req, "401 Unauthorized", "bad token");
        return false;
    }
    return true;
}

static uint32_t header_color(httpd_req_t *req, const char *name, uint32_t fallback)
{
    char buf[16];
    if (httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf)) != ESP_OK) {
        return fallback;
    }
    const char *hex = buf[0] == '#' ? buf + 1 : buf;
    char *end;
    unsigned long v = strtoul(hex, &end, 16);
    return (strlen(hex) == 6 && *end == '\0') ? (uint32_t)v : fallback;
}

static esp_err_t recv_body(httpd_req_t *req, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    return ESP_OK;
}

static esp_err_t frame_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char track_id[PLAYER_TRACK_ID_MAX + 1];
    if (httpd_req_get_hdr_value_str(req, "X-Track-Id", track_id, sizeof(track_id)) != ESP_OK || !track_id[0]) {
        return send_status(req, "400 Bad Request", "missing or too long X-Track-Id");
    }
    if (req->content_len == 0) {
        return send_status(req, "400 Bad Request", "empty body");
    }
    if (req->content_len > MAX_JPEG_BYTES) {
        return send_status(req, "413 Payload Too Large", "jpeg too large");
    }

    display_frame_opts_t opts = {
        .rotation = 90,
        .bar_fg = header_color(req, "X-Bar-Fg", 0xFFFFFF),
        .bar_bg = header_color(req, "X-Bar-Bg", 0x404040),
    };
    char rot[8];
    if (httpd_req_get_hdr_value_str(req, "X-Rotation", rot, sizeof(rot)) == ESP_OK && strcmp(rot, "270") == 0) {
        opts.rotation = 270;
    }

    if (recv_body(req, s_jpeg, req->content_len) != ESP_OK) {
        return ESP_FAIL; /* connection is broken, let httpd close it */
    }
    esp_err_t err = display_show_jpeg(s_jpeg, req->content_len, &opts);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_status(req, "400 Bad Request", "jpeg must be 800x480");
    } else if (err != ESP_OK) {
        return send_status(req, "400 Bad Request", "cannot decode jpeg (baseline only)");
    }
    player_new_track(track_id);
    return send_no_content(req);
}

static esp_err_t state_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > MAX_STATE_BYTES) {
        return send_status(req, "400 Bad Request", "bad body size");
    }
    char body[MAX_STATE_BYTES + 1];
    if (recv_body(req, (uint8_t *)body, req->content_len) != ESP_OK) {
        return ESP_FAIL;
    }
    body[req->content_len] = '\0';

    cJSON *json = cJSON_Parse(body);
    const cJSON *track = cJSON_GetObjectItem(json, "track_id");
    const cJSON *playing = cJSON_GetObjectItem(json, "playing");
    const cJSON *pos = cJSON_GetObjectItem(json, "position_ms");
    const cJSON *dur = cJSON_GetObjectItem(json, "duration_ms");
    const cJSON *speed = cJSON_GetObjectItem(json, "speed");
    esp_err_t ret;
    if (!cJSON_IsString(track) || !cJSON_IsBool(playing) || !cJSON_IsNumber(pos) || !cJSON_IsNumber(dur)) {
        ret = send_status(req, "400 Bad Request", "expected track_id, playing, position_ms, duration_ms");
    } else if (!player_update(track->valuestring, cJSON_IsTrue(playing), (int64_t)pos->valuedouble,
                              (int64_t)dur->valuedouble, cJSON_IsNumber(speed) ? (float)speed->valuedouble : 1.0f)) {
        ret = send_status(req, "409 Conflict", "unknown track, send /frame first");
    } else {
        ret = send_no_content(req);
    }
    cJSON_Delete(json);
    return ret;
}

static esp_err_t off_handler(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    player_off();
    display_off();
    return send_no_content(req);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    player_status_t st;
    player_get(&st);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "track_id", st.track_id);
    cJSON_AddBoolToObject(json, "playing", st.playing);
    cJSON_AddNumberToObject(json, "position_ms", (double)st.position_ms);
    cJSON_AddNumberToObject(json, "duration_ms", (double)st.duration_ms);
    cJSON_AddNumberToObject(json, "rssi", net_rssi());
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    char *out = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t api_start(void)
{
    s_jpeg = heap_caps_malloc(MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_jpeg, ESP_ERR_NO_MEM, TAG, "jpeg buffer");
    if (CONFIG_NP_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "CONFIG_NP_TOKEN is empty: every POST will be refused");
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

    const httpd_uri_t routes[] = {
        {.uri = "/frame", .method = HTTP_POST, .handler = frame_handler},
        {.uri = "/state", .method = HTTP_POST, .handler = state_handler},
        {.uri = "/off", .method = HTTP_POST, .handler = off_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    ESP_LOGI(TAG, "HTTP server on port %d", cfg.server_port);
    return ESP_OK;
}
