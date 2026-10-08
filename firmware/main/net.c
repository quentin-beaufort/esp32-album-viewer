#include "net.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define RECONNECT_MAX_DELAY_MS 30000

static int s_retry_delay_ms = 500;

/* Read by the diagnostic screen; written from the event loop only. */
static volatile net_state_t s_state = NET_CONNECTING;
static volatile uint32_t s_ip;
static volatile int s_last_reason;
static volatile int s_disconnects;

static void reconnect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(s_retry_delay_ms));
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %d ms", ev->reason, s_retry_delay_ms);
        s_state = NET_DISCONNECTED;
        s_ip = 0;
        s_last_reason = ev->reason;
        s_disconnects++;
        /* Retry from a short-lived task so the event loop is never blocked. */
        xTaskCreate(reconnect_task, "wifi_retry", 2048, NULL, 3, NULL);
        s_retry_delay_ms = s_retry_delay_ms * 2 > RECONNECT_MAX_DELAY_MS ? RECONNECT_MAX_DELAY_MS : s_retry_delay_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR ", reachable at http://%s.local", IP2STR(&ev->ip_info.ip), CONFIG_NP_HOSTNAME);
        s_retry_delay_ms = 500;
        s_ip = ev->ip_info.ip.addr;
        s_state = NET_CONNECTED;
    }
}

static esp_err_t mdns_start(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_NP_HOSTNAME), TAG, "mdns hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("Now Playing"), TAG, "mdns instance");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_nowplaying", "_tcp", 80, NULL, 0), TAG, "mdns service");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0), TAG, "mdns http");
    return ESP_OK;
}

esp_err_t net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, CONFIG_NP_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip events");

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_NP_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_NP_WIFI_PASSWORD, sizeof(cfg.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* Power save makes the radio miss multicast (mDNS) and adds latency to incoming requests. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi ps");

    return mdns_start();
}

void net_get(net_status_t *out)
{
    out->state = s_state;
    out->ip = s_ip;
    out->last_reason = s_last_reason;
    out->disconnects = s_disconnects;
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
