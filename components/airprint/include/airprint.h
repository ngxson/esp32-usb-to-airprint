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

    // platform hooks; all optional except job_write
    void (*get_status)(void *ctx, airprint_status_t *st);
    int (*job_open)(void *ctx);                 // claim the printer, 0 = ok
    int (*job_write)(void *ctx, const uint8_t *data, size_t len);  // < 0 aborts
    int (*job_sync)(void *ctx, const char *what);   // after "StartJob"/"ModeShift"/"EndJob": flush + wait
    void (*job_close)(void *ctx, bool ok);
    void (*job_cancel)(void *ctx);              // asynchronous cancel of the running job
    void (*job_state)(void *ctx, bool printing);    // e.g. LED
    void (*identify)(void *ctx);                // Identify-Printer (blink LED)
    // Extra GET resources (e.g. debug downloads). Return true and fill
    // data/len/type to serve one; the data must stay valid while sending.
    bool (*get_file)(void *ctx, const char *path, const uint8_t **data, size_t *len, const char **type);
    void *ctx;
} airprint_config_t;

// Starts the listener thread. The config must stay valid.
int airprint_start(const airprint_config_t *cfg);

#ifdef __cplusplus
}
#endif
