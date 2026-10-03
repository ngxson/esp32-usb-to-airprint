// WiFi station using the credentials from .env
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Starts connecting in the background (reconnects forever).
esp_err_t wifi_start(const char *hostname);
bool wifi_connected(void);
