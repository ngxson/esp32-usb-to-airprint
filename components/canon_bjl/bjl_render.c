#include "bjl_render.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// cycle counters per rendering stage (read with bjl_render_profile)
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#define CYCLES() ((uint32_t)esp_cpu_get_cycle_count())
#else
#define CYCLES() 0u
#endif
static uint64_t s_prof[4];      // convert, dither, add_line, lines

void bjl_render_profile(uint64_t out[4], bool reset)
{
    memcpy(out, s_prof, sizeof(s_prof));
    if (reset)
        memset(s_prof, 0, sizeof(s_prof));
}

// Ink dots spread on plain paper, so mid tones print darker than their
// nominal coverage. Correction: out = v * (1 - g * (1 - v)), which leaves 0%
// and 100% alone and lightens mid tones by up to g/4. Tune after real prints.
#define DOT_GAIN_CMY 0.25f
#define DOT_GAIN_K 0.15f

BJL_HOT void bjl_rgb_to_cmyk(uint8_t r, uint8_t g, uint8_t b, uint8_t cmyk[4])
{
    int c = 255 - r, m = 255 - g, y = 255 - b;
    // full grey component replacement: neutral and dark tones go to the
    // black cartridge (sharp black text, less colour ink)
    int k = c < m ? (c < y ? c : y) : (m < y ? m : y);
    if (k == 255) {
        cmyk[0] = cmyk[1] = cmyk[2] = 0;
    } else {
        // inks multiply: (1-C)(1-K) = 1-c  =>  C = (c - k) / (1 - k)
        cmyk[0] = (uint8_t)((c - k) * 255 / (255 - k));
        cmyk[1] = (uint8_t)((m - k) * 255 / (255 - k));
        cmyk[2] = (uint8_t)((y - k) * 255 / (255 - k));
    }
    cmyk[3] = (uint8_t)k;
}

// One non-white RGB pixel; kept out of line so the white-skipping loop that
// calls it stays small (register pressure on Xtensa).
static BJL_HOT __attribute__((noinline)) void ink_pixel(bjl_render_t *r, int x, const uint8_t *p, unsigned any[4],
                                                       int *x_lo, int *x_hi)
{
    uint8_t v[4];
    bjl_rgb_to_cmyk(p[0], p[1], p[2], v);
    any[0] |= r->density[0][x] = r->lut_cmy[v[0]];
    any[1] |= r->density[1][x] = r->lut_cmy[v[1]];
    any[2] |= r->density[2][x] = r->lut_cmy[v[2]];
    any[3] |= r->density[3][x] = r->lut_k[v[3]];
    if (x < *x_lo)
        *x_lo = x;
    *x_hi = x;
}

int bjl_render_init(bjl_render_t *r, bjl_job_t *job)
{
    memset(r, 0, sizeof(*r));
    r->job = job;
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++) {
        r->density[ch] = BJL_MALLOC_FAST(job->width);
        r->packed[ch] = BJL_MALLOC_FAST(job->line_bytes[ch]);
        if (!r->density[ch] || !r->packed[ch]) {
            bjl_render_free(r);
            return -1;
        }
    }
    for (int i = 0; i < 256; i++) {
        float v = i / 255.0f;
        r->lut_cmy[i] = (uint8_t)(255.0f * v * (1.0f - DOT_GAIN_CMY * (1.0f - v)) + 0.5f);
        r->lut_k[i] = (uint8_t)(255.0f * v * (1.0f - DOT_GAIN_K * (1.0f - v)) + 0.5f);
    }
    return 0;
}

void bjl_render_free(bjl_render_t *r)
{
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++) {
        free(r->density[ch]);
        free(r->packed[ch]);
        r->density[ch] = r->packed[ch] = NULL;
    }
}

void bjl_render_page(bjl_render_t *r, int src_w, int src_h, int bytes_pp)
{
    r->src_w = src_w;
    r->src_h = src_h;
    r->bytes_pp = bytes_pp;
}

BJL_HOT int bjl_render_line(bjl_render_t *r, int src_y, const uint8_t *px)
{
    bjl_job_t *j = r->job;
    int y = src_y - j->top;
    if (y < 0 || y >= j->height)
        return j->err;
    // the area may have been padded already if lines were skipped
    while (j->lines_done < y && !j->err)
        bjl_page_add_line(j, NULL);
    if (j->lines_done != y)
        return j->err;

    if (!px)    // white line
        return bjl_page_add_line(j, NULL);

    uint8_t *k = r->density[3];
    int n = r->src_w - j->left;     // source pixels available in the area
    if (n > j->width)
        n = j->width;
    if (n < 0)
        n = 0;
    uint32_t t0 = CYCLES();
    s_prof[3]++;
    const uint8_t *p = px + (size_t)j->left * r->bytes_pp;
    unsigned any[4] = {0, 0, 0, 0};     // OR of each channel: skip inks with nothing to print
    int x_lo = n, x_hi = -1;            // span of inked pixels: only that part is dithered
    if (r->bytes_pp == 3) {
        for (int ch = 0; ch < 4; ch++)
            memset(r->density[ch], 0, j->width);
        int x = 0;
        while (x < n) {
            // skip white: pixel by pixel to a word boundary, then 4 pixels
            // (3 aligned words) at a time
            while (x < n && ((uintptr_t)p & 3) && (p[0] & p[1] & p[2]) == 0xff) {
                x++;
                p += 3;
            }
            if (!((uintptr_t)p & 3)) {
                while (x + 4 <= n) {
                    const uint32_t *w = (const uint32_t *)p;
                    if ((w[0] & w[1] & w[2]) != 0xffffffffu)
                        break;
                    x += 4;
                    p += 12;
                }
            }
            for (int stop = x + 4; x < n && x < stop; x++, p += 3)
                if ((p[0] & p[1] & p[2]) != 0xff)
                    ink_pixel(r, x, p, any, &x_lo, &x_hi);
        }
    } else {
        memset(k, 0, j->width);
        for (int x = 0; x < n; x++) {
            if (p[x] == 0xff)
                continue;
            any[3] |= k[x] = r->lut_k[255 - p[x]];
            if (x < x_lo)
                x_lo = x;
            x_hi = x;
        }
    }
    uint32_t t1 = CYCLES();
    s_prof[0] += t1 - t0;
    if (x_hi < 0)
        return bjl_page_add_line(j, NULL);
    // dither [x0, x1): x0 on a multiple of 8 keeps the matrix phase and
    // whole output bytes for both 1 bit and 2 bit pixels
    int x0 = x_lo & ~7, x1 = x_hi + 1;
    const uint8_t *planes[BJL_NUM_CHANNELS];
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++) {
        if (!any[ch]) {
            planes[ch] = NULL;      // blank line for this ink
            continue;
        }
        int ppb = 8 / j->mode->bits[ch];    // pixels per output byte
        memset(r->packed[ch], 0, j->line_bytes[ch]);
        bjl_dither_line(r->density[ch] + x0, x1 - x0, j->mode->bits[ch], y, r->packed[ch] + x0 / ppb,
                        (x1 - x0 + ppb - 1) / ppb);
        planes[ch] = r->packed[ch];
    }
    uint32_t t2 = CYCLES();
    s_prof[1] += t2 - t1;
    int ret = bjl_page_add_line(j, planes);
    s_prof[2] += CYCLES() - t2;
    return ret;
}
