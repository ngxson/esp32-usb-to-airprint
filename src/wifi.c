#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "local_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_setup.h"

static const char *TAG = "wifi_sta";
static const char *TAG_SETUP = "wifi_setup";

#define NVS_NAMESPACE "wifi_net"
#define SETUP_RESTART_S 10          // after joining from the setup page: time to show the result
#define SETUP_RETRY_S 60            // setup after a lost network: try it again this often
#define SETUP_TRIES 3               // attempts per network from the form
#define SETUP_ATTEMPT_S 45          // an attempt from the form that has not ended by then failed
#define SAVED_ATTEMPT_S 30          // the same for a retry of the lost network
#define PAGE_SIZE 12288

// asked of the next boot (RTC memory, kept over esp_restart)
#define SETUP_FALLBACK 0x57534631u  // the network was lost: setup, and keep trying it
#define SETUP_FORCED 0x57534632u    // the network was forgotten: setup only
static RTC_NOINIT_ATTR uint32_t s_setup_request;

static volatile bool s_connected;
static int s_retries;
static esp_netif_t *s_netif;
static bool (*s_busy)(void);
static int64_t s_down_since;        // when the station last lost the network (or started)

// the network to join: saved from the setup page, else the one from .env
static char s_ssid[33], s_pass[65];
static bool s_have_network;

// setup mode
static char s_ap_ssid[33];          // set: the setup network runs
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_lock;
static wifi_setup_network_t s_nets[WIFI_SETUP_MAX_NETWORKS];
static int s_n_nets;
static wifi_setup_state_t s_state;
static char s_try_ssid[33], s_try_pass[65], s_error[96], s_ip[16];
static int s_tries;                 // attempts left with s_try_*
static int64_t s_attempt_at;
static bool s_trying_saved;         // the attempt is a retry of the lost network, not from the form
static bool s_retry_saved;
static bool s_saving;               // joined from the form: being saved (save_task)

// ---------------------------------------------------------------------------
// saved network

static bool load_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t a = sizeof(s_ssid), b = sizeof(s_pass);
    bool ok = nvs_get_str(h, "ssid", s_ssid, &a) == ESP_OK && s_ssid[0] &&
              nvs_get_str(h, "pass", s_pass, &b) == ESP_OK;
    nvs_close(h);
    if (!ok)
        s_ssid[0] = s_pass[0] = 0;
    return ok;
}

static esp_err_t save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void wifi_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    // also the driver's own copy: builds before the setup network kept the .env password there
    // (esp_wifi_restore() only clears it with flash storage, which is off)
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "WiFi network forgotten: restarting into the setup network");
    s_setup_request = SETUP_FORCED;
    vTaskDelay(pdMS_TO_TICKS(200));     // let the log out
    esp_restart();
}

static void sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static bool setup_password_ok(void)
{
    size_t n = strlen(SETUP_PASSWORD);
    return n >= 8 && n <= 63;
}

// ---------------------------------------------------------------------------
// setup: joining a network from the page

// after a network was joined: the WiFi works, so a freshly updated image is kept (here, not on
// the event loop's small stack)
static void restart_cb(void *arg)
{
    esp_ota_mark_app_valid_cancel_rollback();
    esp_restart();
}

static void restart_in(int s)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = restart_cb, .name = "wifi_restart"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, (uint64_t)s * 1000000);
}

static const char *reason_text(int r, char *buf, size_t n)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found (the printer only sees 2.4 GHz networks)";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "the network's security does not match (a password for an open network?)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "the network's security is not supported";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password";
    default:
        snprintf(buf, n, "no connection (reason %d)", r);
        return buf;
    }
}

// under s_lock; the caller then calls esp_wifi_connect()
static void begin_attempt(const char *ssid, const char *pass, int tries, bool saved)
{
    s_tries = tries;
    s_trying_saved = saved;
    s_attempt_at = esp_timer_get_time();
    if (!saved) {               // the page shows this one
        strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
        strlcpy(s_try_pass, pass, sizeof(s_try_pass));
        s_state = WIFI_SETUP_CONNECTING;
        s_error[0] = 0;
    }
    esp_wifi_disconnect();
    sta_config(ssid, pass);
}

// under s_lock: the network the running attempt is for
static bool is_attempt(const uint8_t *ssid, size_t len)
{
    const char *want = s_trying_saved ? s_ssid : s_try_ssid;
    // an event without a name: the driver's own, for the network it was given (ours)
    return len == 0 || (len == strlen(want) && memcmp(ssid, want, len) == 0);
}

static void setup_disconnected(const wifi_event_sta_disconnected_t *d)
{
    if (d->reason == WIFI_REASON_ASSOC_LEAVE)
        return;                 // our own esp_wifi_disconnect() before another attempt
    bool again = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_saving || !is_attempt(d->ssid, d->ssid_len)) {
        // an earlier attempt's event (another network): it counts for nothing
    } else if (s_trying_saved) {
        s_trying_saved = --s_tries > 0;
        again = s_trying_saved;
    } else if (s_state == WIFI_SETUP_CONNECTING) {
        if (--s_tries > 0) {
            again = true;
        } else {
            char buf[40];
            strlcpy(s_error, reason_text(d->reason, buf, sizeof(buf)), sizeof(s_error));
            s_state = WIFI_SETUP_FAILED;
            ESP_LOGW(TAG_SETUP, "could not join the network from the page: reason %d", d->reason);
        }
    }
    xSemaphoreGive(s_lock);
    if (again)
        esp_wifi_connect();     // a failure here ends in the attempt's timeout (tick_cb)
}

// Saving to flash is too much for the event loop's small stack: a task of its own.
static void save_task(void *arg)
{
    char ssid[33], pass[65];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(ssid, s_try_ssid, sizeof(ssid));
    strlcpy(pass, s_try_pass, sizeof(pass));
    xSemaphoreGive(s_lock);
    esp_err_t err = save(ssid, pass);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_saving = false;
    if (err == ESP_OK) {
        s_state = WIFI_SETUP_CONNECTED;
        // the WiFi works: a freshly updated image must not roll back over the restart
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG_SETUP, "joined, IP %s; network saved, restarting in %d s", s_ip, SETUP_RESTART_S);
        restart_in(SETUP_RESTART_S);
    } else {
        s_state = WIFI_SETUP_FAILED;
        snprintf(s_error, sizeof(s_error), "it worked, but saving it failed (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK)
        esp_wifi_disconnect();
    vTaskDelete(NULL);
}

static void setup_got_ip(const esp_ip4_addr_t *ip)
{
    wifi_ap_record_t ap;
    bool joined = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // only for the network being tried (not a late event of the one before)
    if (s_saving || !joined || !is_attempt(ap.ssid, strnlen((const char *)ap.ssid, sizeof(ap.ssid)))) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_trying_saved) {
        s_trying_saved = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG_SETUP, "the saved network is back: restarting");
        restart_in(1);
        return;
    }
    if (s_state != WIFI_SETUP_CONNECTING) {
        xSemaphoreGive(s_lock);
        return;
    }
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(ip));
    s_saving = true;
    if (xTaskCreate(save_task, "wifi_save", 4096, NULL, 3, NULL) != pdPASS) {
        s_saving = false;
        s_state = WIFI_SETUP_FAILED;
        strlcpy(s_error, "out of memory", sizeof(s_error));
    }
    xSemaphoreGive(s_lock);
}

// Every second in setup mode: ends attempts that never answered, and after a lost network
// tries it again now and then while nobody uses the setup network.
static void tick_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    static int64_t last_retry;
    wifi_sta_list_t stations;
    bool alone = esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num == 0;
    bool stop = false, retry = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_saving) {
    } else if (s_trying_saved) {
        if (now - s_attempt_at > SAVED_ATTEMPT_S * 1000000LL) {
            s_trying_saved = false;
            stop = true;
        }
    } else if (s_state == WIFI_SETUP_CONNECTING) {
        if (now - s_attempt_at > SETUP_ATTEMPT_S * 1000000LL) {
            s_state = WIFI_SETUP_FAILED;
            strlcpy(s_error, "no answer", sizeof(s_error));
            stop = true;
        }
    } else if (s_retry_saved && alone && s_state != WIFI_SETUP_CONNECTED &&
               now - last_retry >= SETUP_RETRY_S * 1000000LL) {
        last_retry = now;
        begin_attempt(s_ssid, s_pass, 1, true);
        retry = true;
    }
    if (stop)                   // under the lock: an attempt the page starts next is not cut off
        esp_wifi_disconnect();
    xSemaphoreGive(s_lock);
    if (retry)
        esp_wifi_connect();
}

// ---------------------------------------------------------------------------
// setup: networks around

// The networks around, strongest first, into nets; their number, -1 on failure. *channel (if
// given): the channel of the saved network if seen, else of the strongest one, else 0.
static int scan_networks(wifi_setup_network_t *nets, int max, int *channel)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK)
        return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40)
        n = 40;
    if (channel)
        *channel = 0;
    wifi_ap_record_t *rec = calloc(n ? n : 1, sizeof(*rec));
    if (!rec) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, rec);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const char *ssid = (const char *)rec[i].ssid;
        if (!ssid[0])
            continue;           // hidden
        bool dup = false;
        for (int j = 0; j < k && !dup; j++)
            dup = strcmp(nets[j].ssid, ssid) == 0;
        if (dup)
            continue;
        strlcpy(nets[k].ssid, ssid, sizeof(nets[k].ssid));
        nets[k].rssi = rec[i].rssi;
        nets[k].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    for (int i = 0; channel && i < n; i++) {
        if (i == 0)
            *channel = rec[i].primary;
        if (s_have_network && strcmp((const char *)rec[i].ssid, s_ssid) == 0) {
            *channel = rec[i].primary;
            break;
        }
    }
    free(rec);
    return k;
}

// ---------------------------------------------------------------------------
// setup: DNS (every name is the board)

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG_SETUP, "DNS server: cannot bind port 53");
        if (s >= 0)
            close(s);
        vTaskDelete(NULL);
    }
    esp_netif_ip_info_t ipi;
    esp_netif_get_ip_info(s_ap_netif, &ipi);
    uint8_t ip[4];
    memcpy(ip, &ipi.ip.addr, 4);
    uint8_t q[512], r[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s, q, sizeof(q), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0)
            continue;
        size_t rn = wifi_setup_dns_reply(q, (size_t)n, ip, r, sizeof(r));
        if (rn)
            sendto(s, r, rn, 0, (struct sockaddr *)&from, flen);
    }
}

// ---------------------------------------------------------------------------
// setup: the page

static char s_ap_ip[16], s_ap_url[32];     // 192.168.4.1, http://192.168.4.1/

static esp_err_t redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_ap_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // iOS needs a body to show the portal
    return httpd_resp_send(req, "WiFi setup", HTTPD_RESP_USE_STRLEN);
}

// unknown pages and methods (GET /connect)
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    return redirect(req);
}

static esp_err_t page_get(httpd_req_t *req)
{
    // a phone's connectivity check (http://captive.apple.com/ ...) gets the portal's own address
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (host[0] && strcmp(host, s_ap_ip) != 0)
        return redirect(req);

    char *buf = malloc(PAGE_SIZE);
    if (!buf)
        return httpd_resp_send_500(req);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wifi_setup_page_t p = {
        .printer_name = PRINTER_NAME,
        .hostname = HOSTNAME,
        .networks = s_nets,
        .n_networks = s_n_nets,
        .state = s_state,
        .ssid = s_state == WIFI_SETUP_IDLE ? NULL : s_try_ssid,
        .error = s_error,
        .ip = s_ip,
    };
    size_t n = wifi_setup_page(&p, buf, PAGE_SIZE);
    xSemaphoreGive(s_lock);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r;
}

static esp_err_t see_page(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[512];
    int len = 0;
    while (len < (int)req->content_len && len < (int)sizeof(body)) {
        int r = httpd_req_recv(req, body + len, sizeof(body) - len);
        if (r <= 0)
            return ESP_FAIL;
        len += r;
    }
    // a typed name wins over the list
    char ssid[33], other[33], pass[65];
    int ssid_len = wifi_setup_form_value(body, len, "ssid", ssid, sizeof(ssid));
    int other_len = wifi_setup_form_value(body, len, "other", other, sizeof(other));
    int pass_len = wifi_setup_form_value(body, len, "password", pass, sizeof(pass));
    const char *net = other_len > 0 || other_len == -2 ? other : ssid;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool go = false;
    if (other_len == -2 || (other_len <= 0 && ssid_len == -2)) {
        s_state = WIFI_SETUP_FAILED;
        s_try_ssid[0] = 0;
        strlcpy(s_error, "a network name has at most 32 bytes", sizeof(s_error));
    } else if (!net[0]) {
        s_state = WIFI_SETUP_FAILED;
        s_try_ssid[0] = 0;
        strlcpy(s_error, "choose a network", sizeof(s_error));
    } else if (pass_len == -2 || !wifi_setup_password_ok(pass)) {
        s_state = WIFI_SETUP_FAILED;
        strlcpy(s_try_ssid, net, sizeof(s_try_ssid));
        strlcpy(s_error, "a WiFi password has 8 to 63 characters", sizeof(s_error));
    } else if (s_state != WIFI_SETUP_CONNECTED && !s_saving) {
        begin_attempt(net, pass, SETUP_TRIES, false);
        go = true;
    }
    xSemaphoreGive(s_lock);
    if (go) {
        ESP_LOGI(TAG_SETUP, "trying the network from the page");
        esp_wifi_connect();
    }
    return see_page(req);
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    wifi_setup_network_t *nets = calloc(WIFI_SETUP_MAX_NETWORKS, sizeof(*nets));
    int r = nets ? scan_networks(nets, WIFI_SETUP_MAX_NETWORKS, NULL) : -1;
    if (r >= 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_n_nets = r;
        memcpy(s_nets, nets, sizeof(s_nets));
        xSemaphoreGive(s_lock);
    }
    free(nets);
    return see_page(req);
}

static esp_err_t setup_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // look around first, before anyone is on the setup network
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG_SETUP, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG_SETUP, "start");
    int channel = 0;
    int r = scan_networks(s_nets, WIFI_SETUP_MAX_NETWORKS, &channel);
    s_n_nets = r < 0 ? 0 : r;
    if (!channel)
        channel = 1;
    esp_wifi_stop();

    // the setup network, on the channel of the network it will most likely join (a station and an
    // access point share the radio: when the station joins another channel, the access point follows)
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = channel;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (setup_password_ok()) {
        strlcpy((char *)ap.ap.password, SETUP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (SETUP_PASSWORD[0]) {
        ESP_LOGE(TAG_SETUP, "SETUP_PASSWORD ignored: it needs 8 to 63 characters");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG_SETUP, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG_SETUP, "config");

    // DHCP hands out the board as the DNS server (LWIP_DHCPS_ADD_DNS); phones find the page
    // through their connectivity checks, which every answer redirects
    esp_netif_ip_info_t ipi;
    esp_netif_get_ip_info(s_ap_netif, &ipi);
    snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ipi.ip));
    snprintf(s_ap_url, sizeof(s_ap_url), "http://%s/", s_ap_ip);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG_SETUP, "start");

    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t h;
    ESP_RETURN_ON_ERROR(httpd_start(&h, &cfg), TAG_SETUP, "http");
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/connect", .method = HTTP_POST, .handler = connect_post},
        {.uri = "/rescan", .method = HTTP_POST, .handler = rescan_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(h, &uris[i]);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, not_found);
    httpd_register_err_handler(h, HTTPD_405_METHOD_NOT_ALLOWED, not_found);
    // phones probe many addresses: the redirects are expected
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    const esp_timer_create_args_t a = {.callback = tick_cb, .name = "wifi_setup"};
    esp_timer_handle_t t;
    ESP_RETURN_ON_ERROR(esp_timer_create(&a, &t), TAG_SETUP, "timer");
    esp_timer_start_periodic(t, 1000000);
    ESP_LOGW(TAG_SETUP, "setup network \"%s\" (%s) on channel %d: join it and open %s%s", s_ap_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "SETUP_PASSWORD", channel, s_ap_url,
             s_retry_saved ? "; the saved network is tried every minute" : "");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// station

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_ap_ssid[0])
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_connected)
            s_down_since = esp_timer_get_time();
        s_connected = false;
        if (s_ap_ssid[0]) {
            setup_disconnected(d);
            return;
        }
        if (s_retries++ % 10 == 0)
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        if (s_ap_ssid[0]) {
            setup_got_ip(&e->ip_info.ip);
            return;
        }
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG_SETUP, "a device joined the setup network");
    }
}

// a network gone for good (another router, a new password): the setup network, to choose again.
// Only with SETUP_PASSWORD: an open one would let anyone who keeps the board off its network
// for a while (or waits for a power cut) point it at theirs.
static void watch_cb(void *arg)
{
    int64_t limit = (int64_t)CONFIG_WIFI_SETUP_AFTER_MIN * 60 * 1000000;
    if (s_connected || limit == 0 || !setup_password_ok() || esp_timer_get_time() - s_down_since < limit || (s_busy && s_busy()))
        return;
    ESP_LOGW(TAG, "no WiFi for %d minutes: restarting into the setup network", CONFIG_WIFI_SETUP_AFTER_MIN);
    s_setup_request = SETUP_FALLBACK;
    esp_restart();
}

esp_err_t wifi_start(const char *hostname, bool (*busy)(void))
{
    s_busy = busy;
    uint32_t request = s_setup_request;
    s_setup_request = 0;
    if (esp_reset_reason() != ESP_RST_SW)
        request = 0;            // RTC memory is only meaningful after esp_restart()

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    if (load_saved()) {
        s_have_network = true;
    } else if (strlen(WIFI_SSID) > 0) {
        strlcpy(s_ssid, WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, WIFI_PASSWORD, sizeof(s_pass));
        s_have_network = true;
    }
    bool setup = !s_have_network || request == SETUP_FORCED || request == SETUP_FALLBACK;
    s_retry_saved = s_have_network && request == SETUP_FALLBACK;
    if (setup) {
        // the same name whatever the printer and the build; the MAC tells two boards apart
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Printer-Setup-%02X%02X", mac[4], mac[5]);
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);

    // the driver's own info logs include the SSID; keep warnings only
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // the network is in our own NVS namespace
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    // no modem sleep: a sleeping station answers mDNS / IPP slowly
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (setup) {
        err = setup_start();
        if (err != ESP_OK)
            s_ap_ssid[0] = 0;
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    sta_config(s_ssid, s_pass);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_down_since = esp_timer_get_time();
    if (CONFIG_WIFI_SETUP_AFTER_MIN > 0 && !setup_password_ok())
        ESP_LOGI(TAG, "no SETUP_PASSWORD: a lost network does not open the setup network (hold BOOT 10 s)");
    const esp_timer_create_args_t a = {.callback = watch_cb, .name = "wifi_watch"};
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_periodic(t, 10 * 1000000);

    // real wall clock for IPP date-time attributes
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    ESP_LOGI(TAG, "connecting as %s", hostname);
    return ESP_OK;
}

bool wifi_connected(void)
{
    return s_connected;
}

const char *wifi_setup_ap(void)
{
    return s_ap_ssid[0] ? s_ap_ssid : NULL;
}

const char *wifi_setup_ip(void)
{
    return s_ap_ip[0] ? s_ap_ip : NULL;
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
