// The WiFi setup network's captive portal: the page, its form and the DNS
// server that sends every name to the board. Portable: the platform runs the
// access point, the sockets and the station (src/wifi.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SETUP_MAX_NETWORKS 20

typedef struct {
    char ssid[33];
    int rssi;                   // dBm
    bool secure;                // needs a password
} wifi_setup_network_t;

typedef enum {
    WIFI_SETUP_IDLE,            // the form
    WIFI_SETUP_CONNECTING,      // trying the network from the form
    WIFI_SETUP_FAILED,          // it did not work: error, and the form again
    WIFI_SETUP_CONNECTED,       // joined and saved: the board restarts
} wifi_setup_state_t;

typedef struct {
    const char *printer_name;
    const char *hostname;       // the printer's address afterwards: http://<hostname>.local/
    const wifi_setup_network_t *networks;   // strongest first
    int n_networks;
    wifi_setup_state_t state;
    const char *ssid;           // the network tried (connecting, failed, connected)
    const char *error;          // failed: why
    const char *ip;             // connected: the board's address on that network
} wifi_setup_page_t;

// The setup page (HTML) into out; returns its length. Cut short (still a
// string) if it does not fit in n bytes.
size_t wifi_setup_page(const wifi_setup_page_t *p, char *out, size_t n);

// The value of key in an application/x-www-form-urlencoded body, decoded
// into out (n bytes with the NUL). Returns its length, -1 when the key is
// missing ("" in out), -2 when the value does not fit.
int wifi_setup_form_value(const char *body, size_t len, const char *key, char *out, size_t n);

// A password the station can use: empty (open network), 8 to 63 characters
// or 64 hex digits.
bool wifi_setup_password_ok(const char *password);

// The reply to a DNS query: every A question is answered with ip (network
// order), other types with no records. Returns the reply's length, 0 for a
// packet to ignore (not a query, malformed, too big for out).
size_t wifi_setup_dns_reply(const uint8_t *q, size_t len, const uint8_t ip[4], uint8_t *out, size_t outsize);

#ifdef __cplusplus
}
#endif
