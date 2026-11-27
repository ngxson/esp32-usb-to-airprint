// Canon i-SENSYS / imageCLASS MF220 series (MF221d, ...): monochrome laser,
// UFR II LT. Written from the streams that Canon's macOS driver (CUPS_MF_Printer,
// CNPZBMF220ZB.ppd, "Standard" 600 dpi quality) sends to an MF221d; see
// AGENTS.md "Printer protocol (MF220 series)".
//
// The stream: CPCA packets ("CD CA 10 00", 20-byte header); the job's
// settings in packets of their own, then the UFR II page description in
// packets of type 0x1a. A page is a JBIG image (ITU-T T.82: one layer,
// two-line template, typical prediction, 128-line stripes) of 1200 x 600 dpi
// dots, sent in 256-line bands. The JBIG encoder here is written from T.82;
// its probability table is T.82 Table 24 (= T.81 Table D.2, as in IJG's
// jaricom.c). Portable C.
#include "canon_ufr2.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

#define STRIPE 128              // JBIG lines per stripe (L0)
#define UFR_CHUNK 4075          // UFR bytes per CPCA packet (+ 1 flag byte), as Canon's driver
#define TPB2CX 0x195            // T.82 context of the typical prediction bit, two-line template

typedef struct {
    const char *name;
    uint8_t ufr, cpca;          // paper codes in the UFR job header and the CPCA settings
    uint16_t width, height;     // page at 600 dpi as the UFR page header gives it
} ufr_paper_t;

// index = printer_paper_t index
static const ufr_paper_t UFR_PAPERS[] = {
    {"A4", 0x00, 0x02, 4958, 7016},
    {"Letter", 0x64, 0x0d, 5100, 6600},
};

// QM coder probability estimation: Qe << 16 | next LPS << 8 | switch << 7 | next MPS
static const uint32_t QE[113] = {
    0x5a1d0181, 0x25860e02, 0x11141003, 0x080b1204, 0x03d81405, 0x01da1706,
    0x00e51907, 0x006f1c08, 0x00361e09, 0x001a210a, 0x000d230b, 0x0006090c,
    0x00030a0d, 0x00010c0d, 0x5a7f0f8f, 0x3f252410, 0x2cf22611, 0x207c2712,
    0x17b92813, 0x11822a14, 0x0cef2b15, 0x09a12d16, 0x072f2e17, 0x055c3018,
    0x04063119, 0x0303331a, 0x0240341b, 0x01b1361c, 0x0144381d, 0x00f5391e,
    0x00b73b1f, 0x008a3c20, 0x00683e21, 0x004e3f22, 0x003b2023, 0x002c2109,
    0x5ae125a5, 0x484c4026, 0x3a0d4127, 0x2ef14328, 0x261f4429, 0x1f33452a,
    0x19a8462b, 0x1518482c, 0x1177492d, 0x0e744a2e, 0x0bfb4b2f, 0x09f84d30,
    0x08614e31, 0x07064f32, 0x05cd3033, 0x04de3234, 0x040f3235, 0x03633336,
    0x02d43437, 0x025c3538, 0x01f83639, 0x01a4373a, 0x0160383b, 0x0125393c,
    0x00f63a3d, 0x00cb3b3e, 0x00ab3d3f, 0x008f3d20, 0x5b1241c1, 0x4d045042,
    0x412c5143, 0x37d85244, 0x2fe85345, 0x293c5446, 0x23795647, 0x1edf5748,
    0x1aa95749, 0x174e484a, 0x1424484b, 0x119c4a4c, 0x0f6b4a4d, 0x0d514b4e,
    0x0bb64d4f, 0x0a404d30, 0x583250d1, 0x4d1c5852, 0x438e5953, 0x3bdd5a54,
    0x34ee5b55, 0x2eae5c56, 0x299a5d57, 0x25165647, 0x557058d9, 0x4ca95f5a,
    0x44d9605b, 0x3e22615c, 0x3824635d, 0x32b4635e, 0x2e175d56, 0x56a85fe0,
    0x4f466561, 0x47e56662, 0x41cf6763, 0x3c3d6864, 0x375e635d, 0x52316966,
    0x4c0f6a67, 0x46396b68, 0x415e6763, 0x562769ea, 0x50e76c6b, 0x4b856d67,
    0x55976e6d, 0x504f6f6b, 0x5a106eef, 0x5522706d, 0x59eb70ef,
};

// Clustered-dot screen: one 4 x 4 cell at 600 dpi is 8 x 4 dots; a dot turns
// black in this order as the gray darkens (centre first).
static const uint8_t SCREEN[4][8] = {
    {28, 24, 16, 12, 13, 17, 25, 29},
    {20, 8, 4, 0, 1, 5, 9, 21},
    {23, 11, 7, 3, 2, 6, 10, 22},
    {31, 27, 19, 15, 14, 18, 26, 30},
};

typedef struct {
    printer_io_t io;
    int err;
    uint32_t bytes;             // whole stream so far (the trailer gives the total)
    int pages;
    bool duplex;                // both sides (the client turns long-edge backs)
    bool short_edge;            // bound on the short edge
    const ufr_paper_t *paper;
    int band_w;                 // page width at 600 dpi, a multiple of 32
    int lb;                     // bytes per raster line (2 * band_w dots)
    uint8_t pkt[1 + UFR_CHUNK]; // UFR bytes waiting for their CPCA packet
    int pkt_len;
    uint8_t gray_thr[4][8];     // black when gray < this

    // page
    bool in_page;
    int scale;                  // raster lines per source line: 600 / the job's dpi
    int src_w, src_h, bpp;
    int y;                      // raster lines added
    uint8_t *lines;             // the stripe being collected, STRIPE lines
    uint8_t *above, *above2;    // the two lines above it
    uint8_t *white;
    bool reset;                 // first stripe, or after SDRST: nothing above it is used
    int n;                      // lines in it

    // band being built: [BIH] stripe [stripe]
    uint8_t *band;
    size_t band_len, band_cap;
    int band_y, band_lines;

    // QM arithmetic encoder (T.82 6.8)
    uint32_t c, a, sc;
    int ct, buffer;
    uint8_t st[1024];
} job_t;

// ---------------------------------------------------------------------------
// output: CPCA packets

static void put(job_t *j, const void *data, size_t len)
{
    if (!j->err && j->io.write(j->io.ctx, data, len) < 0)
        j->err = -1;
    j->bytes += len;
}

static void cpca(job_t *j, int op, int kind, const uint8_t *p, size_t len)
{
    uint8_t h[20] = {0xcd, 0xca, 0x10, kind, op >> 8, op & 0xff, 0x00, 0x01, len >> 8, len & 0xff, 0xff, 0xff};
    put(j, h, sizeof(h));
    put(j, p, len);
}

#define CPCA(j, op, ...) cpca(j, op, 0, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

// The UFR data in packets, cut as Canon's driver cuts it; the printer needs
// it (with a page end and the next page in one packet, a duplex job printed
// its first page only). Canon writes each object (the job and page header, an
// image band, a page end, the job end) as one write, at most 65515 bytes
// (a band's header alone, then its data in such pieces, each an object of its
// own, when longer). A write
// that does not fit the pending packet sends what is pending, an empty
// packet, then full packets; what is left waits; the object's end sends it.
#define UFR_WRITE_MAX 65515

static void ufr_flush(job_t *j)
{
    j->pkt[0] = 0x01;
    cpca(j, 0x1a, 2, j->pkt, 1 + j->pkt_len);
    j->pkt_len = 0;
}

// ends an object: what is pending goes out as one packet
static void ufr_end(job_t *j)
{
    if (j->pkt_len)
        ufr_flush(j);
}

// one write of two pieces (a band's header and data), total at most UFR_WRITE_MAX
static void ufr_write2(job_t *j, const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    if (j->pkt_len + na + nb > UFR_CHUNK) {
        ufr_end(j);
        ufr_flush(j);       // the empty packet
    }
    for (int k = 0; k < 2; k++) {
        const uint8_t *p = k ? b : a;
        size_t len = k ? nb : na;
        while (len) {
            size_t n = UFR_CHUNK - j->pkt_len;
            if (n > len)
                n = len;
            memcpy(j->pkt + 1 + j->pkt_len, p, n);
            j->pkt_len += n;
            p += n;
            len -= n;
            if (j->pkt_len == UFR_CHUNK)
                ufr_flush(j);
        }
    }
}

static void ufr(job_t *j, const uint8_t *p, size_t len)
{
    ufr_write2(j, p, len, NULL, 0);
}

#define UFR(j, ...) ufr(j, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

// a string item of the job ticket (packet 0x6b) or of the settings
static size_t str_item(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = 0x00;
    p[1] = 0x04;
    p[2] = n;
    memcpy(p + 3, s, n);
    return 3 + n;
}

static size_t item(uint8_t *p, int id, const uint8_t *data, size_t len)
{
    p[0] = id >> 8;
    p[1] = id & 0xff;
    p[2] = len >> 8;
    p[3] = len & 0xff;
    memcpy(p + 4, data, len);
    return 4 + len;
}

#define DOC_NAME "AirPrint"
#define USER_NAME "airprint"
#define HOST_NAME "esp32"

// The job ticket and settings Canon's driver sends before the UFR data:
// plain paper, 1 copy, 600 dpi, tray auto, simplex or duplex. Canon's driver
// sends long and short edge duplex alike but for the binding setting (07e0);
// its back pages differ: turned by 180 degrees for the long edge.
static void job_header(job_t *j)
{
    uint8_t p[256], s[64];
    size_t n = 0;
    static const uint8_t pre[] = {0x00, 0x09, 0x00, 0xf0, 0x00, 0x01, 0x01, 0x01, 0x30, 0x00, 0x03, 0x03, 0x00, 0x00};
    memcpy(p, pre, sizeof(pre));
    n = sizeof(pre);
    size_t sl = str_item(s, DOC_NAME);
    n += item(p + n, 0x0004, s, sl);
    n += item(p + n, 0x0117, s, sl);
    n += item(p + n, 0x0006, s, str_item(s, USER_NAME));
    n += item(p + n, 0x000d, (const uint8_t[]){0x04}, 1);
    uint8_t ids[64] = "ESP32-USB-UFR2\x00\x01";   // job owner id (16), 0 (16), host (16), host (16)
    memset(ids + 16, 0, 48);
    memcpy(ids + 32, HOST_NAME, sizeof(HOST_NAME) - 1);
    memcpy(ids + 48, HOST_NAME, sizeof(HOST_NAME) - 1);
    n += item(p + n, 0x0118, ids, sizeof(ids));
    n += item(p + n, 0x000c, (const uint8_t[]){0x32}, 1);
    n += item(p + n, 0x0009, (const uint8_t[]){0x7e, 0xab, 0xdd, 0x00, 0x08, 0x39, 0x5b, 0x6a}, 8);
    cpca(j, 0x6b, 0, p, n);

    CPCA(j, 0x12, 0x07, 0xd7, 0x00, 0x01);
    CPCA(j, 0x14, 0x00, 0x00, 0x00, 0x00);
    CPCA(j, 0x15, 0x08, 0xb3, 0x02);
    s[0] = 0x08;
    s[1] = 0x2b;
    cpca(j, 0x15, 0, s, 2 + str_item(s + 2, DOC_NAME));
    CPCA(j, 0x15, 0x07, 0xd7, 0x00, 0x01);
    CPCA(j, 0x15, 0x07, 0xd8, 0x0f);
    CPCA(j, 0x15, 0x07, 0xd9, 0x17, 0x00, 0x00, 0x00, 0x00);
    CPCA(j, 0x15, 0x08, 0x4a, 0x04);
    CPCA(j, 0x15, 0x07, 0xda, 0x00);
    CPCA(j, 0x15, 0x08, 0xa5, 0xfe, 0xfe, 0xff, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0x03);
    CPCA(j, 0x17, 0x00, 0x00, 0x00, 0x00);
    CPCA(j, 0x18, 0x00, 0x2e, 0x83, 0x00, 0x00);
    CPCA(j, 0x18, 0x08, 0xb5, 0x02);
    CPCA(j, 0x18, 0x08, 0x6e, 0x01, 0x0b, 0x00, 0x00);
    CPCA(j, 0x18, 0x07, 0xed, 0x03, 0xfe, 0x00, j->paper->cpca, 0, 0, 0, 0, 0, 0, 0, 0, 0x03);
    CPCA(j, 0x18, 0x07, 0xd7, 0x00, 0x01);              // copies
    CPCA(j, 0x18, 0x07, 0xdf, j->duplex ? 0x04 : 0x01);     // duplex : simplex
    CPCA(j, 0x18, 0x07, 0xe0, j->short_edge ? 0x03 : 0x04);    // binding: short : long edge
    CPCA(j, 0x18, 0x07, 0xe1, 0x00, 0x00, 0x00, 0x00);
    CPCA(j, 0x18, 0x00, 0x3a, 0x08, 0x02, 0x58, 0x02, 0x58);   // 600 x 600 dpi
    CPCA(j, 0x18, 0x09, 0x72, 0x00);
    CPCA(j, 0x18, 0x09, 0x71, 0x01);

    // UFR: the job (600 dpi, the paper, duplex)
    UFR(j, 0x01, 0xc1, 0x85, 0x10, 0x00, 0x10, 0x95, 0xc2, 0x00, 0xd8, 0x84, 0x02, 0x58, 0xdd, 0x80, 0xc8, 0xf0, 0x84,
        0x08, 0x00, 0x02, 0xc3, j->paper->ufr, 0xc5, j->duplex, 0xc6, 0x00, 0x51, 0xf2, 0x00);
}

static void job_trailer(job_t *j)
{
    UFR(j, 0x12, 0x11);
    ufr_end(j);
    cpca(j, 0x1a, 0, (const uint8_t[]){0x01}, 1);   // end of the UFR data
    uint8_t pg[] = {0x01, 0x13, j->pages >> 24, j->pages >> 16, j->pages >> 8, j->pages};
    cpca(j, 0x19, 0, NULL, 0);
    cpca(j, 0x15, 0, pg, sizeof(pg));
    cpca(j, 0x16, 0, NULL, 0);
    cpca(j, 0x12, 0, pg, sizeof(pg));
    uint32_t total = j->bytes + 26 + 21;        // with this packet and the last one
    CPCA(j, 0x12, 0x07, 0xd6, total >> 24, total >> 16, total >> 8, total);
    CPCA(j, 0x13, 0x00);
}

// ---------------------------------------------------------------------------
// JBIG encoder (T.82): lowest resolution layer, two-line template, TPBON,
// no adaptive template moves. Output goes to the band buffer.

static void band_byte(job_t *j, uint8_t b)
{
    if (j->band_len == j->band_cap) {
        size_t cap = j->band_cap ? 2 * j->band_cap : 65536;
        uint8_t *p = realloc(j->band, cap);
        if (!p) {
            j->err = -1;
            return;
        }
        j->band = p;
        j->band_cap = cap;
    }
    j->band[j->band_len++] = b;
}

// a PSCD byte: 0xff is stuffed with 0x00
static void scd_byte(job_t *j, uint8_t b)
{
    band_byte(j, b);
    if (b == 0xff)
        band_byte(j, 0x00);
}

static void qm_byteout(job_t *j)
{
    uint32_t t = j->c >> 19;
    if (t > 0xff) {                     // carry into the buffered byte
        if (j->buffer >= 0)
            scd_byte(j, j->buffer + 1);
        for (; j->sc; j->sc--)
            band_byte(j, 0x00);         // the stacked 0xff bytes became 0x00
        j->buffer = t & 0xff;
    } else if (t == 0xff) {
        j->sc++;                        // could still carry: hold it
    } else {
        if (j->buffer >= 0)
            scd_byte(j, j->buffer);
        for (; j->sc; j->sc--)
            scd_byte(j, 0xff);
        j->buffer = t;
    }
    j->c &= 0x7ffff;
    j->ct = 8;
}

static inline void qm_code(job_t *j, int cx, int pix)
{
    uint8_t *s = &j->st[cx];
    uint32_t q = QE[*s & 0x7f], lsz = q >> 16;
    j->a -= lsz;
    if (pix == (*s >> 7)) {
        if (j->a >= 0x8000)
            return;
        if (j->a < lsz) {               // conditional exchange: the MPS gets the top
            j->c += j->a;
            j->a = lsz;
        }
        *s = (*s & 0x80) | (q & 0x7f);
    } else {
        if (j->a >= lsz) {
            j->c += j->a;
            j->a = lsz;
        }
        *s = ((*s ^ q) & 0x80) | ((q >> 8) & 0x7f);
    }
    do {
        j->a <<= 1;
        j->c <<= 1;
        if (--j->ct == 0)
            qm_byteout(j);
    } while (j->a < 0x8000);
}

// T.82 FLUSH: the shortest code that stays in the interval
static void qm_flush(job_t *j)
{
    uint32_t t = (j->a - 1 + j->c) & 0xffff0000;
    j->c = t < j->c ? t + 0x8000 : t;
    j->c <<= j->ct;
    if (j->c & 0xf8000000) {
        if (j->buffer >= 0)
            scd_byte(j, j->buffer + 1);
        if (j->c & 0x7fff800)
            for (; j->sc; j->sc--)
                band_byte(j, 0x00);
    } else {
        if (j->buffer >= 0)
            scd_byte(j, j->buffer);
        for (; j->sc; j->sc--)
            scd_byte(j, 0xff);
    }
    if (j->c & 0x7fff800) {
        scd_byte(j, (j->c >> 19) & 0xff);
        if (j->c & 0x7f800)
            scd_byte(j, (j->c >> 11) & 0xff);
    }
}

// n white dots in context 0 (white on white): the same as n qm_code(j, 0, 0),
// without going through the codings that change nothing but A
static void code_white_run(job_t *j, uint32_t n)
{
    uint8_t *s = &j->st[0];
    while (n) {
        if (*s >> 7) {                  // white is the less probable symbol here
            qm_code(j, 0, 0);
            n--;
            continue;
        }
        uint32_t lsz = QE[*s & 0x7f] >> 16;
        uint32_t m = (j->a - 0x8000) / lsz;     // codings that leave A >= 0x8000
        if (m >= n) {
            j->a -= n * lsz;
            return;
        }
        j->a -= m * lsz;
        n -= m;
        qm_code(j, 0, 0);               // the one that renormalises
        n--;
    }
}

static void code_line(job_t *j, const uint8_t *cur, const uint8_t *up)
{
    uint32_t c4 = 0;                    // current line: x-4 .. x-1
    int lb = j->lb;
    for (int i = 0; i < lb; i++) {
        uint32_t w = (i ? up[i - 1] << 16 : 0) | up[i] << 8 | (i + 1 < lb ? up[i + 1] : 0);
        uint32_t b = cur[i];
        if (!(w | b | c4)) {            // white on white: context 0 while it lasts
            int r = 1;
            while (i + r < lb && !cur[i + r] && (i + r + 1 >= lb || !up[i + r + 1]))
                r++;
            code_white_run(j, 8 * (uint32_t)r);
            i += r - 1;
            continue;
        }
        for (int k = 0; k < 8; k++) {
            int p = (b >> (7 - k)) & 1;
            // line above: x-3 .. x+2, then the current line
            qm_code(j, ((w >> (13 - k)) & 0x3f) << 4 | c4, p);
            c4 = ((c4 << 1) | p) & 0xf;
        }
    }
}

// one stripe (SDE) from j->lines, appended to the band. A stripe after
// SDRST sees white above it and starts its typical prediction afresh.
static void encode_stripe(job_t *j)
{
    j->c = 0;
    j->a = 0x10000;
    j->ct = 11;
    j->sc = 0;
    j->buffer = -1;
    size_t start = j->band_len;
    const uint8_t *up = j->reset ? j->white : j->above;
    int ltp_old = j->reset ? 0 : !memcmp(j->above, j->above2, j->lb);
    for (int y = 0; y < j->n; y++) {
        const uint8_t *cur = j->lines + (size_t)y * j->lb;
        int ltp = !memcmp(cur, up, j->lb);
        qm_code(j, TPB2CX, ltp == ltp_old);
        ltp_old = ltp;
        if (!ltp)
            code_line(j, cur, up);
        up = cur;
    }
    qm_flush(j);
    // trailing 0x00 bytes are implied (T.82 decoders read 0x00 past the
    // end); a 0xff left last keeps its stuffing
    while (j->band_len > start && j->band[j->band_len - 1] == 0x00)
        j->band_len--;
    if (j->band_len > start && j->band[j->band_len - 1] == 0xff)
        band_byte(j, 0x00);
}

// ---------------------------------------------------------------------------
// UFR page

static void emit_band(job_t *j, size_t len, int lines)
{
    int y = j->band_y;
    uint8_t h[32];
    size_t n = 0;
#define H(...)                                                                                                         \
    do {                                                                                                               \
        const uint8_t b_[] = {__VA_ARGS__};                                                                            \
        memcpy(h + n, b_, sizeof(b_));                                                                                 \
        n += sizeof(b_);                                                                                               \
    } while (0)
    H(0x62, 0xe3, 0x85, j->band_w >> 8, j->band_w & 0xff, lines >> 8, lines & 0xff, 0xe8);
    if (y == 0)
        H(0xa1, 0x00, 0x00);            // (0, 0) in bytes
    else
        H(0xa5, 0x00, 0x00, y >> 8, y & 0xff);
    H(0xe1, 0x05, 0xd7);
    // the length twice: as a signed 8-, 16- or 32-bit value (a0, a4, a8),
    // then as an unsigned one (9c, 9d, 9e), the shortest form as Canon's
    // driver (seen: a0/9c up to 24, a4 up to 32767, 9d up to 65535)
    if (len < 128)
        H(0xa0, len);
    else if (len < 32768)
        H(0xa4, len >> 8, len & 0xff);
    else
        H(0xa8, len >> 24, len >> 16, len >> 8, len & 0xff);
    if (len < 128)
        H(0x9c, len);
    else if (len < 65536)
        H(0x9d, len >> 8, len & 0xff);
    else
        H(0x9e, len >> 24, len >> 16, len >> 8, len & 0xff);
#undef H
    if (n + len <= UFR_WRITE_MAX) {
        ufr_write2(j, h, n, j->band, len);
    } else {
        ufr(j, h, n);
        ufr_end(j);
        for (size_t off = 0; off < len; off += UFR_WRITE_MAX) {
            ufr(j, j->band + off, len - off < UFR_WRITE_MAX ? len - off : UFR_WRITE_MAX);
            ufr_end(j);     // each piece ends its own packets
        }
    }
    ufr_end(j);
    j->band_y += lines;
    j->band_len = 0;
    j->band_lines = 0;
}

static void end_mark(job_t *j, bool reset)
{
    band_byte(j, 0xff);
    band_byte(j, reset ? 0x03 : 0x02);  // SDRST also resets the probabilities
    j->reset = reset;
    if (reset)
        memset(j->st, 0, sizeof(j->st));
}

// The stripe in j->lines is complete. Bands are two stripes, the second (or
// the page's last) ending in SDRST, as Canon's driver sends them.
static void end_stripe(job_t *j, bool last)
{
    bool reset = j->band_lines > 0 || last;
    encode_stripe(j);
    end_mark(j, reset);
    memcpy(j->above, j->lines + (size_t)(j->n - 1) * j->lb, j->lb);
    memcpy(j->above2, j->n > 1 ? j->lines + (size_t)(j->n - 2) * j->lb : j->white, j->lb);
    if (reset)
        emit_band(j, j->band_len, j->band_lines + j->n);
    else
        j->band_lines = j->n;
    j->n = 0;
}

// the line at j->n of the stripe is filled
static void line_done(job_t *j)
{
    j->n++;
    j->y++;
    if (j->n == STRIPE || j->y == j->paper->height)
        end_stripe(j, j->y == j->paper->height);
}

int canon_ufr2_line_bytes(void *job)
{
    return ((job_t *)job)->lb;
}

int canon_ufr2_put_line(void *job, const uint8_t *bits)
{
    job_t *j = job;
    if (j->err || !j->in_page)
        return -1;
    if (j->y >= j->paper->height)
        return 0;               // below the paper
    uint8_t *dst = j->lines + (size_t)j->n * j->lb;
    if (bits)
        memcpy(dst, bits, j->lb);
    else
        memset(dst, 0, j->lb);
    line_done(j);
    return j->err;
}

// ---------------------------------------------------------------------------
// driver

static void free_page(job_t *j)
{
    free(j->lines);
    free(j->above);
    free(j->above2);
    free(j->white);
    j->lines = j->above = j->above2 = j->white = NULL;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper, int sides)
{
    if (paper < 0 || paper >= (int)(sizeof(UFR_PAPERS) / sizeof(UFR_PAPERS[0])))
        paper = 0;
    job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    j->scale = dpi == 300 ? 2 : 1;
    j->duplex = sides != PRINTER_SIDES_ONE;
    j->short_edge = sides == PRINTER_SIDES_SHORT_EDGE;
    j->paper = &UFR_PAPERS[paper];
    j->band_w = (j->paper->width + 31) & ~31;
    j->lb = j->band_w * 2 / 8;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            j->gray_thr[y][x] = 255 - (SCREEN[y][x] * 255 + 128) / 32;
    job_header(j);
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    job_t *j = job;
    if (j->err)
        return -1;
    j->lines = malloc((size_t)STRIPE * j->lb);
    j->above = calloc(1, j->lb);
    j->above2 = calloc(1, j->lb);
    j->white = calloc(1, j->lb);
    if (!j->lines || !j->above || !j->above2 || !j->white) {
        free_page(j);
        return j->err = -1;
    }
    j->src_w = src_w;
    j->src_h = src_h;
    j->bpp = bytes_pp;
    j->y = j->n = 0;
    j->band_y = j->band_lines = 0;
    j->band_len = 0;
    memset(j->st, 0, sizeof(j->st));
    j->reset = true;
    j->in_page = true;
    int w = j->paper->width, h = j->paper->height, xd = j->band_w * 2;
    UFR(j, 0x03, 0xe7, 0x85, w >> 8, w & 0xff, h >> 8, h & 0xff, 0xde, 0x80, 0x00, 0xc8, 0x00, 0xca, 0xa1, 0x00,
        0x00, 0xcb, 0x00);
    ufr_end(j);                         // with the job header before the first page
    UFR(j, 0x61, 0xe6, 0x80, 0x02, 0xe5, 0x00);     // goes with the first band
    // JBIG BIH: one layer, one plane, xd x h, 128-line stripes, LRLTWO | TPBON
    const uint8_t bih[20] = {0, 0, 1, 0, xd >> 24, xd >> 16, xd >> 8, xd, h >> 24, h >> 16, h >> 8, h,
                             0, 0, 0, STRIPE, 0, 0, 0, 0x48};
    for (int i = 0; i < 20; i++)
        band_byte(j, bih[i]);
    return j->err;
}

// the end of the white (255) stretch of a gray line that starts at x
static int white_run(const uint8_t *px, int x, int w)
{
    while (x < w && (x & 3))
        if (px[x] == 255)
            x++;
        else
            return x;
    for (; x + 4 <= w; x += 4) {
        uint32_t v;
        memcpy(&v, px + x, 4);
        if (v != 0xffffffffu)
            break;
    }
    while (x < w && px[x] == 255)
        x++;
    return x;
}

// A source line at the job's dpi: `scale` raster lines, 2 * scale dots
// across per pixel, through the screen
static int page_line(void *job, int y, const uint8_t *px)
{
    job_t *j = job;
    if (j->err || !j->in_page)
        return -1;
    int s = j->scale, dots = 2 * s;
    while (j->y < y * s && j->y < j->paper->height)     // skipped lines are white
        if (canon_ufr2_put_line(j, NULL))
            return -1;
    int w = j->src_w < j->band_w / s ? j->src_w : j->band_w / s;
    for (int k = 0; k < s && j->y < j->paper->height; k++) {
        if (!px) {
            if (canon_ufr2_put_line(j, NULL))
                return -1;
            continue;
        }
        if (j->bpp == 1 && white_run(px, 0, w) == w) {     // a white line
            if (canon_ufr2_put_line(j, NULL))
                return -1;
            continue;
        }
        uint8_t *dst = j->lines + (size_t)j->n * j->lb;
        memset(dst, 0, j->lb);
        const uint8_t *thr = j->gray_thr[j->y & 3];
        for (int x = 0; x < w; x++) {
            if (j->bpp == 1 && px[x] == 255) {
                x = white_run(px, x, w) - 1;    // skip the white stretch
                continue;
            }
            const uint8_t *p = px + (size_t)x * j->bpp;
            int g = j->bpp >= 3 ? (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8 : p[0];
            if (g == 255)
                continue;
            for (int d = 0, X = x * dots; d < dots; d++, X++)
                if (g < thr[X & 7])
                    dst[X >> 3] |= 0x80 >> (X & 7);
        }
        line_done(j);
    }
    return j->err;
}

static int page_end(void *job)
{
    job_t *j = job;
    if (!j->in_page)
        return -1;
    while (!j->err && j->y < j->paper->height)
        canon_ufr2_put_line(j, NULL);
    UFR(j, 0x13);
    ufr_end(j);
    j->in_page = false;
    j->pages++;
    free_page(j);
    return j->err;
}

static int job_end(void *job)
{
    job_t *j = job;
    if (j->in_page)
        page_end(j);    // a cancelled page still comes out
    job_trailer(j);
    int ret = j->err ? -1 : 0;
    free_page(j);
    free(j->band);
    free(j);
    return ret;
}

// The printer's paper sizes (UFR_PAPERS: same order), borders 5 mm (the PPD's
// imageable area)
static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

// Seen on the MF221d: 04a9:27a8, "MFG:Canon;MDL:MF220 Series;CMD:LIPSLX,CPCA;
// CLS:PRINTER;DES:Canon MF220 Series;CID:CA UFRII BW OIP;"
const printer_driver_t canon_mf220_driver = {
    .id = "canon-mf220",
    .make_model = "Canon MF220 Series",
    .usb_mfg = "Canon",
    .usb_mdl = "MF220 Series",
    .usb_vid = 0x04a9,
    .usb_pid = 0x27a8,
    .color = false,
    .scanner = PRINTER_SCANNER_ICLASS,     // the glass, SANE's imageCLASS protocol (iclass_scan.c)
    .duplex = true,
    .resolutions = {300, 600},     // 600: what Canon's driver sends; 300 scaled up
    .nresolutions = 2,
    .papers = PAPERS,
    .npapers = 2,
    .margin_left = 500,
    .margin_right = 500,
    .margin_top = 500,
    .margin_bottom = 500,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
};
