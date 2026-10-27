// WiFi setup from the web installer: Improv Wi-Fi over the console UART
// (components/improv, flasher/).
#pragma once

#include "esp_err.h"

// What the installer shows and compares with its manifest's "name"
// (tools/gen_flasher.py reads it from here): the same name means "Update",
// another one "Install" (with an offer to erase the board).
#define IMPROV_FIRMWARE_NAME "ESP32 USB to AirPrint"

// Takes over the console UART's input. Call after wifi_start.
esp_err_t improv_esp_start(void);
