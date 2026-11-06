// Grozziie TP732H 80 mm thermal label printer (USB 4c4a:c200, "USB Printer WT",
// no IEEE 1284 Device ID).
// Its Windows driver (SangYi/ANKUI "tp518w201.inf", Unidrv with the
// OEM640dots render plugin) sends a vendor command set, not ESC/POS or TSPL.
// This driver reproduces that stream byte for byte (host/bjl_host selftest
// against the captures in tools/testdata/tp732h/).
//
// A page, on its own (every page repeats it all):
//   50 zero bytes
//   "@SMS" 02 d5 "\r\n" "@S~~E" "@SGS" 02 "\r\n" "@STPZ\n" <14 bytes> "\r\n" "@SPP" 00
//                     the settings as the driver sent them with its defaults
//                     (the 14 bytes are likely the density per speed mode)
//   "@<" u32be N u16le rows        N = bytes from here to the end, minus 92
//   per row: "#<" runs "#>"        640 dots at 203 dpi
//   "@>\r\n"
// Runs: 0x80|level, n = n + 1 dots of a level (up to 256); a single dot is
// the level byte alone. Levels: 0 white, 1 black, 2 black with white
// directly above it (the driver marks the top edge of every shape, likely
// for more heat on a cold dot). The driver sends the page down to its last
// black row plus 24 rows, not the full label.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

#define TP_DOTS 640         // dots per row (80 mm at 203 dpi)
#define TP_TAIL_ROWS 24     // white rows kept after the last black one
#define TP_LEN_OFFSET 92    // N in the "@<" header is the remaining length minus this

static const uint8_t PAGE_HEADER[] = "@SMS\x02\xd5\r\n@S~~E@SGS\x02\r\n@STPZ\n"
                                     "\x14\x14\x14\x14\x14\x14\x5a\x64\x50\x50\x50\x50\x50\x50\r\n@SPP";

// 8x8 ordered dither, thresholds 2..254: gray 0 is always black, 255 never,
// 50 % a checkerboard (as the Windows driver)
static const uint8_t BAYER8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
};

#define TP_ROW_BYTES (TP_DOTS / 8)
#define TP_ROW_MAX (2 + TP_DOTS + 2)    // "#<", at most a byte per dot, "#>"

typedef struct {
    printer_io_t io;
    int err;
    // the page, 1 bit per dot (1 = black): encoded and sent at page_end, once
    // the count and length of the rows to send are known (83 KB for a label,
    // where the encoded rows of a dense one take 670 KB)
    int src_w, bytes_pp;
    uint8_t *bits;
    int cap_rows, rows, last_black;
} tp_job_t;

static uint8_t level_at(const uint8_t *row, const uint8_t *above, int x)
{
    uint8_t mask = 0x80 >> (x & 7);
    if (!(row[x >> 3] & mask))
        return 0;
    return above && (above[x >> 3] & mask) ? 1 : 2;
}

// One row as the printer takes it, into out (TP_ROW_MAX bytes); returns its length
static size_t encode_row(const uint8_t *row, const uint8_t *above, uint8_t *out)
{
    size_t n = 0;
    out[n++] = '#';
    out[n++] = '<';
    for (int x = 0; x < TP_DOTS;) {
        uint8_t level = level_at(row, above, x);
        int run = 1;
        while (x + run < TP_DOTS && level_at(row, above, x + run) == level)
            run++;
        x += run;
        for (; run > 0; run -= 256) {
            int c = run > 256 ? 256 : run;
            if (c == 1) {
                out[n++] = level;
            } else {
                out[n++] = 0x80 | level;
                out[n++] = (uint8_t)(c - 1);
            }
        }
    }
    out[n++] = '#';
    out[n++] = '>';
    return n;
}

static int send(tp_job_t *j, const void *data, size_t n)
{
    if (!j->err && j->io.write(j->io.ctx, data, n) < 0)
        j->err = -1;
    return j->err;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    tp_job_t *j = calloc(1, sizeof(*j));
    if (j)
        j->io = *io;
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    tp_job_t *j = job;
    if (src_h > 65535)      // the row count is 16 bits
        j->err = -1;
    if (!j->err && src_h > j->cap_rows) {
        free(j->bits);
        j->bits = malloc((size_t)src_h * TP_ROW_BYTES);
        j->cap_rows = j->bits ? src_h : 0;
        if (!j->bits)
            j->err = -1;
    }
    j->src_w = src_w;
    j->bytes_pp = bytes_pp;
    j->rows = 0;
    j->last_black = -1;
    return j->err;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    tp_job_t *j = job;
    if (j->err || j->rows >= j->cap_rows)
        return j->err;
    uint8_t *row = j->bits + (size_t)j->rows * TP_ROW_BYTES;
    bool black = false;
    int w = j->src_w < TP_DOTS ? j->src_w : TP_DOTS;
    memset(row, 0, TP_ROW_BYTES);
    for (int x = 0; px && x < w; x++) {
        const uint8_t *p = px + x * j->bytes_pp;
        int gray = j->bytes_pp == 3 ? (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8 : p[0];
        if (gray < BAYER8[y & 7][x & 7] * 4 + 2) {
            row[x >> 3] |= 0x80 >> (x & 7);
            black = true;
        }
    }
    if (black)
        j->last_black = j->rows;
    j->rows++;
    return 0;
}

static int page_end(void *job)
{
    tp_job_t *j = job;
    static const uint8_t zeros[50];
    if (j->err)
        return j->err;
    int keep = j->last_black + 1 + TP_TAIL_ROWS;
    if (keep > j->rows)
        keep = j->rows;
    uint8_t out[TP_ROW_MAX];
    const uint8_t *row = j->bits;
    long n = 4 - TP_LEN_OFFSET;     // the rows and "@>\r\n"
    for (int y = 0; y < keep; y++, row += TP_ROW_BYTES)
        n += (long)encode_row(row, y ? row - TP_ROW_BYTES : NULL, out);
    if (n < 0)
        n = 0;
    uint8_t start[8] = {'@', '<', n >> 24, n >> 16, n >> 8, n, keep & 0xff, keep >> 8};
    send(j, zeros, sizeof(zeros));
    send(j, PAGE_HEADER, sizeof(PAGE_HEADER));     // with its terminating 00
    send(j, start, sizeof(start));
    row = j->bits;
    for (int y = 0; y < keep && !j->err; y++, row += TP_ROW_BYTES)
        send(j, out, encode_row(row, y ? row - TP_ROW_BYTES : NULL, out));
    send(j, "@>\r\n", 4);
    // the printer prints while it receives and garbles a page whose data
    // pauses: send all of it now, not when the next page fills the buffer
    if (!j->err && j->io.sync && j->io.sync(j->io.ctx, NULL) < 0)
        j->err = -1;
    return j->err;
}

// Pages are sent whole at page_end: a cancelled page prints nothing.
static int job_end(void *job)
{
    tp_job_t *j = job;
    int err = j->err;
    free(j->bits);
    free(j);
    return err;
}

static const printer_paper_t PAPERS[] = {
    {"80x130mm", "om_label_80x130mm", 8000, 13000},
};

const printer_driver_t tp732h_driver = {
    .id = "tp732h",
    .make_model = "Grozziie TP732H label printer",
    .usb_mfg = "Grozziie",
    .usb_mdl = "TP732H",
    .usb_vid = 0x4c4a,
    .usb_pid = 0xc200,
    .color = false,
    .scanner = false,
    .resolutions = {203},
    .nresolutions = 1,
    .papers = PAPERS,
    .npapers = 1,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
};
