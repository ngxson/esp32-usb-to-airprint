// WiFi: the station on the saved network, or the setup network (an access
// point with a captive portal) to choose one.
//
// The network comes from the setup page (kept in NVS), else from .env
// (WIFI_SSID). With neither, the board opens the setup network
// "Printer-Setup-XXXX" (the end of its MAC address, the same for every
// printer and build): join it, the phone shows the page (or open
// http://192.168.4.1/), pick the network, type its password; the board joins
// it, saves it and restarts. It also opens after wifi_forget(), and, when
// SETUP_PASSWORD protects it, after CONFIG_WIFI_SETUP_AFTER_MIN minutes
// without the network (then it keeps trying the saved one and restarts when
// it is back).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Starts connecting in the background (reconnects forever), or the setup
// network. busy() (optional): no restart into the setup network while it
// returns true (printing, scanning).
esp_err_t wifi_start(const char *hostname, bool (*busy)(void));
bool wifi_connected(void);
// The setup network's name while it runs, else NULL.
const char *wifi_setup_ap(void);
// The setup page's address (192.168.4.1) while it runs, else NULL.
const char *wifi_setup_ip(void);
// Forgets the saved network and restarts into the setup network.
void wifi_forget(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);
