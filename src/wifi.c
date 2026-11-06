#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "local_config.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

// The network set from the installer lives in NVS and wins over .env's.
// (The same namespace and keys as the setup page of the captive-portal branch,
// so either way of setting the network stores it alike.)
#define NVS_NAMESPACE "wifi_net"

static volatile bool s_connected;
static volatile bool s_hold;            // no reconnecting: a scan or a change is under way
static volatile int s_failures;         // disconnects since the last connection
static int s_retries;
static esp_netif_t *s_netif;
static char s_ssid[33], s_password[65];
static SemaphoreHandle_t s_lock;        // scans and changes, one at a time

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0])
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        s_failures++;
        if (s_hold || !s_ssid[0])
            return;
        if (s_retries++ % 10 == 0)
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_failures = 0;
        s_connected = true;
    }
}

// the saved network if there is one, else .env's
static void load_credentials(void)
{
    nvs_handle_t h;
    size_t n1 = sizeof(s_ssid), n2 = sizeof(s_password);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        bool saved = nvs_get_str(h, "ssid", s_ssid, &n1) == ESP_OK &&
                     nvs_get_str(h, "pass", s_password, &n2) == ESP_OK && s_ssid[0];
        nvs_close(h);
        if (saved) {
            ESP_LOGI(TAG, "using the saved network");
            return;
        }
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", WIFI_SSID);
    snprintf(s_password, sizeof(s_password), "%s", WIFI_PASSWORD);
}

static esp_err_t save_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, "ssid", s_ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", s_password);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t apply_credentials(void)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, s_password, strnlen(s_password, sizeof(cfg.sta.password)));
    cfg.sta.threshold.authmode = s_password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

esp_err_t wifi_start(const char *hostname)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    load_credentials();
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);

    // the driver's own info logs include the SSID; keep warnings only
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    // the network is kept by save_credentials, not by the driver
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    // the radio also starts without a network: the installer scans with it
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(apply_credentials(), TAG, "config");
    // no modem sleep: a sleeping station answers mDNS / IPP slowly
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    // real wall clock for IPP date-time attributes
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (s_ssid[0])
        ESP_LOGI(TAG, "connecting as %s", hostname);
    else
        ESP_LOGW(TAG, "no WiFi network: set it from the web installer (COM port) or in .env");
    return ESP_OK;
}

bool wifi_connected(void)
{
    return s_connected;
}

bool wifi_configured(void)
{
    return s_ssid[0] != 0;
}

bool wifi_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (n)
            buf[0] = 0;
        return false;
    }
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_rssi(void)
{
    int rssi = 0;
    if (!s_connected || esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
        return 0;
    return rssi;
}

// stops the connection attempts (a station busy connecting can neither scan
// nor take another network)
static void hold(void)
{
    s_hold = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void release(void)
{
    s_hold = false;
    if (s_ssid[0] && !s_connected)
        esp_wifi_connect();
}

int wifi_scan(wifi_network_t *out, int max)
{
    if (!s_lock)
        return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool paused = !s_connected;     // a connected station scans between its frames
    if (paused)
        hold();
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    if (err == ESP_OK)
        esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = n ? calloc(n, sizeof(*recs)) : NULL;
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK)
        n = 0;
    esp_wifi_clear_ap_list();   // frees the driver's list when it was not read
    if (paused)
        release();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        free(recs);
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    int count = 0;
    for (int i = 0; i < n && count < max; i++, count++) {
        snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", (const char *)recs[i].ssid);
        out[count].rssi = recs[i].rssi;
        out[count].secured = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    free(recs);
    return count;
}

bool wifi_set_network(const char *ssid, const char *password, int timeout_ms)
{
    if (!s_lock)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char old_ssid[sizeof(s_ssid)], old_password[sizeof(s_password)];
    memcpy(old_ssid, s_ssid, sizeof(s_ssid));
    memcpy(old_password, s_password, sizeof(s_password));

    hold();
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_password, sizeof(s_password), "%s", password);
    ESP_LOGI(TAG, "joining the network from the installer");
    bool ok = apply_credentials() == ESP_OK;
    s_failures = 0;
    if (ok)
        release();
    // a few attempts (each fails in a few seconds with a wrong password), at most timeout_ms
    TickType_t start = xTaskGetTickCount();
    while (ok && !s_connected && s_failures < 4 && xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms))
        vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && s_connected;

    if (ok) {
        esp_err_t err = save_credentials();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "could not save the network: %s", esp_err_to_name(err));
        else
            ESP_LOGI(TAG, "network saved");
    } else {
        ESP_LOGW(TAG, "could not join the network from the installer%s",
                 old_ssid[0] ? ", back to the previous one" : "");
        hold();
        memcpy(s_ssid, old_ssid, sizeof(s_ssid));
        memcpy(s_password, old_password, sizeof(s_password));
        apply_credentials();
        release();
    }
    memset(old_password, 0, sizeof(old_password));
    xSemaphoreGive(s_lock);
    return ok;
}
