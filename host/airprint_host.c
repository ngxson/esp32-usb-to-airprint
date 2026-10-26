// Runs the AirPrint server on the development machine for testing with
// ipptool / CUPS. Each job's printer stream is written to out/job-<n>.prn.
// The web UI is at http://localhost:<port>/; scanning uses the emulated
// scanner. SIGUSR1 presses the device's button (a kept scan).
//
//   ./airprint_host [port]          (default 8631)
//   FAKE_SCANNER_KBPS=800           emulated USB rate (0: no delays)
//   SCAN_BUF_KB=7680                size of the shared buffer (the ESP32's PSRAM)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "airprint.h"
#include "events.h"
#include "printer_driver.h"
#include "fake_scanner.h"
#include "oled_ui.h"
#include "scan_service.h"
#include "shared_buf.h"
#include "webui.h"

static FILE *s_out;
static int s_job;

static void get_status(void *ctx, airprint_status_t *st)
{
    st->connected = true;
    st->nsupplies = 2;
    st->supply[0].name = "Black";
    st->supply[0].color = "#000000";
    st->supply[0].level = 60;
    st->supply[1].name = "Color";
    st->supply[1].color = "#00FFFF#FF00FF#FFFF00";
    st->supply[1].level = 5;
    st->reasons[st->nreasons++] = "marker-supply-low-warning";
}

static int job_open(void *ctx)
{
    if (scan_service_active())
        return -1;      // the device is scanning
    char path[64];
    snprintf(path, sizeof(path), "out/job-%d.prn", ++s_job);
    s_out = fopen(path, "wb");
    fprintf(stderr, "host: job output -> %s\n", path);
    return s_out ? 0 : -1;
}

static int s_slow_us;    // AIRPRINT_HOST_SLOW_US: delay per write, emulates a slow printer

static int job_write(void *ctx, const uint8_t *data, size_t len)
{
    if (s_slow_us)
        usleep(s_slow_us);
    return fwrite(data, 1, len, s_out) == len ? 0 : -1;
}

static int job_sync(void *ctx, const char *what)
{
    fprintf(stderr, "host: sync %s\n", what);
    return 0;
}

static void job_close(void *ctx, bool ok)
{
    fclose(s_out);
    s_out = NULL;
    fprintf(stderr, "host: job closed (%s)\n", ok ? "ok" : "printer error");
}

static void identify(void *ctx)
{
    fprintf(stderr, "host: identify (LED would blink)\n");
}

// --- emulated scanner
static fake_scanner_t s_scanner;

static int scan_open(void *ctx)
{
    if (s_out)
        return -1;      // printing
    fprintf(stderr, "host: scanner opened\n");
    return 0;
}

static void scan_close(void *ctx)
{
    fprintf(stderr, "host: scanner closed (%d protocol violations%s%s)\n", s_scanner.errors,
            s_scanner.errors ? ", first: " : "", s_scanner.error);
}

// GET /dev/oled.pbm: the device's status screen for the current state
static void send_oled(airprint_http_t *h)
{
    static oled_state_t st;
    memset(&st, 0, sizeof(st));
    st.name = "airprint-host";
    st.wifi_configured = true;
    strcpy(st.ip, "127.0.0.1");
    st.started = airprint_get_activity(&st.print) == 0;
    scan_service_get_info(&st.scan);
    uint8_t fb[OLED_FB_SIZE];
    oled_ui_render(&st, fb);
    char pbm[16 + OLED_FB_SIZE];
    int n = snprintf(pbm, sizeof(pbm), "P4\n%d %d\n", OLED_WIDTH, OLED_HEIGHT);
    for (int y = 0; y < OLED_HEIGHT; y++)
        for (int x = 0; x < OLED_WIDTH; x += 8) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++)
                b |= (uint8_t)((fb[(y >> 3) * OLED_WIDTH + x + k] >> (y & 7) & 1) << (7 - k));
            pbm[n++] = (char)b;
        }
    airprint_http_send(h, "200 OK", "image/x-portable-bitmap", pbm, (size_t)n);
}

static bool http_handler(void *ctx, airprint_http_t *h, const char *method, const char *path)
{
    fprintf(stderr, "host: %s %s (Host: %s)\n", method, path, airprint_http_host(h));
    if (!strcmp(method, "GET") && !strcmp(path, "/dev/oled.pbm")) {
        send_oled(h);
        return true;
    }
    return scan_service_http(h, method, path) || events_http(h, method, path) || webui_http(h, method, path);
}

static volatile sig_atomic_t s_button;

static void on_usr1(int sig)
{
    s_button = 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    static airprint_config_t cfg = {
        .name = "Canon MG2500 (host test)",
        .make_model = "Canon MG2500 series",
        .uuid = "a4d1e9b2-0b6f-4c3e-9d2a-5e8f7c6b1a00",
        .device_id = "MFG:Canon;MDL:MG2500 series;CMD:URF,PWG;",
        .port = 8631,
        .get_status = get_status,
        .job_open = job_open,
        .job_write = job_write,
        .job_sync = job_sync,
        .job_close = job_close,
        .identify = identify,
        .http_handler = http_handler,
        .home_html = SCAN_HOME_HTML,
    };
    if (argc > 1)
        cfg.port = atoi(argv[1]);
    // PRINTER_DRIVER=<id> picks another driver (components/printers/printers.c)
    cfg.driver = printer_driver_find(getenv("PRINTER_DRIVER") ? getenv("PRINTER_DRIVER") : "canon-mg2500");
    if (!cfg.driver) {
        fprintf(stderr, "unknown PRINTER_DRIVER\n");
        return 1;
    }
    if (getenv("AIRPRINT_HOST_SLOW_US"))
        s_slow_us = atoi(getenv("AIRPRINT_HOST_SLOW_US"));
    unsigned kbps = getenv("FAKE_SCANNER_KBPS") ? (unsigned)atoi(getenv("FAKE_SCANNER_KBPS")) : 800;
    size_t buf_kb = getenv("SCAN_BUF_KB") ? (size_t)atoi(getenv("SCAN_BUF_KB")) : 7680;
    if (fake_scanner_init(&s_scanner, kbps * 1024) != 0)
        return 1;
    sbuf_init(malloc(buf_kb * 1024), buf_kb * 1024);
    static scan_config_t scfg = {.open = scan_open, .close = scan_close, .make_model = "Canon MG2500 series",
                                 .uuid = "a4d1e9b2-0b6f-4c3e-9d2a-5e8f7c6b1a00"};
    scfg.io = fake_scanner_io(&s_scanner);
    scan_service_init(&scfg);
    fprintf(stderr, "host: scanner emulated at %u KB/s, %u KB buffer\n", kbps, (unsigned)buf_kb);
    events_add_source("printer", airprint_status_json, NULL);
    events_add_source("scan", scan_service_status_json, NULL);
    if (airprint_start(&cfg) != 0 || events_start() != 0)
        return 1;
    signal(SIGUSR1, on_usr1);
    for (;;) {
        pause();
        if (s_button) {
            s_button = 0;
            const char *why;
            if (scan_service_start_button(&why))
                fprintf(stderr, "host: button: %s\n", why);
        }
    }
}
