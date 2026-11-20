// Kyocera FS-1040: a GDI printer speaking KPSL. The host renders; the printer
// takes 1-bit bands of 256 lines, JBIG-compressed (jbig/: JBIG-KIT, GPL-2.0+,
// jbig/COPYING). The stream is the one rastertokpsl-re (https://github.com/sv99/rastertokpsl-re,
// Apache-2.0, LICENSE.rastertokpsl-re; a reverse engineering of Kyocera's macOS filter) writes with its
// FS-1020MFP PPD, byte for byte (host/kpsl_host.c): the PPD's imageable area,
// Kyocera's 16x16 ordered dither and the filter's band sizes, quirks included.
// One difference: the dither runs line by line instead of on a buffered 8-bit
// band, so a page needs two 1-bit bands (300 KB at 600 dpi), not 1.5 MB.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jbig.h"
#include "printer_driver.h"

#define BAND 256        // lines per band

// the PPD's papers: size and imageable area in points (from the bottom left),
// KPSL paper code. Same order as PAPERS.
typedef struct {
    int w, h;
    int x0, y0, x1, y1;
    int code;
} kpsl_paper_t;

static const kpsl_paper_t KPSL_PAPERS[] = {
    {595, 842, 12, 10, 583, 832, 8},    // A4
    {612, 792, 12, 8, 600, 776, 6},     // Letter
};

// Kyocera's DeviceBestDither: a pixel is inked when its darkness is above
static const uint8_t DITHER[16][16] = {
    {0x91, 0xB9, 0xB1, 0x89, 0x6B, 0x43, 0x4B, 0x73, 0x93, 0xBB, 0xB3, 0x8B, 0x69, 0x41, 0x49, 0x71},
    {0xC1, 0xF1, 0xE9, 0xA1, 0x3B, 0x0B, 0x13, 0x5B, 0xC3, 0xF3, 0xEB, 0xA3, 0x39, 0x09, 0x11, 0x59},
    {0xC9, 0xF9, 0xE1, 0xA9, 0x33, 0x03, 0x1B, 0x53, 0xCB, 0xFB, 0xE3, 0xAB, 0x31, 0x01, 0x19, 0x51},
    {0x99, 0xD9, 0xD1, 0x81, 0x63, 0x23, 0x2B, 0x7B, 0x9B, 0xDB, 0xD3, 0x83, 0x61, 0x21, 0x29, 0x79},
    {0x6C, 0x44, 0x4C, 0x74, 0x94, 0xBC, 0xB4, 0x8C, 0x6E, 0x46, 0x4E, 0x76, 0x96, 0xBE, 0xB6, 0x8E},
    {0x3C, 0x0C, 0x14, 0x5C, 0xC4, 0xF4, 0xEC, 0xA4, 0x3E, 0x0E, 0x16, 0x5E, 0xC6, 0xF6, 0xEE, 0xA6},
    {0x34, 0x04, 0x1C, 0x54, 0xCC, 0xFC, 0xE4, 0xAC, 0x36, 0x06, 0x1E, 0x56, 0xCE, 0xFE, 0xE6, 0xAE},
    {0x64, 0x24, 0x2C, 0x7C, 0x9C, 0xDC, 0xD4, 0x84, 0x66, 0x26, 0x2E, 0x7E, 0x9E, 0xDE, 0xD6, 0x86},
    {0x92, 0xBA, 0xB2, 0x8A, 0x68, 0x40, 0x48, 0x70, 0x90, 0xB8, 0xB0, 0x88, 0x6A, 0x42, 0x4A, 0x72},
    {0xC2, 0xF2, 0xEA, 0xA2, 0x38, 0x08, 0x10, 0x58, 0xC0, 0xF0, 0xE8, 0xA0, 0x3A, 0x0A, 0x12, 0x5A},
    {0xCA, 0xFA, 0xE2, 0xAA, 0x30, 0x00, 0x18, 0x50, 0xC8, 0xF8, 0xE0, 0xA8, 0x32, 0x02, 0x1A, 0x52},
    {0x9A, 0xDA, 0xD2, 0x82, 0x60, 0x20, 0x28, 0x78, 0x98, 0xD8, 0xD0, 0x80, 0x62, 0x22, 0x2A, 0x7A},
    {0x6F, 0x47, 0x4F, 0x77, 0x97, 0xBF, 0xB7, 0x8F, 0x6D, 0x45, 0x4D, 0x75, 0x95, 0xBD, 0xB5, 0x8D},
    {0x3F, 0x0F, 0x17, 0x5F, 0xC7, 0xF7, 0xEF, 0xA7, 0x3D, 0x0D, 0x15, 0x5D, 0xC5, 0xF5, 0xED, 0xA5},
    {0x37, 0x07, 0x1F, 0x57, 0xCF, 0xFE, 0xE7, 0xAF, 0x35, 0x05, 0x1D, 0x55, 0xCD, 0xFD, 0xE5, 0xAD},
    {0x67, 0x27, 0x2F, 0x7F, 0x9F, 0xDF, 0xD7, 0x87, 0x65, 0x25, 0x2D, 0x7D, 0x9D, 0xDD, 0xD5, 0x85},
};

typedef struct {
    printer_io_t io;
    int err;
    int dpi;
    const kpsl_paper_t *paper;
    bool in_page;
    bool page_ended;            // its end block waits: it says whether it was the last page
    // the page: the imageable area cropped from the source raster
    int left, top, width, height;
    int src_w, bytes_pp;
    unsigned wib;               // bytes per band line, a multiple of 32
    unsigned y;                 // next line of the imageable area
    unsigned num_ver;           // lines of the band being filled (rastertokpsl's numVer)
    bool vert_flag;             // false once the last band has started
    uint8_t *band;              // BAND lines, 1 bit per pixel
    uint8_t *out;               // the compressed band
    size_t out_len, out_cap;
    bool skip_bih;
} kpsl_job_t;

static void emit(kpsl_job_t *j, const void *data, size_t len)
{
    if (!j->err && len && j->io.write(j->io.ctx, data, len) < 0)
        j->err = -1;
}

static uint8_t *put16(uint8_t *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
    return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p = put16(p, v);
    return put16(p, v >> 16);
}

// a block header: ESC $ 0 <op>, its length in 4-byte words, "@@@@"
static uint8_t *block(uint8_t *p, char op, uint32_t words)
{
    memcpy(p, "\x1b$0", 3);
    p[3] = op;
    p = put32(p + 4, words);
    memcpy(p, "@@@@", 4);
    return p + 4;
}

// ASCII into n UTF-16LE units, zero padded
static uint8_t *put_utf16(uint8_t *p, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        p = put16(p, *s ? (uint8_t)*s++ : 0);
    return p;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    kpsl_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    j->dpi = dpi;
    j->paper = &KPSL_PAPERS[paper];

    uint8_t h[256], *p = h;
    memcpy(p, "LSPK", 4);
    p = block(p + 4, 'J', 13);
    memcpy(p, "0100", 4);
    p = put_utf16(p + 4, "airprint", 16);       // user
    time_t now = time(NULL);
    char ts[15];
    strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", localtime(&now));
    memcpy(p, ts, 14);
    p = put16(p + 14, 0);
    p = block(p, 'D', 16);
    p = put_utf16(p, "", 32);                   // title
    p = block(p, 'C', 1);
    p = put16(put16(p, 1), 0);                  // copies, collate
    p = block(p, 'S', 2);
    p = put16(put16(put16(put16(p, 0), 0), 0), 0);  // tray, duplex, feeding, engine speed
    p = block(p, 'G', 3);
    p = put16(put16(p, dpi), dpi);
    p = put16(put16(put16(put16(p, 1), 1), 32), 1 << 8);
    emit(j, h, p - h);
    return j;
}

// the end block of the page before: section end on the last page
static void end_block(kpsl_job_t *j, bool last)
{
    uint8_t h[16];
    emit(j, h, put32(block(h, 'F', 1), last) - h);
    j->page_ended = false;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    kpsl_job_t *j = job;
    const kpsl_paper_t *pp = j->paper;
    // macOS rasterises the imageable area, its size truncated to pixels
    j->left = (pp->x0 * j->dpi + 36) / 72;
    j->top = ((pp->h - pp->y1) * j->dpi + 36) / 72;
    j->width = (pp->x1 - pp->x0) * j->dpi / 72;
    j->height = (pp->y1 - pp->y0) * j->dpi / 72;
    j->wib = (j->width + 255) / 256 * 32;
    j->src_w = src_w;
    j->bytes_pp = bytes_pp;
    j->y = 0;
    j->num_ver = BAND;
    j->vert_flag = true;
    j->out_cap = (size_t)j->wib * BAND;
    j->band = calloc(1, j->out_cap);
    j->out = malloc(j->out_cap);
    if (!j->band || !j->out) {
        free(j->band);
        free(j->out);
        j->band = j->out = NULL;
        return -1;      // page_ended stays set: job_end ends the section
    }
    if (j->page_ended)
        end_block(j, false);
    j->in_page = true;

    uint8_t h[32], *p = block(h, 'P', 3);
    p = put16(put16(p, 0), 0);          // orientation
    p = put16(p, (unsigned short)(10.0 * (pp->w * 0.352777778)));   // 1/10 mm
    p = put16(p, (unsigned short)(10.0 * (pp->h * 0.352777778)));
    p = put16(put16(p, pp->code), 0);   // media type: auto
    emit(j, h, p - h);
    return j->err;
}

// JBIG-KIT's output: the 20-byte header first, left out (the band block
// carries the sizes)
static void bie_out(unsigned char *start, size_t len, void *user)
{
    kpsl_job_t *j = user;
    if (j->skip_bih && len == 20) {
        j->skip_bih = false;
        return;
    }
    if (j->out_len + len <= j->out_cap)
        memcpy(j->out + j->out_len, start, len);
    j->out_len += len;
}

// Sends the band that ends at line y: JBIG, or raw if that is not smaller.
// Like rastertokpsl, a band has num_ver lines (white past the page) and says
// it starts at y - 255, also when it is shorter.
static void send_band(kpsl_job_t *j)
{
    unsigned rows = j->num_ver;
    size_t plane = (size_t)rows * j->wib;
    struct jbg_enc_state s;
    unsigned char *planes[1] = {j->band};
    j->out_len = 0;
    j->skip_bih = true;
    jbg_enc_init(&s, 8 * j->wib, rows, 1, planes, bie_out, j);
    jbg_enc_layers(&s, 0);
    jbg_enc_options(&s, 0, 0, BAND, 0, 0);
    jbg_enc_out(&s);
    jbg_enc_free(&s);

    size_t padded = (j->out_len + 31) & ~(size_t)31;
    uint8_t h[64], *p;
    if (padded <= plane) {
        p = block(h, 'B', padded / 4 + 13);
        p = put32(put32(put32(p, 1 << 16), j->width), j->wib);
        p = put32(put32(put32(put32(p, rows), BAND), 1 << 8), 0);
        p = put32(put32(put32(p, j->out_len), padded), 0);
        p = put32(put32(put32(p, j->y - 255), 0), 1);
        emit(j, h, p - h);
        memset(j->out + j->out_len, 0, padded - j->out_len);
        emit(j, j->out, padded);
    } else {
        p = block(h, 'R', plane / 4 + 10);
        p = put32(put32(put32(put32(p, j->width), j->wib), rows), BAND);
        p = put32(put32(put32(p, plane), plane), 0);
        p = put32(put32(put32(p, j->y - 255), 0), 1);
        emit(j, h, p - h);
        emit(j, j->band, plane);
    }
    memset(j->band, 0, (size_t)j->wib * BAND);
    if (!j->vert_flag)
        j->num_ver = j->height % BAND;
}

// the next line of the imageable area from a source line (NULL: white)
static void add_line(kpsl_job_t *j, const uint8_t *px)
{
    int n = j->src_w - j->left < j->width ? j->src_w - j->left : j->width;
    if (px && n > 0) {
        uint8_t *row = j->band + (size_t)(j->y % BAND) * j->wib;
        const uint8_t *th = DITHER[j->y % 16];
        const uint8_t *s = px + (size_t)j->left * j->bytes_pp;
        for (int x = 0; x < n; x++, s += j->bytes_pp) {
            int g = j->bytes_pp == 3 ? (s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8 : s[0];
            if (255 - g > th[x & 15])
                row[x >> 3] |= 0x80 >> (x & 7);
        }
    }
    unsigned y = j->y;
    if (j->vert_flag && j->height - y <= 255)
        j->vert_flag = false;
    if ((y && y % BAND == 255) || y == (unsigned)j->height - 1)
        send_band(j);
    j->y++;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    kpsl_job_t *j = job;
    int iy = y - j->top;
    if (iy < 0 || iy >= j->height)
        return j->err;
    while (j->y < (unsigned)iy && !j->err)
        add_line(j, NULL);
    add_line(j, px);
    return j->err;
}

static int page_end(void *job)
{
    kpsl_job_t *j = job;
    while (j->y < (unsigned)j->height && !j->err)    // a short source: white
        add_line(j, NULL);
    free(j->band);
    free(j->out);
    j->band = j->out = NULL;
    j->in_page = false;
    j->page_ended = true;
    return j->err;
}

static int job_end(void *job)
{
    kpsl_job_t *j = job;
    if (j->in_page)     // canceled: the rest of the page is white
        page_end(j);
    if (j->page_ended)
        end_block(j, true);
    uint8_t h[32], *p = block(h, 'E', 0);
    p = block(p, 'T', 0);
    emit(j, h, p - h);
    int ret = j->err;
    free(j);
    return ret;
}

static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

// Matched by USB id, or by name (CID:KY_KPSL_MonoPersonal on a real one)
const printer_driver_t kyocera_fs1040_driver = {
    .id = "kyocera-fs1040",
    .make_model = "Kyocera FS-1040",
    .usb_mfg = "Kyocera",
    .usb_mdl = "FS-1040",
    .usb_vid = 0x0482,
    .usb_pid = 0x0493,
    .color = false,
    .scanner = false,
    .resolutions = {600},
    .nresolutions = 1,
    .papers = PAPERS,
    .npapers = 2,
    // the PPD's imageable area, the wider border of A4 and Letter: 12 pt left
    // and right, 16 pt top, 10 pt bottom
    .margin_left = 424,
    .margin_right = 424,
    .margin_top = 565,
    .margin_bottom = 353,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
};
