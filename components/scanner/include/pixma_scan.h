// Flatbed scanning on the Canon PIXMA MG2500 series (USB 04a9:176d,
// interface 0), following the SANE pixma backend (pixma_mp150.c, where the
// MG2500 is a "generation 5" device). Portable C: the platform supplies the
// bulk transfers through pixma_io_t.
//
// Session (all commands are request/response on the bulk endpoints):
//   XML StartJob + ModeShift (IVEC, servicetype "scan")
//   start session (retried while busy), gamma table x3, scan parameters,
//   start scan, poll status until calibrated, read image blocks until the
//   end-of-image flags, abort session, XML EndJob.
// Image data: 8-bit RGB lines, padded to a multiple of 32 pixels.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIXMA_OK 0
#define PIXMA_EIO (-1)
#define PIXMA_ETIMEDOUT (-2)
#define PIXMA_EPROTO (-3)
#define PIXMA_EBUSY (-4)
#define PIXMA_ECANCELED (-5)
#define PIXMA_ENOMEM (-6)
#define PIXMA_EINVAL (-7)

// Scan area of the MG2500 in 1/75 inch (8.5 x 11.69 in)
#define PIXMA_MAX_WIDTH_75 638
#define PIXMA_MAX_HEIGHT_75 877
#define PIXMA_MAX_DPI 600

typedef struct {
    // Bulk OUT: returns len, or < 0 (PIXMA_ETIMEDOUT / PIXMA_EIO).
    int (*write)(void *ctx, const uint8_t *data, size_t len, int timeout_ms);
    // Bulk IN, one USB transfer: up to len bytes (fewer at the end of what
    // the device sends, 0 for a zero-length packet), or < 0. len is a
    // multiple of 64 except for the last piece of a response, and at most
    // PIXMA_READ_CHUNK.
    int (*read)(void *ctx, uint8_t *buf, size_t len, int timeout_ms);
    void (*sleep_ms)(void *ctx, int ms);
    void *ctx;
} pixma_io_t;

#define PIXMA_READ_CHUNK 8192

typedef struct {
    int dpi;                    // 75, 150, 300 or 600
    int x, y, w, h;             // scan area in pixels at dpi
    bool calibrate;             // calibrate before scanning (SANE's default: the first scan of a session)
} pixma_params_t;

// One scanned line: w pixels of 8-bit RGB. Return non-zero to stop the scan.
typedef int (*pixma_line_fn)(void *user, int y, const uint8_t *rgb);

// Runs a complete scan. Always closes the session and the job, also after
// errors and cancels (the scanner otherwise stays in "PCScanning" state).
// Returns PIXMA_OK when the whole image was read; fewer than h lines may
// have been delivered on errors.
int pixma_scan(const pixma_io_t *io, const pixma_params_t *p, pixma_line_fn line, void *user,
               volatile bool *cancel);

const char *pixma_strerror(int err);

#ifdef __cplusplus
}
#endif
