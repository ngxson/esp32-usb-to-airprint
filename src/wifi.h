// WiFi station: the network set from the web installer (Improv over the COM
// port, improv_esp.c; kept in NVS) or the one in .env
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Starts connecting in the background (reconnects forever). Without a
// network the radio still starts, for wifi_scan and wifi_set_network.
esp_err_t wifi_start(const char *hostname);
bool wifi_connected(void);
// A network is set (from the installer or .env).
bool wifi_configured(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);

typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
} wifi_network_t;

// The networks in range (at most max, as the driver lists them); -1 on
// failure. Pauses the connection attempts while it scans.
int wifi_scan(wifi_network_t *out, int max);
// Joins another network: true once connected (the network is then saved),
// false after a few failed attempts or timeout_ms (back on the previous one).
bool wifi_set_network(const char *ssid, const char *password, int timeout_ms);
