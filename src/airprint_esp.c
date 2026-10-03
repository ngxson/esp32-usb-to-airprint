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
#include "fake_scanner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_config.h"
#include "mdns.h"
#include "scan_service.h"
#include "shared_buf.h"
#include "status_led.h"
#include "usb_printer.h"

static const char *TAG = "airprint_esp";

#define IPP_PORT 631
#define WEB_PORT 80             // status and scan pages for browsers (also on 631)
#define CANCEL_FORCE_MS 8000    // graceful cancel didn't finish: abort the USB transfer

static airprint_config_t s_cfg;
static char s_uuid[40];
static void (*s_on_job_state)(bool);
static esp_timer_handle_t s_cancel_timer;
static volatile bool s_job_running;

// The shared buffer takes all the PSRAM but this much, left for everything
// else (encoder strips, raster buffers, ...)
#define PSRAM_RESERVE (512 * 1024)
#define SHARED_BUF_MIN (1024 * 1024)

// MODE=dev: no printer; jobs are converted, checksummed and kept in the
// shared buffer. Scans come from the emulated scanner.
#define DEV_SCANNER_RATE (800 * 1024)   // about what full-speed USB gives
static uint8_t *s_cap;
static size_t s_cap_size, s_cap_len;
static bool s_cap_truncated;
static volatile bool s_dev_scanning;
static fake_scanner_t s_fake_scanner;
static scan_config_t s_scan_cfg;
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
    st->busy = usb_printer_job_active() && !s_job_running;     // local test page or scan
    bool scanning = usb_scanner_active();
    if (scanning)
        snprintf(st->message, sizeof(st->message), "Scanning");

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
    if (!s_job_running && !scanning && usb_printer_port_status(&port) == ESP_OK && (port & 0x20) &&
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
    if (s_job_running || s_dev_scanning)
        return -1;
    s_cap = sbuf_acquire(SBUF_PRINT, &s_cap_size);     // NULL: CRC only
    s_cap_len = 0;
    s_cap_truncated = false;
    s_dev_crc = 0;
    s_dev_t0 = esp_timer_get_time();
    s_job_running = true;
    if (s_cap)
        ESP_LOGI(TAG, "dev job started (capture buffer %u KB)", (unsigned)(s_cap_size / 1024));
    else
        ESP_LOGI(TAG, "dev job started (capture buffer in use or missing: CRC only)");
    return 0;
}

static int dev_job_write(void *ctx, const uint8_t *data, size_t len)
{
    s_dev_crc = bjl_crc32(s_dev_crc, data, len);
    if (s_cap && s_cap_len + len <= s_cap_size) {
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
    if (s_cap)
        sbuf_release(SBUF_PRINT, s_cap_truncated ? 0 : s_cap_len);
    s_job_running = false;
    ESP_LOGI(TAG, "DEV job done: %u bytes of printer data, crc32 %08lx, %lld ms%s%s", (unsigned)s_cap_len,
             (unsigned long)s_dev_crc, (long long)((esp_timer_get_time() - s_dev_t0) / 1000),
             s_cap_truncated ? ", capture truncated" : "", ok ? "" : ", job failed");
    if (s_cap && !s_cap_truncated)
        ESP_LOGI(TAG, "download: curl -o last-job.prn http://<this IP>/dev/last-job.prn");
    ESP_LOGI(TAG, "internal RAM free %u (min %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static bool dev_http(void *ctx, airprint_http_t *h, const char *method, const char *path)
{
    if (strcmp(path, "/dev/last-job.prn") != 0)
        return scan_service_http(h, method, path);
    size_t len;
    const uint8_t *data = sbuf_read_begin(SBUF_PRINT, &len);
    if (!data)
        return false;
    airprint_http_send(h, "200 OK", "application/octet-stream", data, len);
    sbuf_read_end();
    return true;
}

static bool dev_scan_available(void *ctx)
{
    return true;
}

static int dev_scan_open(void *ctx)
{
    if (s_job_running)
        return -1;
    s_dev_scanning = true;
    return 0;
}

static void dev_scan_close(void *ctx)
{
    if (s_fake_scanner.errors)
        ESP_LOGW(TAG, "emulated scanner: %d protocol violations, first: %s", s_fake_scanner.errors,
                 s_fake_scanner.error);
    s_dev_scanning = false;
}

// ---------------------------------------------------------------------------
// scanner

static bool scan_available(void *ctx)
{
    return usb_scanner_present();
}

static int scan_open(void *ctx)
{
    esp_err_t err = usb_scanner_begin();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "scanner not available: %s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static void scan_close(void *ctx)
{
    usb_scanner_end();
}

static int scan_io_result(int r)
{
    return r >= 0 ? r : r == USB_SCAN_TIMEOUT ? PIXMA_ETIMEDOUT : PIXMA_EIO;
}

static int scan_write(void *ctx, const uint8_t *data, size_t len, int timeout_ms)
{
    return scan_io_result(usb_scanner_write(data, len, (uint32_t)timeout_ms));
}

static int scan_read(void *ctx, uint8_t *buf, size_t len, int timeout_ms)
{
    return scan_io_result(usb_scanner_read(buf, len, (uint32_t)timeout_ms));
}

static void scan_sleep(void *ctx, int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool http_handler(void *ctx, airprint_http_t *h, const char *method, const char *path)
{
    return scan_service_http(h, method, path);
}

// Most of the PSRAM, for scans (and the dev mode print capture)
static void alloc_shared_buf(void)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t size = largest > PSRAM_RESERVE + SHARED_BUF_MIN ? largest - PSRAM_RESERVE : 0;
    uint8_t *buf = size ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : NULL;
    sbuf_init(buf, buf ? size : 0);
    if (buf)
        ESP_LOGI(TAG, "shared buffer: %u KB of PSRAM (%u KB PSRAM left)", (unsigned)(size / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    else
        ESP_LOGW(TAG, "no PSRAM for the shared buffer: scanning disabled");
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
    snprintf(adminurl, sizeof(adminurl), "http://%s.local/", hostname);
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
        {"Scan", "T"},     // the eSCL scanner below, same UUID
        {"Fax", "F"},
    };
    err = mdns_service_add(PRINTER_NAME, "_ipp", "_tcp", IPP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK)
        return err;
    // "_universal" is what iOS looks for; "_print" is IPP Everywhere
    mdns_service_subtype_add_for_host(PRINTER_NAME, "_ipp", "_tcp", NULL, "_universal");
    mdns_service_subtype_add_for_host(PRINTER_NAME, "_ipp", "_tcp", NULL, "_print");

    // eSCL scanner (AirScan): macOS Image Capture / Preview, sane-airscan, Windows
    char icon[96];
    snprintf(icon, sizeof(icon), "http://%s.local/icon.png", hostname);
    mdns_txt_item_t scan_txt[] = {
        {"txtvers", "1"},
        {"vers", "2.0"},
        {"rs", "eSCL"},
        {"ty", s_cfg.make_model},
        {"note", ""},
        {"pdl", "image/jpeg"},
        {"cs", "color"},
        {"is", "platen"},
        {"duplex", "F"},
        {"UUID", s_uuid},
        {"adminurl", adminurl},
        {"representation", icon},
    };
    err = mdns_service_add(PRINTER_NAME, "_uscan", "_tcp", WEB_PORT, scan_txt, sizeof(scan_txt) / sizeof(scan_txt[0]));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "cannot announce the scanner: %s", esp_err_to_name(err));
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
        .web_port = WEB_PORT,
        .get_status = get_status,
        .job_open = job_open,
        .job_write = job_write,
        .job_sync = job_sync,
        .job_close = job_close,
        .job_cancel = job_cancel,
        .job_state = job_state,
        .identify = identify,
    };
    alloc_shared_buf();
    s_scan_cfg = (scan_config_t){
        .available = scan_available,
        .open = scan_open,
        .close = scan_close,
        .io = {.write = scan_write, .read = scan_read, .sleep_ms = scan_sleep},
        .state = job_state,
        .make_model = "Canon MG2500 series",
        .uuid = s_uuid,
    };
    s_cfg.http_handler = http_handler;
    s_cfg.home_html = SCAN_HOME_HTML;
    if (strcmp(MODE, "dev") == 0) {
        ESP_LOGW(TAG, "MODE=dev: AirPrint jobs are converted but NOT printed, scans come from an emulated scanner");
        s_cfg.get_status = dev_get_status;
        s_cfg.job_open = dev_job_open;
        s_cfg.job_write = dev_job_write;
        s_cfg.job_sync = dev_job_sync;
        s_cfg.job_close = dev_job_close;
        s_cfg.job_cancel = NULL;
        s_cfg.http_handler = dev_http;
        if (fake_scanner_init(&s_fake_scanner, DEV_SCANNER_RATE) == 0) {
            s_scan_cfg.available = dev_scan_available;
            s_scan_cfg.open = dev_scan_open;
            s_scan_cfg.close = dev_scan_close;
            s_scan_cfg.io = fake_scanner_io(&s_fake_scanner);
        }
    }
    scan_service_init(&s_scan_cfg);
    if (airprint_start(&s_cfg) != 0)
        return ESP_FAIL;
    return start_mdns(hostname);
}
