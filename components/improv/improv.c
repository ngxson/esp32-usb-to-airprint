// Improv Wi-Fi serial protocol, device side (see improv.h).
#include "improv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
#define VERSION 1

size_t improv_packet(uint8_t *out, uint8_t type, const uint8_t *data, size_t n)
{
    if (n > 255)
        n = 255;
    memcpy(out, HEADER, 6);
    out[6] = VERSION;
    out[7] = type;
    out[8] = (uint8_t)n;
    memcpy(out + 9, data, n);
    uint8_t sum = 0;
    for (size_t i = 0; i < 9 + n; i++)
        sum += out[i];
    out[9 + n] = sum;
    out[10 + n] = '\n';
    return 11 + n;
}

size_t improv_result(uint8_t *out, uint8_t cmd, const char *const *strs, int count)
{
    uint8_t data[255];
    size_t n = 2;
    data[0] = cmd;
    for (int i = 0; i < count && n < sizeof(data); i++) {
        size_t len = strlen(strs[i]);
        if (len > sizeof(data) - n - 1)
            len = sizeof(data) - n - 1;
        data[n++] = (uint8_t)len;
        memcpy(data + n, strs[i], len);
        n += len;
    }
    // The installer finds packets by reading lines: a \n among the first 9
    // bytes would cut this one. Only the length can be 10; an empty string
    // more makes it 11 (the client ignores strings it does not expect).
    if (n == '\n')
        data[n++] = 0;
    data[1] = (uint8_t)(n - 2);
    return improv_packet(out, IMPROV_TYPE_RPC_RESULT, data, n);
}

static void send(improv_t *im, const uint8_t *pkt, size_t n)
{
    im->cfg.write(pkt, n, im->cfg.ctx);
}

static void send_byte(improv_t *im, uint8_t type, uint8_t value)
{
    uint8_t pkt[IMPROV_PACKET_MAX];
    send(im, pkt, improv_packet(pkt, type, &value, 1));
}

static void send_result(improv_t *im, uint8_t cmd, const char *const *strs, int count)
{
    uint8_t pkt[IMPROV_PACKET_MAX];
    send(im, pkt, improv_result(pkt, cmd, strs, count));
}

// The state; when on the network also the RPC result with the page's URL
// (what both "current state" and a successful "WiFi settings" answer).
static void send_state(improv_t *im, uint8_t cmd)
{
    char url[96];
    bool online = im->cfg.url(url, sizeof(url), im->cfg.ctx);
    send_byte(im, IMPROV_TYPE_CURRENT_STATE, online ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY);
    if (online) {
        const char *s[] = {url};
        send_result(im, cmd, s, 1);
    }
}

static int by_rssi(const void *a, const void *b)
{
    return ((const improv_network_t *)b)->rssi - ((const improv_network_t *)a)->rssi;
}

static void send_networks(improv_t *im)
{
    improv_network_t *nets = calloc(IMPROV_MAX_NETWORKS, sizeof(*nets));
    int n = nets && im->cfg.scan ? im->cfg.scan(nets, IMPROV_MAX_NETWORKS, im->cfg.ctx) : -1;
    if (n < 0) {
        free(nets);
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_UNKNOWN);
        return;
    }
    // strongest first, one entry per name (several access points of one network)
    qsort(nets, (size_t)n, sizeof(*nets), by_rssi);
    for (int i = 0; i < n; i++) {
        bool dup = nets[i].ssid[0] == 0;     // hidden network
        for (int j = 0; j < i && !dup; j++)
            dup = strcmp(nets[i].ssid, nets[j].ssid) == 0;
        if (dup)
            continue;
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", nets[i].rssi);
        const char *s[] = {nets[i].ssid, rssi, nets[i].secured ? "YES" : "NO"};
        send_result(im, IMPROV_CMD_GET_WIFI_NETWORKS, s, 3);
    }
    free(nets);
    send_result(im, IMPROV_CMD_GET_WIFI_NETWORKS, NULL, 0);    // the end of the list
}

static void wifi_settings(improv_t *im, const uint8_t *d, size_t n)
{
    // ssid length, ssid, password length, password
    char ssid[33], password[65];
    size_t sl = n ? d[0] : 0;
    size_t pl = 1 + sl < n ? d[1 + sl] : 0;
    if (n < 2 || sl == 0 || sl > 32 || 2 + sl + pl != n || pl > 64) {
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_INVALID_RPC);
        return;
    }
    memcpy(ssid, d + 1, sl);
    ssid[sl] = 0;
    memcpy(password, d + 2 + sl, pl);
    password[pl] = 0;
    send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_NONE);
    send_byte(im, IMPROV_TYPE_CURRENT_STATE, IMPROV_STATE_PROVISIONING);
    bool ok = im->cfg.connect(ssid, password, im->cfg.ctx);
    memset(password, 0, sizeof(password));
    if (!ok) {
        // "ready" even when back on the previous network: the installer then
        // keeps its form open with the error (with "provisioned" it would say
        // "connected"); asked again, the state says provisioned
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_UNABLE_TO_CONNECT);
        send_byte(im, IMPROV_TYPE_CURRENT_STATE, IMPROV_STATE_READY);
        return;
    }
    send_state(im, IMPROV_CMD_WIFI_SETTINGS);
}

static void handle(improv_t *im, const uint8_t *p, size_t total)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < total - 1; i++)
        sum += p[i];
    if (sum != p[total - 1]) {
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_INVALID_RPC);
        return;
    }
    if (p[7] != IMPROV_TYPE_RPC)
        return;     // device-to-client types: not for us
    const uint8_t *d = p + 9;
    size_t n = p[8];
    if (n < 2 || d[1] != n - 2) {
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_INVALID_RPC);
        return;
    }
    switch (d[0]) {
    case IMPROV_CMD_WIFI_SETTINGS:
        wifi_settings(im, d + 2, n - 2);
        break;
    case IMPROV_CMD_GET_CURRENT_STATE:
        send_state(im, IMPROV_CMD_GET_CURRENT_STATE);
        break;
    case IMPROV_CMD_GET_DEVICE_INFO: {
        const char *s[] = {im->cfg.firmware, im->cfg.version, im->cfg.chip, im->cfg.name};
        send_result(im, IMPROV_CMD_GET_DEVICE_INFO, s, 4);
        break;
    }
    case IMPROV_CMD_GET_WIFI_NETWORKS:
        send_networks(im);
        break;
    default:
        send_byte(im, IMPROV_TYPE_ERROR_STATE, IMPROV_ERROR_UNKNOWN_RPC);
        break;
    }
}

void improv_init(improv_t *im, const improv_config_t *cfg)
{
    memset(im, 0, sizeof(*im));
    im->cfg = *cfg;
}

void improv_feed(improv_t *im, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (im->len < sizeof(HEADER)) {
            // looking for "IMPROV" (anything else on the line is skipped)
            if (b == HEADER[im->len])
                im->buf[im->len++] = b;
            else
                im->len = b == HEADER[0] ? (im->buf[0] = b, 1) : 0;
            continue;
        }
        im->buf[im->len++] = b;
        if (im->len == 7 && b != VERSION) {
            im->len = 0;
            continue;
        }
        if (im->len >= 9 && im->len == 9 + (size_t)im->buf[8] + 1) {
            size_t total = im->len;
            im->len = 0;
            handle(im, im->buf, total);
        }
    }
}
