// Scanning from the browser. Pages are scanned in colour and JPEG-encoded
// line by line while the scanner delivers them, so no page is ever held raw.
//
// Two modes, both writing into the shared buffer (shared_buf.h):
//   stream  GET /scan/stream.jpg?dpi=150|300|600&paper=a4|letter
//           The response is the JPEG, sent while it is scanned. The buffer is
//           a ring between the scan and the connection; if the client falls
//           behind, the scan waits (the scanner pauses), and a client that
//           stops reading for STALL_TIMEOUT cancels it.
//   last    POST /scan/last?paper=a4|letter
//           A 150 dpi scan kept whole in the buffer; the browser can leave and
//           fetch it later from GET /scan/last.jpg (until the buffer is reused).
// Also: GET /scan (the page), GET /scan/status (JSON), POST /scan/cancel.
// One scan at a time; the device is shared with printing (open() fails while
// a print job runs, and print jobs are refused while a scan runs).
#pragma once

#include <stdbool.h>

#include "airprint.h"
#include "pixma_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool (*available)(void *ctx);       // a scanner is attached
    int (*open)(void *ctx);             // claim the device for a scan, 0 = ok
    void (*close)(void *ctx);
    pixma_io_t io;                      // bulk transfers to the scanner interface
    void (*state)(void *ctx, bool scanning);    // e.g. LED
    void *ctx;
} scan_config_t;

// The config must stay valid. The buffer comes from sbuf_init().
void scan_service_init(const scan_config_t *cfg);
// HTTP handler for /scan and /scan/...; false for other paths.
bool scan_service_http(airprint_http_t *h, const char *method, const char *path);
bool scan_service_active(void);
void scan_service_cancel(void);

// HTML for the status page
#define SCAN_HOME_HTML "<p><a href=\"/scan\">Scan a document</a></p>"

#ifdef __cplusplus
}
#endif
