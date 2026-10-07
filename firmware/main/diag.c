#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "console.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "sdkconfig.h"

static const char *TAG = "diag";

#define REFRESH_MS 1000
#define LOG_LINES  32
#define LOG_LEN    120
#define MAX_LINES  16

/* Latest log lines, also kept while the screen is off so they can be shown again later. */
static char s_log[LOG_LINES][LOG_LEN];
static unsigned s_log_count;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_http_ready;
static unsigned s_requests, s_failures;
static char s_last_req[48];
static int s_last_status;
static int64_t s_last_req_us;

typedef struct {
    uint16_t color;
    char text[160];
} line_t;

/* What the next refresh draws, built before taking the display lock. */
static struct {
    line_t lines[MAX_LINES];
    int count;
    char log[LOG_LINES][LOG_LEN];
    int log_count;
} s_frame;

static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char raw[LOG_LEN];
    vsnprintf(raw, sizeof(raw), fmt, copy);
    va_end(copy);

    /* Drop colour escapes and the trailing newline. */
    char clean[LOG_LEN];
    size_t n = 0;
    for (const char *p = raw; *p && n < sizeof(clean) - 1; p++) {
        if (*p == '\033') {
            while (*p && *p != 'm') {
                p++;
            }
            if (!*p) {
                break;
            }
            continue;
        }
        if (*p != '\n' && *p != '\r') {
            clean[n++] = *p;
        }
    }
    clean[n] = '\0';
    if (n) {
        portENTER_CRITICAL(&s_log_mux);
        memcpy(s_log[s_log_count % LOG_LINES], clean, n + 1);
        s_log_count++;
        portEXIT_CRITICAL(&s_log_mux);
    }
    return s_prev_vprintf(fmt, args);
}

const char *diag_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "mise sous tension";
    case ESP_RST_EXT: return "bouton reset";
    case ESP_RST_SW: return "redémarrage logiciel";
    case ESP_RST_PANIC: return "plantage du firmware";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "chien de garde (blocage)";
    case ESP_RST_DEEPSLEEP: return "réveil";
    case ESP_RST_BROWNOUT: return "baisse de tension (alimentation)";
    case ESP_RST_PWR_GLITCH: return "parasite d'alimentation";
    case ESP_RST_USB: return "reset par l'USB";
    case ESP_RST_JTAG: return "reset JTAG";
    default: return "inconnue";
    }
}

/* The disconnection reasons worth explaining; see wifi_err_reason_t. */
static const char *wifi_reason(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "réseau introuvable";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "mot de passe refusé ?";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal perdu";
    case WIFI_REASON_ASSOC_LEAVE: return "déconnexion volontaire";
    case WIFI_REASON_CONNECTION_FAIL: return "connexion échouée";
    default: return "voir wifi_err_reason_t";
    }
}

static bool bad_reset(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
        return true;
    default:
        return false;
    }
}

void diag_http_ready(bool ready)
{
    s_http_ready = ready;
}

void diag_note_request(const char *method, const char *uri, int status)
{
    portENTER_CRITICAL(&s_req_mux);
    s_requests++;
    if (status == 0 || status >= 400) {
        s_failures++;
    }
    snprintf(s_last_req, sizeof(s_last_req), "%s %s", method, uri);
    s_last_status = status;
    s_last_req_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_req_mux);
}

static void add(uint16_t color, const char *fmt, ...)
{
    if (s_frame.count == MAX_LINES) {
        return;
    }
    line_t *line = &s_frame.lines[s_frame.count++];
    line->color = color;
    va_list args;
    va_start(args, fmt);
    vsnprintf(line->text, sizeof(line->text), fmt, args);
    va_end(args);
}

static void build(void)
{
    s_frame.count = 0;
    int64_t now = esp_timer_get_time();
    int up = (int)(now / 1000000);

    add(CONSOLE_WHITE, "Now Playing - diagnostic");
    add(CONSOLE_GREY, "------------------------------------------------");
    add(bad_reset() ? CONSOLE_RED : CONSOLE_WHITE, "Démarrage : %s", diag_reset_reason());
    add(CONSOLE_WHITE, "Allumé depuis : %d min %02d s", up / 60, up % 60);

    net_status_t net;
    net_get(&net);
    if (net.state == NET_CONNECTED) {
        esp_ip4_addr_t ip = {.addr = net.ip};
        add(CONSOLE_GREEN, "Wi-Fi : connecté à \"%s\", %d dBm", CONFIG_NP_WIFI_SSID, net_rssi());
        add(CONSOLE_GREEN, "IP : " IPSTR, IP2STR(&ip));
    } else if (net.state == NET_CONNECTING) {
        add(CONSOLE_YELLOW, "Wi-Fi : connexion à \"%s\"...", CONFIG_NP_WIFI_SSID);
    } else {
        add(CONSOLE_RED, "Wi-Fi : déconnecté de \"%s\" (raison %d : %s), nouvel essai", CONFIG_NP_WIFI_SSID,
            net.last_reason, wifi_reason(net.last_reason));
    }
    if (net.disconnects) {
        add(CONSOLE_YELLOW, "Coupures Wi-Fi depuis le démarrage : %d", net.disconnects);
    }
    add(CONSOLE_WHITE, "Adresse : http://%s.local/status", CONFIG_NP_HOSTNAME);
    add(s_http_ready ? CONSOLE_GREEN : CONSOLE_YELLOW, "Serveur HTTP : %s",
        s_http_ready ? "prêt, port 80" : "pas encore démarré");
    if (CONFIG_NP_TOKEN[0] == '\0') {
        add(CONSOLE_RED, "Jeton : absent, à définir dans menuconfig");
    }

    char last[48];
    int status;
    unsigned requests, failures;
    int64_t last_us;
    portENTER_CRITICAL(&s_req_mux);
    strlcpy(last, s_last_req, sizeof(last));
    status = s_last_status;
    requests = s_requests;
    failures = s_failures;
    last_us = s_last_req_us;
    portEXIT_CRITICAL(&s_req_mux);
    if (requests == 0) {
        add(CONSOLE_YELLOW, "Requêtes : aucune reçue");
    } else {
        add(failures ? CONSOLE_YELLOW : CONSOLE_WHITE, "Requêtes : %u, dont %u en erreur", requests, failures);
        char result[24];
        if (status) {
            snprintf(result, sizeof(result), "%d", status);
        } else {
            strlcpy(result, "connexion coupée", sizeof(result));
        }
        add(CONSOLE_WHITE, "Dernière : %s -> %s, il y a %d s", last, result, (int)((now - last_us) / 1000000));
    }
    add(CONSOLE_WHITE, "Mémoire libre : interne %u Ko, PSRAM %u Ko",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    add(CONSOLE_GREY, "------------------------------------------------");

    portENTER_CRITICAL(&s_log_mux);
    unsigned total = s_log_count;
    unsigned first = total > LOG_LINES ? total - LOG_LINES : 0;
    s_frame.log_count = (int)(total - first);
    for (unsigned i = first; i < total; i++) {
        memcpy(s_frame.log[i - first], s_log[i % LOG_LINES], LOG_LEN);
    }
    portEXIT_CRITICAL(&s_log_mux);
}

static void paint(uint16_t *fb, void *ctx)
{
    console_clear(fb);
    int row = 0;
    for (int i = 0; i < s_frame.count; i++) {
        row += console_print(fb, row, s_frame.lines[i].color, s_frame.lines[i].text);
    }

    /* Newest log lines at the bottom, as many as fit. */
    int first = s_frame.log_count, rows = 0;
    while (first > 0) {
        int need = console_rows(s_frame.log[first - 1]);
        if (row + rows + need > CONSOLE_ROWS) {
            break;
        }
        rows += need;
        first--;
    }
    for (int i = first; i < s_frame.log_count; i++) {
        const char *text = s_frame.log[i];
        uint16_t color = text[0] == 'E' ? CONSOLE_RED : text[0] == 'W' ? CONSOLE_YELLOW : CONSOLE_GREY;
        row += console_print(fb, row, color, text);
    }
}

static void diag_task(void *arg)
{
    int64_t end_us = CONFIG_NP_DIAG_SECONDS > 0 ? (int64_t)CONFIG_NP_DIAG_SECONDS * 1000000 : INT64_MAX;
    while (display_diag_active()) {
        if (esp_timer_get_time() >= end_us) {
            ESP_LOGI(TAG, "no image after %d s, screen off", CONFIG_NP_DIAG_SECONDS);
            display_off();
            break;
        }
        build();
        display_show_diag(paint, NULL);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
    vTaskDelete(NULL);
}

void diag_start(void)
{
#if CONFIG_NP_DIAG_SCREEN
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    ESP_LOGW(TAG, "reset reason: %s", diag_reset_reason());
    xTaskCreate(diag_task, "diag", 4096, NULL, 2, NULL);
#else
    ESP_LOGI(TAG, "reset reason: %s", diag_reset_reason());
    display_off();
#endif
}
