#include "airprint_esp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airprint.h"
#include "canon_bjl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "local_config.h"
#include "mdns.h"
#include "status_led.h"
#include "usb_printer.h"

static const char *TAG = "airprint_esp";

#define IPP_PORT 631
#define CANCEL_FORCE_MS 8000    // graceful cancel didn't finish: abort the USB transfer

static airprint_config_t s_cfg;
static char s_uuid[40];
static void (*s_on_job_state)(bool);
static esp_timer_handle_t s_cancel_timer;
static volatile bool s_job_running;

// MODE=dev: no printer; jobs are converted, checksummed and kept in PSRAM
#define DEV_CAPTURE_MAX (6 * 1024 * 1024)
static uint8_t *s_cap;
static size_t s_cap_len;
static bool s_cap_truncated;
static uint32_t s_dev_crc;
static int64_t s_dev_t0;

// ---------------------------------------------------------------------------
// printer status

// Canon "CIR:CL=000,BK=UK" -> remaining ink in percent, -2 = unknown
static int ink_level(const char *data, const char *key)
{
    const char *cir = strstr(data, "CIR:");
    if (!cir)
        return -2;
    const char *p = strstr(cir, key);
    const char *end = strchr(cir, ';');
    if (!p || (end && p > end))
        return -2;
    p += strlen(key);
    if (*p < '0' || *p > '9')
        return -2;      // "UK": unknown (e.g. refilled cartridge)
    int v = atoi(p);
    return v > 100 ? 100 : v;
}

static void add_reason(airprint_status_t *st, const char *reason)
{
    if (st->nreasons < (int)(sizeof(st->reasons) / sizeof(st->reasons[0])))
        st->reasons[st->nreasons++] = reason;
}

static void get_status(void *ctx, airprint_status_t *st)
{
    st->connected = usb_printer_connected();
    if (!st->connected)
        return;
    st->busy = usb_printer_job_active() && !s_job_running;     // local test page

    usb_printer_status_t cs;
    bool have = usb_printer_get_status(&cs);
    const char *d = have ? cs.detail : "";
    if (strstr(d, "MarkerSupplyEmpty") || strstr(d, "MarkerSupplyMissing")) {
        add_reason(st, "marker-supply-empty-error");
        snprintf(st->message, sizeof(st->message), "Ink cartridge empty or missing");
    } else if (strstr(d, "MarkerSupplyAttention") || strstr(d, "MarkerSupplyLow")) {
        add_reason(st, "marker-supply-low-warning");
        snprintf(st->message, sizeof(st->message), "Ink low");
    }
    if (strstr(d, "MediaEmpty") || strstr(d, "MediaNeeded")) {
        add_reason(st, "media-empty-error");
        snprintf(st->message, sizeof(st->message), "Out of paper");
    }
    if (strstr(d, "MediaJam")) {
        add_reason(st, "media-jam-error");
        snprintf(st->message, sizeof(st->message), "Paper jam");
    }
    if (strstr(d, "DoorOpen") || strstr(d, "CoverOpen")) {
        add_reason(st, "cover-open-error");
        snprintf(st->message, sizeof(st->message), "Cover open");
    }
    uint8_t port = 0;
    if (!s_job_running && usb_printer_port_status(&port) == ESP_OK && (port & 0x20) &&
        !strstr(d, "MediaEmpty")) {
        add_reason(st, "media-empty-error");
        snprintf(st->message, sizeof(st->message), "Out of paper");
    }

    st->nsupplies = 2;
    st->supply[0].name = "Black";
    st->supply[0].color = "#000000";
    st->supply[0].level = have ? ink_level(cs.data, "BK=") : -2;
    st->supply[1].name = "Color";
    st->supply[1].color = "#00FFFF#FF00FF#FFFF00";
    st->supply[1].level = have ? ink_level(cs.data, "CL=") : -2;
}

// ---------------------------------------------------------------------------
// job hooks

static int job_open(void *ctx)
{
    esp_err_t err = usb_printer_job_begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "printer not available: %s", esp_err_to_name(err));
        return -1;
    }
    s_job_running = true;
    return 0;
}

static int job_write(void *ctx, const uint8_t *data, size_t len)
{
    return usb_printer_write(data, len) == ESP_OK ? 0 : -1;
}

// same handshake as the Canon driver: wait for each IVEC reply, but go on
// without it (Gutenprint never waits)
static int job_sync(void *ctx, const char *what)
{
    if (usb_printer_flush() != ESP_OK)
        return -1;
    // EndJob is answered when the page is done: keeps the job "processing"
    uint32_t timeout = strcmp(what, "EndJob") == 0 ? 180000 : 5000;
    esp_err_t err = usb_printer_wait_response(what, timeout);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s acknowledged", what);
    else if (err == ESP_ERR_TIMEOUT)
        ESP_LOGW(TAG, "no %s reply, continuing", what);
    else if (err != ESP_ERR_NOT_SUPPORTED)
        return -1;
    return 0;
}

static void job_close(void *ctx, bool ok)
{
    esp_timer_stop(s_cancel_timer);
    usb_printer_job_end();
    s_job_running = false;
    if (!ok && usb_printer_connected()) {
        ESP_LOGW(TAG, "job failed, resetting printer");
        usb_printer_soft_reset();
    }
}

static void cancel_timeout(void *arg)
{
    if (s_job_running) {
        ESP_LOGW(TAG, "job did not stop, aborting the transfer");
        usb_printer_cancel();
    }
}

// The pipeline stops at the next raster line and ejects the sheet. If the
// printer is stuck (no paper), writes block: force-abort after a while.
static void job_cancel(void *ctx)
{
    esp_timer_stop(s_cancel_timer);
    esp_timer_start_once(s_cancel_timer, CANCEL_FORCE_MS * 1000ULL);
}

static void job_state(void *ctx, bool printing)
{
    if (s_on_job_state)
        s_on_job_state(printing);
}

static void identify(void *ctx)
{
    ESP_LOGI(TAG, "Identify-Printer");
    status_led_identify();
}

// ---------------------------------------------------------------------------
// MODE=dev backend: a printer that accepts everything and prints nothing

static void dev_get_status(void *ctx, airprint_status_t *st)
{
    st->connected = true;
    snprintf(st->message, sizeof(st->message), "Dev mode: jobs are not printed");
    st->nsupplies = 2;
    st->supply[0].name = "Black";
    st->supply[0].color = "#000000";
    st->supply[0].level = 80;
    st->supply[1].name = "Color";
    st->supply[1].color = "#00FFFF#FF00FF#FFFF00";
    st->supply[1].level = 30;
}

static int dev_job_open(void *ctx)
{
    if (s_job_running)
        return -1;
    if (!s_cap)
        s_cap = heap_caps_malloc(DEV_CAPTURE_MAX, MALLOC_CAP_SPIRAM);
    s_cap_len = 0;
    s_cap_truncated = false;
    s_dev_crc = 0;
    s_dev_t0 = esp_timer_get_time();
    s_job_running = true;
    ESP_LOGI(TAG, "dev job started (capture buffer %s)", s_cap ? "6 MB in PSRAM" : "unavailable, CRC only");
    return 0;
}

static int dev_job_write(void *ctx, const uint8_t *data, size_t len)
{
    s_dev_crc = bjl_crc32(s_dev_crc, data, len);
    if (s_cap && s_cap_len + len <= DEV_CAPTURE_MAX) {
        memcpy(s_cap + s_cap_len, data, len);
        s_cap_len += len;
    } else {
        s_cap_truncated = true;
        s_cap_len += s_cap ? 0 : len;
    }
    return 0;
}

static int dev_job_sync(void *ctx, const char *what)
{
    ESP_LOGI(TAG, "dev: %s (would wait for the printer here)", what);
    return 0;
}

static void dev_job_close(void *ctx, bool ok)
{
    s_job_running = false;
    ESP_LOGI(TAG, "DEV job done: %u bytes of printer data, crc32 %08lx, %lld ms%s%s", (unsigned)s_cap_len,
             (unsigned long)s_dev_crc, (long long)((esp_timer_get_time() - s_dev_t0) / 1000),
             s_cap_truncated ? ", capture truncated" : "", ok ? "" : ", job failed");
    if (s_cap && !s_cap_truncated)
        ESP_LOGI(TAG, "download: curl -o last-job.prn http://<this IP>:%d/dev/last-job.prn", IPP_PORT);
    ESP_LOGI(TAG, "internal RAM free %u (min %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static bool dev_get_file(void *ctx, const char *path, const uint8_t **data, size_t *len, const char **type)
{
    if (strcmp(path, "/dev/last-job.prn") != 0 || s_job_running || !s_cap || !s_cap_len || s_cap_truncated)
        return false;
    *data = s_cap;
    *len = s_cap_len;
    *type = "application/octet-stream";
    return true;
}

// ---------------------------------------------------------------------------

static esp_err_t start_mdns(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK)
        return err;
    mdns_hostname_set(hostname);
    mdns_instance_name_set(PRINTER_NAME);

    char adminurl[96];
    snprintf(adminurl, sizeof(adminurl), "http://%s.local:%d/", hostname, IPP_PORT);
    // Bonjour Printing / AirPrint TXT record
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", "ipp/print"},
        {"ty", s_cfg.make_model},
        {"product", "(MG2500 series)"},
        {"note", ""},
        {"adminurl", adminurl},
        {"priority", "50"},
        {"pdl", AIRPRINT_PDL},
        {"URF", AIRPRINT_URF},
        {"UUID", s_uuid},
        {"Color", "T"},
        {"Duplex", "F"},
        {"Copies", "F"},
        {"kind", "document"},
        {"PaperMax", "<legal-A4"},     // up to A4
        {"usb_MFG", "Canon"},
        {"usb_MDL", "MG2500 series"},
        {"air", "none"},
        {"Transparent", "T"},
        {"Binary", "T"},
        {"TBCP", "F"},
        {"Scan", "F"},
        {"Fax", "F"},
    };
    err = mdns_service_add(PRINTER_NAME, "_ipp", "_tcp", IPP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK)
        return err;
    // "_universal" is what iOS looks for; "_print" is IPP Everywhere
    mdns_service_subtype_add_for_host(PRINTER_NAME, "_ipp", "_tcp", NULL, "_universal");
    mdns_service_subtype_add_for_host(PRINTER_NAME, "_ipp", "_tcp", NULL, "_print");
    ESP_LOGI(TAG, "announced \"%s\" as %s.local:%d", PRINTER_NAME, hostname, IPP_PORT);
    return ESP_OK;
}

esp_err_t airprint_esp_start(const char *hostname, void (*on_job_state)(bool printing))
{
    s_on_job_state = on_job_state;
    // stable per-board UUID from the MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_uuid, sizeof(s_uuid), "a4d1e9b2-0b6f-4c3e-9d2a-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    const esp_timer_create_args_t targs = {.callback = cancel_timeout, .name = "ipp_cancel"};
    esp_timer_create(&targs, &s_cancel_timer);

    s_cfg = (airprint_config_t){
        .name = PRINTER_NAME,
        .make_model = "Canon MG2500 series",
        .uuid = s_uuid,
        .device_id = "MFG:Canon;MDL:MG2500 series;CMD:URF,PWG;CLS:PRINTER;",
        .port = IPP_PORT,
        .get_status = get_status,
        .job_open = job_open,
        .job_write = job_write,
        .job_sync = job_sync,
        .job_close = job_close,
        .job_cancel = job_cancel,
        .job_state = job_state,
        .identify = identify,
    };
    if (strcmp(MODE, "dev") == 0) {
        ESP_LOGW(TAG, "MODE=dev: AirPrint jobs are converted but NOT printed");
        s_cfg.get_status = dev_get_status;
        s_cfg.job_open = dev_job_open;
        s_cfg.job_write = dev_job_write;
        s_cfg.job_sync = dev_job_sync;
        s_cfg.job_close = dev_job_close;
        s_cfg.job_cancel = NULL;
        s_cfg.get_file = dev_get_file;
    }
    if (airprint_start(&s_cfg) != 0)
        return ESP_FAIL;
    return start_mdns(hostname);
}
