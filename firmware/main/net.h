#pragma once

#include "esp_err.h"

/* Connect to Wi-Fi (station, power save off, automatic reconnect) and announce
 * <CONFIG_NP_HOSTNAME>.local over mDNS. Returns once the stack is started, before the IP. */
esp_err_t net_start(void);

/* RSSI of the access point in dBm, or 0 when not connected. */
int net_rssi(void);
