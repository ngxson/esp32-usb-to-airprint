// Converts full-page RGB / gray raster lines (paper corner = (0,0), same dpi
// as the job) into dithered CMYK lines for the printable area of a bjl job.
#pragma once

#include <stdbool.h>

#include "canon_bjl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bjl_job_t *job;
    int src_w, src_h, bytes_pp;     // current source page
    uint8_t *density[BJL_NUM_CHANNELS];
    uint8_t *packed[BJL_NUM_CHANNELS];
    uint8_t lut_cmy[256];           // density correction (dot gain)
    uint8_t lut_k[256];
} bjl_render_t;

int bjl_render_init(bjl_render_t *r, bjl_job_t *job);
void bjl_render_free(bjl_render_t *r);

// Call after bjl_page_begin(). bytes_pp: 1 = gray, 3 = RGB (white = 255).
void bjl_render_page(bjl_render_t *r, int src_w, int src_h, int bytes_pp);
// Feed source lines in order (src_y = 0 .. src_h-1); pixels == NULL is a
// white line. Lines outside the printable area are skipped. Returns the
// job's error state.
int bjl_render_line(bjl_render_t *r, int src_y, const uint8_t *pixels);

// CPU cycles spent per stage since the last reset: convert, dither,
// compress+emit, and the number of non-white lines rendered.
void bjl_render_profile(uint64_t out[4], bool reset);

// Exposed for tests: one sRGB pixel to C, M, Y, K densities (0..255, before
// the dot gain correction).
void bjl_rgb_to_cmyk(uint8_t r, uint8_t g, uint8_t b, uint8_t cmyk[4]);

#ifdef __cplusplus
}
#endif
