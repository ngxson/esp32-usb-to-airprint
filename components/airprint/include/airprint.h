// AirPrint / IPP Everywhere print server: HTTP + IPP, job handling, and the
// URF/PWG raster -> Canon BJ raster pipeline. Portable (BSD sockets +
// pthreads); the platform supplies the printer connection through hooks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// URF capabilities, shared by the IPP attribute and the DNS-SD TXT record:
// 8-bit gray + sRGB, 300 and 600 dpi, plain paper, 3 print qualities.
#define AIRPRINT_URF "V1.4,CP1,W8,SRGB24,RS300-600,MT1,PQ3-4-5"
#define AIRPRINT_PDL "application/octet-stream,image/urf,image/pwg-raster"

#define AIRPRINT_MAX_SUPPLIES 4

typedef struct airprint_http airprint_http_t;   // one HTTP request/response

typedef struct {
    bool connected;             // printer attached and usable
    bool busy;                  // printer in use by something else (local test page)
    // IPP printer-state-reasons keywords, e.g. "marker-supply-low-warning"
    const char *reasons[6];
    int nreasons;
    char message[96];           // printer-state-message
    struct {
        const char *name;       // "Black", "Color"
        const char *color;      // "#000000", "#00FFFF#FF00FF#FFFF00"
        int level;              // 0..100, -2 unknown, -3 some remaining
    } supply[AIRPRINT_MAX_SUPPLIES];
    int nsupplies;
} airprint_status_t;

typedef struct {
    const char *name;           // printer-name, e.g. "Canon MG2500 (ESP32)"
    const char *make_model;     // "Canon MG2500 series"
    const char *uuid;           // "8d2f0c1e-...." (without urn:uuid:)
    const char *device_id;      // IEEE 1284 ID advertised to clients
    int port;                   // 631
    int web_port;               // the same server on a second port for browsers (80), 0 = none

    // platform hooks; all optional except job_write
    void (*get_status)(void *ctx, airprint_status_t *st);
    int (*job_open)(void *ctx);                 // claim the printer, 0 = ok
    int (*job_write)(void *ctx, const uint8_t *data, size_t len);  // < 0 aborts
    int (*job_sync)(void *ctx, const char *what);   // after "StartJob"/"ModeShift"/"EndJob": flush + wait
    void (*job_close)(void *ctx, bool ok);
    void (*job_cancel)(void *ctx);              // asynchronous cancel of the running job
    void (*job_state)(void *ctx, bool printing);    // e.g. LED
    void (*identify)(void *ctx);                // Identify-Printer (blink LED)
    // Extra HTTP resources (scanner, debug downloads). Called for GET/HEAD
    // requests and for POSTs outside /ipp; path includes the query string.
    // Return true once a response was sent with the airprint_http_* calls
    // below, false for a 404. Runs on the connection's thread.
    bool (*http_handler)(void *ctx, airprint_http_t *h, const char *method, const char *path);
    const char *home_html;      // extra HTML on the status page (e.g. a link)
    void *ctx;
} airprint_config_t;

// Starts the listener thread. The config must stay valid.
int airprint_start(const airprint_config_t *cfg);

// --- responses from http_handler
// A complete response.
void airprint_http_send(airprint_http_t *h, const char *status, const char *type, const void *body, size_t len);
// A streamed 200 response of unknown length (chunked); extra_headers are
// full "Name: value\r\n" lines or NULL. Calls return < 0 once the client is gone.
int airprint_http_stream_begin(airprint_http_t *h, const char *type, const char *extra_headers);
int airprint_http_stream_write(airprint_http_t *h, const void *data, size_t len);
int airprint_http_stream_end(airprint_http_t *h);

#ifdef __cplusplus
}
#endif
