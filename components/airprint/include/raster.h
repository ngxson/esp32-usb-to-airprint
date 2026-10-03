// Streaming decoder for the raster formats AirPrint / IPP Everywhere clients
// send: Apple URF ("UNIRAST") and PWG raster ("RaS2"). Delivers one decoded
// line at a time, so a page never has to fit in memory.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int page;           // 1-based
    int width, height;  // pixels
    int dpi;            // x and y resolution (must be equal)
    int bytes_pp;       // 1 = 8-bit gray, 3 = 8-bit sRGB (white = 0xff)
} raster_page_t;

typedef struct {
    // return 0 to continue, non-zero to stop decoding (returned by raster_decode)
    int (*page_begin)(void *user, const raster_page_t *pg);
    // pixels == NULL: the line is entirely white
    int (*line)(void *user, int y, const uint8_t *pixels);
    int (*page_end)(void *user, const raster_page_t *pg);
} raster_cb_t;

// Reads up to n bytes; returns bytes read (> 0), 0 at end of data, < 0 on error.
typedef int (*raster_read_fn)(void *ctx, uint8_t *buf, size_t n);

#define RASTER_ERR_IO -1        // read error / truncated stream
#define RASTER_ERR_FORMAT -2    // not URF/PWG, or corrupt data
#define RASTER_ERR_UNSUPPORTED -3   // valid but unsupported (bit depth, colour space)
#define RASTER_ERR_NOMEM -4

// Returns number of pages decoded (>= 0), a RASTER_ERR_*, or the non-zero
// value a callback returned.
int raster_decode(raster_read_fn read, void *ctx, const raster_cb_t *cb, void *user);

const char *raster_strerror(int err);

#ifdef __cplusplus
}
#endif
