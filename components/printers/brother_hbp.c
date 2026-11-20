// Brother host-based monochrome lasers (HL-L2300D and relatives): PJL job
// header, then each page as a PCL raster group of 1-bit lines in Brother's
// line compression (edits against the previous line: "substitute" and
// "repeat" commands), in blocks of up to 64 lines. The stream follows the
// brlaser CUPS driver (https://github.com/pdewacht/brlaser), which documents
// the format; no back channel status is parsed. Portable C.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

#define LINES_PER_BAND 64       // a line without reference every 64 lines
#define MAX_BLOCK 16350         // bytes of encoded lines in one "w" block
#define MAX_EDITS 254
// The printer puts the first pixel of a page 5 mm from the left and the top
// of the sheet (the corner of its printable area), measured on an HL-L2300D
// (2026-11-14, ruler page): the page is drawn from there, and what lies
// above or left of it is dropped. In 1/100 mm.
#define ORIGIN_X 500
#define ORIGIN_Y 500

typedef struct {
    const char *pjl_paper;      // @PJL SET PAPER value
} brother_paper_t;

typedef struct {
    printer_io_t io;
    const printer_driver_t *drv;
    int dpi, paper;
    int scale;                  // printer pixels per source pixel: 600 / job dpi
    int xo, yo;                 // the printer's origin on the paper, printer pixels
    int width, height;          // the printable part of the paper, printer pixels (600 dpi)
    int line_bytes;             // (width + 7) / 8
    int src_w, src_h, bytes_pp; // the current page's source raster
    int y;                      // next printer line of the page
    bool in_page;
    int err;
    uint8_t *line, *ref;        // 1-bit lines, MSB first, 1 = black
    uint8_t *enc;               // one encoded line (line_bytes + 32)
    uint8_t *block;             // MAX_BLOCK bytes
    int block_len, block_lines;
    long bytes_out;
} brother_job_t;

// ---------------------------------------------------------------------------
// output

static void out(brother_job_t *j, const void *data, size_t len)
{
    if (j->err || !len)
        return;
    if (j->io.write(j->io.ctx, data, len) < 0)
        j->err = -1;
    else
        j->bytes_out += (long)len;
}

static void outs(brother_job_t *j, const char *s)
{
    out(j, s, strlen(s));
}

static void outf(brother_job_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(brother_job_t *j, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        out(j, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

// ---------------------------------------------------------------------------
// line encoding (brlaser's line.cc)

static int put_overflow(uint8_t *o, int value)
{
    int n = 0;
    if (value < 0)
        return 0;
    while (value >= 255) {
        o[n++] = 255;
        value -= 255;
    }
    o[n++] = (uint8_t)value;
    return n;
}

// replace count bytes at offset (from the end of the previous edit) by data
static int put_substitute(uint8_t *o, int offset, const uint8_t *data, int count)
{
    int c = count - 1, n = 0;
    int off_lo = offset < 15 ? offset : 15, cnt_lo = c < 7 ? c : 7;
    o[n++] = (uint8_t)((off_lo << 3) | cnt_lo);
    n += put_overflow(o + n, offset - 15);
    n += put_overflow(o + n, c - 7);
    memcpy(o + n, data, count);
    return n + count;
}

// count (>= 2) times value at offset
static int put_repeat(uint8_t *o, int offset, int count, uint8_t value)
{
    int c = count - 2, n = 0;
    int off_lo = offset < 3 ? offset : 3, cnt_lo = c < 31 ? c : 31;
    o[n++] = (uint8_t)(0x80 | (off_lo << 5) | cnt_lo);
    n += put_overflow(o + n, offset - 3);
    n += put_overflow(o + n, c - 31);
    o[n++] = value;
    return n;
}

static bool all_zero(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

// How many bytes from line[i] to substitute: up to two bytes in a row that
// equal the reference, or three equal bytes (a repeat), or the end.
static int substitute_length(const uint8_t *line, const uint8_t *ref, int i, int end)
{
    if (i == end)
        return 0;
    int prev = i;
    for (int k = i; k + 1 < end; k++) {
        if (line[k] == ref[k] && line[k + 1] == ref[k + 1])
            return k - i;
        if (line[k] == line[k + 1] && line[k] == line[prev])
            return prev - i;
        prev = k;
    }
    return end - i;
}

// a line as edits against ref (NULL: against nothing), into o; its length
static int encode_line(const uint8_t *line, const uint8_t *ref, int len, uint8_t *o)
{
    if (all_zero(line, len)) {
        o[0] = 0xff;
        return 1;
    }
    if (!ref) {
        o[0] = 1;
        return 1 + put_substitute(o + 1, 0, line, len);
    }
    int n = 1, edits = 0;
    int end = len;
    while (end > 0 && line[end - 1] == ref[end - 1])
        end--;
    int i = 0;
    for (;;) {
        int start = i;
        while (i < end && line[i] == ref[i])
            i++;
        int offset = i - start;
        if (i == end)
            break;
        if (++edits == MAX_EDITS) {
            n += put_substitute(o + n, offset, line + i, end - i);
            break;
        }
        int s = substitute_length(line, ref, i, end);
        if (s > 0) {
            n += put_substitute(o + n, offset, line + i, s);
            i += s;
        } else {
            int r = 1;
            while (i + r < end && line[i + r] == line[i])
                r++;
            n += put_repeat(o + n, offset, r, line[i]);
            i += r;
        }
    }
    o[0] = (uint8_t)edits;
    return n;
}

static void flush_block(brother_job_t *j)
{
    if (!j->block_lines)
        return;
    outf(j, "%dw", j->block_len + 2);
    uint8_t hdr[2] = {0, (uint8_t)j->block_lines};
    out(j, hdr, 2);
    out(j, j->block, j->block_len);
    j->block_len = j->block_lines = 0;
}

// the next printer line, in j->line
static void emit_line(brother_job_t *j)
{
    bool band_start = j->y % LINES_PER_BAND == 0;
    if (band_start)
        flush_block(j);
    int n = encode_line(j->line, band_start ? NULL : j->ref, j->line_bytes, j->enc);
    if (j->block_len + n >= MAX_BLOCK) {
        flush_block(j);
        n = encode_line(j->line, NULL, j->line_bytes, j->enc);
    }
    memcpy(j->block + j->block_len, j->enc, n);
    j->block_len += n;
    j->block_lines++;
    uint8_t *t = j->ref;
    j->ref = j->line;
    j->line = t;
    j->y++;
}

// ---------------------------------------------------------------------------
// gray -> 1 bit: a clustered-dot screen (two dots per 8x8 cell, 45 degrees),
// what a laser reproduces evenly

static const uint8_t SCREEN[8][8] = {
    {24, 10, 12, 26, 35, 47, 49, 37}, {8, 0, 2, 14, 45, 59, 61, 51},  {22, 6, 4, 16, 43, 57, 63, 53},
    {30, 20, 18, 28, 33, 41, 55, 39}, {34, 46, 48, 36, 25, 11, 13, 27}, {44, 58, 60, 50, 9, 1, 3, 15},
    {42, 56, 62, 52, 23, 7, 5, 17},   {32, 40, 54, 38, 31, 21, 19, 29},
};

static void dither_line(brother_job_t *j, int y, const uint8_t *px)
{
    memset(j->line, 0, j->line_bytes);
    if (!px)
        return;
    const uint8_t *row = SCREEN[y & 7];
    int w = j->src_w * j->scale - j->xo < j->width ? j->src_w * j->scale - j->xo : j->width;
    for (int x = 0; x < w; x++) {
        int sx = (x + j->xo) / j->scale, g;
        if (j->bytes_pp == 3) {
            const uint8_t *p = px + 3 * sx;
            g = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
        } else {
            g = px[sx];
        }
        if (g < row[x & 7] * 4 + 2)
            j->line[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}

// ---------------------------------------------------------------------------
// the job

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    if (paper < 0 || paper >= d->npapers)
        paper = 0;
    brother_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    j->drv = d;
    // the printer always runs at 600 dpi; a 300 dpi job is doubled
    j->dpi = dpi;
    j->scale = dpi == 300 ? 2 : 1;
    j->paper = paper;
    j->xo = ORIGIN_X * 600 / 2540;
    j->yo = ORIGIN_Y * 600 / 2540;
    j->width = (int)((long)d->papers[paper].width * 600 / 2540) - j->xo;
    j->height = (int)((long)d->papers[paper].height * 600 / 2540) - j->yo;
    j->line_bytes = (j->width + 7) / 8;
    j->line = calloc(1, j->line_bytes);
    j->ref = calloc(1, j->line_bytes);
    j->enc = malloc(j->line_bytes + 32);
    j->block = malloc(MAX_BLOCK);
    if (!j->line || !j->ref || !j->enc || !j->block) {
        free(j->line);
        free(j->ref);
        free(j->enc);
        free(j->block);
        free(j);
        return NULL;
    }
    const brother_paper_t *bp = d->model;
    static const uint8_t zeros[128];
    out(j, zeros, sizeof(zeros));
    outs(j, "\033%-12345X@PJL\n");
    outs(j, "@PJL JOB NAME=\"AirPrint\"\n");
    // the page settings, the same for every page of the job
    outs(j, "\033%-12345X@PJL\n");
    outs(j, "@PJL SET RAS1200MODE = FALSE\n");
    outs(j, "@PJL SET RESOLUTION = 600\n");
    outs(j, "@PJL SET ECONOMODE = OFF\n");
    outs(j, "@PJL SET SOURCETRAY = AUTO\n");
    outs(j, "@PJL SET MEDIATYPE = PLAIN\n");
    outf(j, "@PJL SET PAPER = %s\n", bp[paper].pjl_paper);
    outs(j, "@PJL SET PAGEPROTECT = AUTO\n");
    outs(j, "@PJL SET ORIENTATION = PORTRAIT\n");
    outs(j, "@PJL ENTER LANGUAGE = PCL\n");
    outs(j, "\033E\033&l1X");
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    brother_job_t *j = job;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bytes_pp = bytes_pp;
    j->y = 0;
    j->block_len = j->block_lines = 0;
    memset(j->ref, 0, j->line_bytes);
    j->in_page = true;
    outs(j, "\033*b1030m");
    return j->err;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    brother_job_t *j = job;
    if (j->err)
        return j->err;
    // the source starts at the paper corner, the printer at its origin:
    // lines above the origin and past the paper are dropped
    for (int k = 0; k < j->scale && !j->err; k++) {
        int py = y * j->scale + k - j->yo;
        if (py < 0 || py < j->y)
            continue;
        if (py >= j->height)
            break;
        while (j->y < py && !j->err) {    // skipped source lines are white
            memset(j->line, 0, j->line_bytes);
            emit_line(j);
        }
        dither_line(j, j->y, px);
        emit_line(j);
    }
    return j->err;
}

static int page_end(void *job)
{
    brother_job_t *j = job;
    memset(j->line, 0, j->line_bytes);
    while (j->y < j->height && !j->err)
        emit_line(j);
    flush_block(j);
    outs(j, "1030M\f");
    j->in_page = false;
    return j->err;
}

static int job_end(void *job)
{
    brother_job_t *j = job;
    if (j->in_page && !j->err)
        page_end(j);        // eject a partly sent page
    outs(j, "\033%-12345X@PJL\n");
    outs(j, "@PJL EOJ NAME=\"AirPrint\"\n");
    outs(j, "\033%-12345X\n");
    int ret = j->err;
    free(j->line);
    free(j->ref);
    free(j->enc);
    free(j->block);
    free(j);
    return ret;
}

static void job_stats(void *job, char *buf, size_t n)
{
    brother_job_t *j = job;
    snprintf(buf, n, "%ld bytes to the printer", j->bytes_out);
}

// ---------------------------------------------------------------------------
// the button's test page: a frame 10 mm in, a gray ramp, a checkerboard and
// fine lines (no text: the label is not printed)

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    brother_job_t *j = job_begin(d, io, 600, 0);     // drawn at the printer's own 600 dpi
    if (!j)
        return -1;
    // the whole sheet (the driver drops what is outside the printable area)
    int res = 600, w = j->width + j->xo, h = j->height + j->yo, m = res * 100 / 254, t = res / 100 + 1;   // m: 10 mm
    uint8_t *px = malloc(w);
    if (!px) {
        job_end(j);
        return -1;
    }
    page_begin(j, w, h, 1);
    for (int y = 0; y < h && !j->err; y++) {
        memset(px, 255, w);
        bool frame_row = (y >= m && y < m + t) || (y >= h - m - t && y < h - m);
        for (int x = 0; x < w; x++) {
            bool inside = x >= m && x < w - m && y >= m && y < h - m;
            if (!inside)
                continue;
            if (frame_row || x < m + t || x >= w - m - t)
                px[x] = 0;
            int ry = y - 2 * m;
            if (ry >= 0 && ry < h / 8 && x >= 2 * m && x < w - 2 * m)     // gray ramp
                px[x] = (uint8_t)(255 * (x - 2 * m) / (w - 4 * m));
            if (ry >= h / 8 + m && ry < h / 4 + m && x >= 2 * m && x < w / 2)   // checkerboard, 5 mm squares
                px[x] = (((x - 2 * m) / (m / 2) + (ry - h / 8 - m) / (m / 2)) & 1) ? 0 : 255;
            if (ry >= h / 8 + m && ry < h / 4 + m && x >= w / 2 + m && x < w - 2 * m)   // lines 1 to 4 px wide
                px[x] = ((x - w / 2 - m) % (res / 10) < 1 + (x - w / 2 - m) / (res / 2) % 4) ? 0 : 255;
        }
        page_line(j, y, px);
        if ((y & 63) == 63 && io->progress)
            io->progress(io->ctx, y + 1, h);
    }
    free(px);
    if (!j->err)
        page_end(j);
    return job_end(j);
}

// ---------------------------------------------------------------------------

// The papers in the first tray (the first is the default: Letter, what this
// printer is loaded with); the PJL names in the same order.
static const printer_paper_t PAPERS[] = {
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Legal", "na_legal_8.5x14in", 21590, 35560},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
};
static const brother_paper_t PJL_PAPERS[] = {{"LETTER"}, {"A4"}, {"LEGAL"}, {"A5"}};

#define BROTHER_HBP(var, id_, mdl, pid)                                                                               \
    const printer_driver_t var = {                                                                                     \
        .id = id_,                                                                                                     \
        .make_model = "Brother " mdl,                                                                                  \
        .usb_mfg = "Brother",                                                                                          \
        .usb_mdl = mdl,                                                                                                \
        .usb_vid = (pid) ? 0x04f9 : 0,                                                                                 \
        .usb_pid = pid,                                                                                                \
        .color = false,                                                                                                \
        .scanner = false,                                                                                              \
        .resolutions = {300, 600},                                                                                     \
        .nresolutions = 2,                                                                                             \
        .papers = PAPERS,                                                                                              \
        .npapers = sizeof(PAPERS) / sizeof(PAPERS[0]),                                                                 \
        .margin_left = ORIGIN_X,                                                                                       \
        .margin_right = 423,                                                                                           \
        .margin_top = ORIGIN_Y,                                                                                        \
        .margin_bottom = 423,                                                                                          \
        .model = PJL_PAPERS,                                                                                           \
        .job_begin = job_begin,                                                                                        \
        .page_begin = page_begin,                                                                                      \
        .page_line = page_line,                                                                                        \
        .page_end = page_end,                                                                                          \
        .job_end = job_end,                                                                                            \
        .job_stats = job_stats,                                                                                        \
        .test_page = test_page,                                                                                        \
    }

// The HL-L2300D: 04f9:0061, IEEE 1284 MDL "HL-L2300D series" (seen on the device).
BROTHER_HBP(brother_hl_l2300d_driver, "brother-hl-l2300d", "HL-L2300D series", 0x0061);

// exposed for the host tests
int brother_hbp_encode_line(const uint8_t *line, const uint8_t *ref, int len, uint8_t *o);
int brother_hbp_encode_line(const uint8_t *line, const uint8_t *ref, int len, uint8_t *o)
{
    return encode_line(line, ref, len, o);
}
