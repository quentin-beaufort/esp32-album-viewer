#include "api.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "diag.h"
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
#define MAX_RECV_TIMEOUTS 2 /* each one is cfg.recv_wait_timeout seconds without data */

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

static const char *method_name(httpd_req_t *req)
{
    return req->method == HTTP_POST ? "POST" : req->method == HTTP_GET ? "GET" : "?";
}

static esp_err_t send_status(httpd_req_t *req, const char *status, const char *msg)
{
    diag_note_request(method_name(req), req->uri, atoi(status));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t send_no_content(httpd_req_t *req)
{
    diag_note_request(method_name(req), req->uri, 204);
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

/* Gives up when the phone stops sending: a lost connection must not block the server, which
 * handles one request at a time. */
static esp_err_t recv_body(httpd_req_t *req, uint8_t *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "%s: body cut after %u/%u bytes", req->uri, (unsigned)got, (unsigned)len);
            diag_note_request(method_name(req), req->uri, 0);
            return ESP_FAIL;
        }
        timeouts = 0;
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

    int64_t t0 = esp_timer_get_time();
    if (recv_body(req, s_jpeg, req->content_len) != ESP_OK) {
        return ESP_FAIL; /* connection is broken, let httpd close it */
    }
    ESP_LOGI(TAG, "/frame %s: %u bytes received in %lld ms", track_id, (unsigned)req->content_len,
             (esp_timer_get_time() - t0) / 1000);
    esp_err_t err = display_show_jpeg(s_jpeg, req->content_len);
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
    esp_err_t ret;
    if (!cJSON_IsString(track) || !cJSON_IsBool(playing)) {
        ret = send_status(req, "400 Bad Request", "expected track_id and playing");
    } else if (!player_update(track->valuestring, cJSON_IsTrue(playing))) {
        ESP_LOGI(TAG, "/state for unknown track %s", track->valuestring);
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
    ESP_LOGI(TAG, "/off");
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
    cJSON_AddNumberToObject(json, "rssi", net_rssi());
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(json, "reset_reason", diag_reset_reason());
    net_status_t net;
    net_get(&net);
    cJSON_AddNumberToObject(json, "wifi_disconnects", net.disconnects);
    cJSON_AddNumberToObject(json, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    char *out = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    diag_note_request("GET", req->uri, 200);
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
        {.uri = "/", .method = HTTP_GET, .handler = status_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    ESP_LOGI(TAG, "HTTP server on port %d", cfg.server_port);
    diag_http_ready(true);
    return ESP_OK;
}
