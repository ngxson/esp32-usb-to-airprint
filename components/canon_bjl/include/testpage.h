// Procedural colour test page, rendered one raster line at a time so it
// needs only a few line buffers (no full-page framebuffer).
#pragma once

#include "canon_bjl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int dpi;
    int width;                                  // dots, = job->width
    int height;                                 // dots, = job->height
    const uint8_t *bits;                        // job->mode->bits
    uint8_t *density[BJL_NUM_CHANNELS];         // 0..255 ink density per pixel
    uint8_t *packed[BJL_NUM_CHANNELS];          // dithered, packed output line
    int line_bytes[BJL_NUM_CHANNELS];
    char info1[64];
    char info2[64];
} testpage_t;

// Second info line used by the firmware dry run and the host harness, so both
// render an identical page (and stream CRC).
#define TESTPAGE_DRYRUN_INFO "DRY RUN"

// "MODE: 600x600dpi PLAIN PAPER, COLOR" for the first info line.
void testpage_format_mode(const bjl_job_t *job, char *out, size_t out_len);

// info1/info2 are printed under the title (may be NULL).
int testpage_init(testpage_t *tp, const bjl_job_t *job, const char *info1, const char *info2);
void testpage_free(testpage_t *tp);

// Renders raster line y (0 = top of printable area) and returns the C,M,Y,K
// planes ready for bjl_page_add_line(). Valid until the next call.
const uint8_t *const *testpage_render_line(testpage_t *tp, int y);

#ifdef __cplusplus
}
#endif
