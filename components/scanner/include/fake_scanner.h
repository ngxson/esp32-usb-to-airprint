// Emulated MG2500 scanner for tests and dev mode: speaks the device side of
// the pixma protocol through a pixma_io_t, checks every command the host
// sends (order, lengths, checksums, parameters, unread replies) and returns
// a synthetic test page in irregular blocks. Portable C.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixma_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // --- settings
    unsigned bytes_per_s;       // 0: no delays at all; else emulated transfer rate, and real sleeps
    int busy_starts;            // answer "busy" to this many start-session commands
    int fail_at_block;          // > 0: fail this image block (1 = first)
    // --- results
    int errors;                 // protocol violations
    char error[160];            // the first one
    int scans;                  // scans that delivered the whole image and were closed
    bool job_open, session_open;
    int dpi, x, y, wx, h;       // last scan parameters (x as sent, a multiple of 32)
    uint64_t image_bytes;       // image data sent in the last scan
    // --- internal
    int stage;
    int gamma_count, status_polls, block_no;
    uint64_t total;
    uint32_t rng;
    uint8_t *resp;
    size_t resp_len, resp_pos;
    bool zlp_pending;
    uint8_t *raw;               // current raw line
    int raw_y;
    size_t raw_pos;
} fake_scanner_t;

int fake_scanner_init(fake_scanner_t *f, unsigned bytes_per_s);
void fake_scanner_free(fake_scanner_t *f);
// I/O hooks that talk to this scanner
pixma_io_t fake_scanner_io(fake_scanner_t *f);

// The test page at the given dpi: pixel (x, y) of the full scan area.
void fake_scanner_pixel(int dpi, int x, int y, uint8_t rgb[3]);

#ifdef __cplusplus
}
#endif
