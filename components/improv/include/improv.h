// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/), device side:
// the protocol the web installer (esp-web-tools) uses after flashing to ask
// for the WiFi network, list the networks the board sees and link to its page.
// Portable: bytes in through improv_feed, packets out through a write hook.
//
// Packet: "IMPROV", version 1, type, data length, data, checksum (sum of all
// the bytes before it). The client sends RPCs (type 3: command, its data's
// length, data); the device answers with its state (1), an error (2) or RPC
// results (4: command, length, then length-prefixed strings). Packets share
// the line with the console log, so each one is sent whole and ends with \n.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    IMPROV_STATE_READY = 0x02,          // no network yet (serial is always "authorized")
    IMPROV_STATE_PROVISIONING = 0x03,   // joining the network it was given
    IMPROV_STATE_PROVISIONED = 0x04,    // on the network: the RPC result carries its URL
};

enum {
    IMPROV_ERROR_NONE = 0x00,
    IMPROV_ERROR_INVALID_RPC = 0x01,
    IMPROV_ERROR_UNKNOWN_RPC = 0x02,
    IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_ERROR_UNKNOWN = 0xff,
};

enum {
    IMPROV_TYPE_CURRENT_STATE = 0x01,
    IMPROV_TYPE_ERROR_STATE = 0x02,
    IMPROV_TYPE_RPC = 0x03,
    IMPROV_TYPE_RPC_RESULT = 0x04,
};

enum {
    IMPROV_CMD_WIFI_SETTINGS = 0x01,
    IMPROV_CMD_GET_CURRENT_STATE = 0x02,
    IMPROV_CMD_GET_DEVICE_INFO = 0x03,
    IMPROV_CMD_GET_WIFI_NETWORKS = 0x04,
};

typedef struct {
    char ssid[33];
    int rssi;           // dBm
    bool secured;
} improv_network_t;

typedef struct {
    // REQUEST_INFO: firmware name (the installer compares it with its
    // manifest's "name"), version, chip family ("ESP32-S3"), device name
    const char *firmware;
    const char *version;
    const char *chip;
    const char *name;
    // one whole packet (the caller must not interleave other output in it)
    void (*write)(const uint8_t *data, size_t len, void *ctx);
    // true and the device's page ("http://192.168.1.20/") when it is on the network
    bool (*url)(char *buf, size_t n, void *ctx);
    // joins the network, blocking until it is connected (true; the
    // credentials are then kept) or has failed (false)
    bool (*connect)(const char *ssid, const char *password, void *ctx);
    // the networks in range, at most max; -1 on failure. Optional.
    int (*scan)(improv_network_t *out, int max, void *ctx);
    void *ctx;
} improv_config_t;

#define IMPROV_MAX_NETWORKS 24

typedef struct {
    improv_config_t cfg;
    uint8_t buf[9 + 255 + 1];   // header, data, checksum
    size_t len;
} improv_t;

void improv_init(improv_t *im, const improv_config_t *cfg);
// Feeds bytes read from the serial port (anything that is not a packet is
// skipped); handles each complete RPC, so it may block in connect / scan.
void improv_feed(improv_t *im, const uint8_t *data, size_t n);

// Packet builders, also used by the tests. Return the length written to out
// (at most IMPROV_PACKET_MAX), including the trailing \n.
#define IMPROV_PACKET_MAX (9 + 255 + 2)
size_t improv_packet(uint8_t *out, uint8_t type, const uint8_t *data, size_t n);
// An RPC result: command, then the strings (each cut to fit the packet).
size_t improv_result(uint8_t *out, uint8_t cmd, const char *const *strs, int count);
