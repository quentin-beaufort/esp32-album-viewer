#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    NET_CONNECTING,   /* first attempt since boot */
    NET_CONNECTED,    /* associated and got an IP */
    NET_DISCONNECTED, /* lost or refused, retrying */
} net_state_t;

typedef struct {
    net_state_t state;
    uint32_t ip;      /* IPv4 in network order, 0 when not connected */
    int last_reason;  /* wifi_err_reason_t of the last disconnection, 0 if none */
    int disconnects;  /* since boot */
} net_status_t;

/* Connect to Wi-Fi (station, power save off, automatic reconnect) and announce
 * <CONFIG_NP_HOSTNAME>.local over mDNS. Returns once the stack is started, before the IP. */
esp_err_t net_start(void);

/* RSSI of the access point in dBm, or 0 when not connected. */
int net_rssi(void);

/* Current Wi-Fi state, for the diagnostic screen. */
void net_get(net_status_t *out);
