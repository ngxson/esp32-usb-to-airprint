// Procedural A4 colour test page. Layout is in millimetres relative to the
// top-left corner of the printable area, so it is resolution independent.
#include "testpage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 5x7 font, rows top to bottom, bit 4 = leftmost column. Lowercase letters are
// drawn as uppercase.
typedef struct {
    char ch;
    uint8_t rows[7];
} glyph_t;

static const glyph_t FONT[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}}, {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
};

static const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
        if (FONT[i].ch == c)
            return FONT[i].rows;
    return NULL;    // space and unknown characters are blank
}

// ---------------------------------------------------------------------------
// drawing helpers, all operate on the current line only

typedef struct {
    testpage_t *tp;
    int y;          // current line in dots
} ctx_t;

static int mm(const testpage_t *tp, float v)
{
    return (int)(v * tp->dpi / 25.4f + 0.5f);
}

static void span(ctx_t *c, int ch, int x0, int x1, uint8_t v)
{
    testpage_t *tp = c->tp;
    if (x0 < 0)
        x0 = 0;
    if (x1 > tp->width)
        x1 = tp->width;
    if (x1 > x0)
        memset(tp->density[ch] + x0, v, x1 - x0);
}

// filled rectangle with CMYK densities (0..255), coordinates in mm
static void rect(ctx_t *c, float x0, float y0, float x1, float y1, uint8_t C, uint8_t M, uint8_t Y, uint8_t K)
{
    testpage_t *tp = c->tp;
    if (c->y < mm(tp, y0) || c->y >= mm(tp, y1))
        return;
    int a = mm(tp, x0), b = mm(tp, x1);
    const uint8_t v[4] = {C, M, Y, K};
    for (int ch = 0; ch < 4; ch++)
        if (v[ch])
            span(c, ch, a, b, v[ch]);
}

static void kbox(ctx_t *c, float x0, float y0, float x1, float y1, float t)
{
    rect(c, x0, y0, x1, y0 + t, 0, 0, 0, 255);
    rect(c, x0, y1 - t, x1, y1, 0, 0, 0, 255);
    rect(c, x0, y0, x0 + t, y1, 0, 0, 0, 255);
    rect(c, x1 - t, y0, x1, y1, 0, 0, 0, 255);
}

// horizontal 0..100% ramp on the channels set in mask
static void ramp(ctx_t *c, float x0, float y0, float x1, float y1, unsigned mask)
{
    testpage_t *tp = c->tp;
    if (c->y < mm(tp, y0) || c->y >= mm(tp, y1))
        return;
    int a = mm(tp, x0), b = mm(tp, x1);
    for (int x = a; x < b && x < tp->width; x++) {
        uint8_t v = (uint8_t)((x - a) * 255 / (b - a - 1));
        for (int ch = 0; ch < 4; ch++)
            if (mask & (1u << ch))
                tp->density[ch][x] = v;
    }
}

// full-saturation hue sweep built from C, M, Y
static void hue_bar(ctx_t *c, float x0, float y0, float x1, float y1)
{
    testpage_t *tp = c->tp;
    if (c->y < mm(tp, y0) || c->y >= mm(tp, y1))
        return;
    int a = mm(tp, x0), b = mm(tp, x1);
    for (int x = a; x < b && x < tp->width; x++) {
        int h = (x - a) * 1530 / (b - a);       // 6 sectors of 255
        int f = h % 255, s = h / 255;
        // RGB of the hue, then CMY = 255 - RGB
        int r, g, bl;
        switch (s) {
        case 0: r = 255; g = f; bl = 0; break;
        case 1: r = 255 - f; g = 255; bl = 0; break;
        case 2: r = 0; g = 255; bl = f; break;
        case 3: r = 0; g = 255 - f; bl = 255; break;
        case 4: r = f; g = 0; bl = 255; break;
        default: r = 255; g = 0; bl = 255 - f; break;
        }
        tp->density[BJL_C][x] = 255 - r;
        tp->density[BJL_M][x] = 255 - g;
        tp->density[BJL_Y][x] = 255 - bl;
    }
}

// text in K, cap height h_mm, top-left at (x, y) mm. Returns width in mm.
static float text(ctx_t *c, float x, float y, float h_mm, const char *s)
{
    testpage_t *tp = c->tp;
    float unit = h_mm / 7.0f;                   // one font pixel in mm
    float adv = unit * 6.0f;
    int top = mm(tp, y);
    int row_px = mm(tp, h_mm);
    if (s && c->y >= top && c->y < top + row_px) {
        int row = (c->y - top) * 7 / row_px;
        for (int i = 0; s[i]; i++) {
            const uint8_t *g = glyph_rows(s[i]);
            if (!g)
                continue;
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col)) {
                    float cx = x + i * adv + col * unit;
                    span(c, BJL_K, mm(tp, cx), mm(tp, cx + unit), 255);
                }
        }
    }
    return s ? strlen(s) * adv : 0;
}

// ---------------------------------------------------------------------------
// page content

static void draw(ctx_t *c)
{
    testpage_t *tp = c->tp;
    const float W = tp->width * 25.4f / tp->dpi;
    const float H = tp->height * 25.4f / tp->dpi;
    const float L = 12.0f, R = W - 12.0f;       // content left/right edge

    kbox(c, 3, 3, W - 3, H - 3, 0.5f);          // frame around printable area

    text(c, L, 10, 7, "ESP32-S3 PRINT SERVER");
    text(c, L, 20, 4, "CANON MG2500 SERIES - TEST PAGE - A4");
    text(c, L, 27, 3, tp->info1);
    text(c, L, 32, 3, tp->info2);

    // solid inks and two-ink mixes
    text(c, L, 41, 3, "SOLID INKS");
    static const struct { const char *n; uint8_t c, m, y, k; } patch[] = {
        {"C", 255, 0, 0, 0},   {"M", 0, 255, 0, 0},   {"Y", 0, 0, 255, 0},   {"K", 0, 0, 0, 255},
        {"R", 0, 255, 255, 0}, {"G", 255, 0, 255, 0}, {"B", 255, 255, 0, 0},
    };
    const float pw = (R - L - 6 * 3.0f) / 7;
    for (int i = 0; i < 7; i++) {
        float x = L + i * (pw + 3.0f);
        rect(c, x, 46, x + pw, 66, patch[i].c, patch[i].m, patch[i].y, patch[i].k);
        text(c, x + pw / 2 - 1.2f, 68, 3, patch[i].n);
    }

    // single-ink ramps
    text(c, L, 77, 3, "GRADIENTS 0 - 100%");
    for (int ch = 0; ch < 4; ch++) {
        float y = 82 + ch * 10.0f;
        ramp(c, L + 6, y, R, y + 8, 1u << ch);
        text(c, L, y + 2.5f, 3, (const char *[]){"C", "M", "Y", "K"}[ch]);
    }
    // 10% ticks under the ramps
    for (int i = 0; i <= 10; i++) {
        float x = L + 6 + (R - L - 6) * i / 10.0f;
        if (i == 10)
            x -= 0.3f;
        rect(c, x, 121, x + 0.3f, 124, 0, 0, 0, 255);
    }

    // hue sweep and grey balance
    text(c, L, 129, 3, "COLOR MIX: HUE / CMY GREY / K GREY");
    hue_bar(c, L, 134, R, 148);
    ramp(c, L, 150, R, 157, (1u << BJL_C) | (1u << BJL_M) | (1u << BJL_Y));
    ramp(c, L, 159, R, 166, 1u << BJL_K);

    // fine lines: groups of 1..4 dot wide vertical and horizontal lines
    text(c, L, 172, 3, "LINES 1 2 3 4 DOTS");
    for (int w = 1; w <= 4; w++) {
        float gx = L + (w - 1) * ((R - L) / 4);
        int top = mm(tp, 178), bot = mm(tp, 194);
        if (c->y >= top && c->y < bot) {
            int x = mm(tp, gx);
            for (int k = 0; k < 8; k++, x += 4 * w)    // vertical lines, gap = 3w
                span(c, BJL_K, x, x + w, 255);
        }
        int hx0 = mm(tp, gx + 12), hx1 = mm(tp, gx + 40);
        for (int k = 0; k < 8; k++) {                    // horizontal lines
            int ly = top + k * 4 * w;
            if (c->y >= ly && c->y < ly + w)
                span(c, BJL_K, hx0, hx1, 255);
        }
    }

    // millimetre ruler, 150 mm long: check that the print scale is exact
    text(c, L, 200, 3, "RULER 150 MM");
    rect(c, L, 212, L + 150.0f, 212.3f, 0, 0, 0, 255);
    for (int i = 0; i <= 150; i++) {
        float x = L + i;
        float len = (i % 10 == 0) ? 5.0f : (i % 5 == 0) ? 3.5f : 2.0f;
        rect(c, x - 0.1f, 212 - len, x + 0.1f, 212, 0, 0, 0, 255);
        if (i % 10 == 0) {
            char num[4];
            int n = i / 10, p = 0;
            if (n >= 10)
                num[p++] = '0' + n / 10;
            num[p++] = '0' + n % 10;
            num[p] = 0;
            text(c, x - p * 0.6f, 214, 2, num);
        }
    }

    // ink registration: thin K cross centred on a thicker C+M+Y cross
    const float cx = R - 12, cy = 208;
    rect(c, cx - 10, cy - 0.5f, cx + 10, cy + 0.5f, 255, 255, 255, 0);
    rect(c, cx - 0.5f, cy - 10, cx + 0.5f, cy + 10, 255, 255, 255, 0);
    rect(c, cx - 10, cy - 0.15f, cx + 10, cy + 0.15f, 0, 0, 0, 255);
    rect(c, cx - 0.15f, cy - 10, cx + 0.15f, cy + 10, 0, 0, 0, 255);

    // nozzle check: per ink a staircase of 1-dot-high dashes, one dash per
    // raster line, so a clogged nozzle shows up as a missing step
    text(c, L, 226, 3, "NOZZLE CHECK  C  M  Y  K");
    {
        int top = mm(tp, 232), steps = 32;
        int bh = steps * 2;                     // every other line
        if (c->y >= top && c->y < top + bh * 4 && ((c->y - top) & 1) == 0) {     // 4 cycles
            int step = ((c->y - top) / 2) % steps;
            float bw = (R - L - 3 * 4.0f) / 4;
            for (int ch = 0; ch < 4; ch++) {
                int x0 = mm(tp, L + ch * (bw + 4.0f));
                int dash = mm(tp, bw) / steps;
                span(c, ch, x0 + step * dash, x0 + (step + 1) * dash - 1, 255);
            }
        }
    }

    text(c, L, H - 22, 5, "IF YOU CAN READ THIS, IT WORKS!");
}

// ---------------------------------------------------------------------------

void testpage_format_mode(const bjl_job_t *job, char *out, size_t out_len)
{
    snprintf(out, out_len, "MODE: %s PLAIN PAPER, COLOR", job->mode->name);
}

int testpage_init(testpage_t *tp, const bjl_job_t *job, const char *info1, const char *info2)
{
    memset(tp, 0, sizeof(*tp));
    tp->dpi = job->mode->dpi;
    tp->width = job->width;
    tp->height = job->height;
    tp->bits = job->mode->bits;
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++) {
        tp->line_bytes[ch] = job->line_bytes[ch];
        tp->density[ch] = malloc(tp->width);
        tp->packed[ch] = malloc(tp->line_bytes[ch]);
        if (!tp->density[ch] || !tp->packed[ch]) {
            testpage_free(tp);
            return -1;
        }
    }
    if (info1)
        strncpy(tp->info1, info1, sizeof(tp->info1) - 1);
    if (info2)
        strncpy(tp->info2, info2, sizeof(tp->info2) - 1);
    return 0;
}

void testpage_free(testpage_t *tp)
{
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++) {
        free(tp->density[ch]);
        free(tp->packed[ch]);
        tp->density[ch] = tp->packed[ch] = NULL;
    }
}

const uint8_t *const *testpage_render_line(testpage_t *tp, int y)
{
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++)
        memset(tp->density[ch], 0, tp->width);
    ctx_t c = {tp, y};
    draw(&c);
    for (int ch = 0; ch < BJL_NUM_CHANNELS; ch++)
        bjl_dither_line(tp->density[ch], tp->width, tp->bits[ch], y, tp->packed[ch], tp->line_bytes[ch]);
    return (const uint8_t *const *)tp->packed;
}
