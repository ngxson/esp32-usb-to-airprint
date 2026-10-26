// WiFi station using the credentials from .env
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Starts connecting in the background (reconnects forever).
esp_err_t wifi_start(const char *hostname);
bool wifi_connected(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);
