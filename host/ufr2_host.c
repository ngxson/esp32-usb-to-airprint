// Host checks of the Canon UFR II LT driver (components/printers/canon_ufr2.c).
//
//   ufr2_host selftest              round trips through an independent decoder
//   ufr2_host decode in.prn [out]   the pages of a stream (ours or Canon's) as
//                                   out-N.pbm, with the stream's structure
//   ufr2_host recode canon.prn      decodes a stream from Canon's macOS driver,
//                                   encodes its pages again with our driver and
//                                   compares the two streams
//
// The decoder follows ITU-T T.82 (JBIG, lowest resolution layer, two-line
// template, typical prediction); it shares nothing with the encoder but the
// probability table.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canon_ufr2.h"
#include "printer_driver.h"

extern const printer_driver_t canon_mf220_driver;

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, data, len);
    b->len += len;
}

static int buf_write(void *ctx, const uint8_t *data, size_t len)
{
    buf_add(ctx, data, len);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len ? *len : 1);
    if (fread(p, 1, *len, f) != *len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    return p;
}

static unsigned be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

// ---------------------------------------------------------------------------
// CPCA: the UFR data (packets 0x1a, after their flag byte) and the other
// packets as "op:payload" lines

typedef struct {
    buf_t ufr;
    buf_t other;        // text
    buf_t sizes;        // the UFR packets' payload lengths and kinds, as text ("49,49,0k0," ...)
    int npackets, err;
    bool ufr_end;       // the UFR data closed by an empty packet of kind 0
} cpca_t;

static void cpca_parse(const uint8_t *d, size_t len, cpca_t *c)
{
    memset(c, 0, sizeof(*c));
    size_t i = 0;
    while (i + 20 <= len) {
        if (d[i] != 0xcd || d[i + 1] != 0xca || d[i + 2] != 0x10) {
            printf("CPCA: bad header at %zu\n", i);
            c->err = 1;
            return;
        }
        unsigned op = be16(d + i + 4), n = be16(d + i + 8);
        const uint8_t *p = d + i + 20;
        if (i + 20 + n > len) {
            c->err = 1;
            return;
        }
        if (op == 0x1a) {
            char s[24];
            int k = snprintf(s, sizeof(s), "%u%s,", n - 1, d[i + 3] ? "" : "k0");
            buf_add(&c->sizes, s, k);
            // data: kind 2; the last packet, empty: kind 0
            if (n < 1 || p[0] != 0x01 || d[i + 3] != (n > 1 ? 0x02 : d[i + 3]))
                c->err = 1;
            c->ufr_end = d[i + 3] == 0x00;
            if (n > 1)
                buf_add(&c->ufr, p + 1, n - 1);
        } else {
            char line[1100];
            int k = snprintf(line, sizeof(line), "%04x:", op);
            for (unsigned j = 0; j < n && k < (int)sizeof(line) - 3; j++)
                k += snprintf(line + k, sizeof(line) - k, "%02x", p[j]);
            line[k++] = '\n';
            buf_add(&c->other, line, k);
        }
        c->npackets++;
        i += 20 + n;
    }
    if (i != len)
        c->err = 1;
}

// ---------------------------------------------------------------------------
// JBIG decoder (T.82)

static const uint32_t QE[113] = {
    0x5a1d0181, 0x25860e02, 0x11141003, 0x080b1204, 0x03d81405, 0x01da1706, 0x00e51907, 0x006f1c08, 0x00361e09,
    0x001a210a, 0x000d230b, 0x0006090c, 0x00030a0d, 0x00010c0d, 0x5a7f0f8f, 0x3f252410, 0x2cf22611, 0x207c2712,
    0x17b92813, 0x11822a14, 0x0cef2b15, 0x09a12d16, 0x072f2e17, 0x055c3018, 0x04063119, 0x0303331a, 0x0240341b,
    0x01b1361c, 0x0144381d, 0x00f5391e, 0x00b73b1f, 0x008a3c20, 0x00683e21, 0x004e3f22, 0x003b2023, 0x002c2109,
    0x5ae125a5, 0x484c4026, 0x3a0d4127, 0x2ef14328, 0x261f4429, 0x1f33452a, 0x19a8462b, 0x1518482c, 0x1177492d,
    0x0e744a2e, 0x0bfb4b2f, 0x09f84d30, 0x08614e31, 0x07064f32, 0x05cd3033, 0x04de3234, 0x040f3235, 0x03633336,
    0x02d43437, 0x025c3538, 0x01f83639, 0x01a4373a, 0x0160383b, 0x0125393c, 0x00f63a3d, 0x00cb3b3e, 0x00ab3d3f,
    0x008f3d20, 0x5b1241c1, 0x4d045042, 0x412c5143, 0x37d85244, 0x2fe85345, 0x293c5446, 0x23795647, 0x1edf5748,
    0x1aa95749, 0x174e484a, 0x1424484b, 0x119c4a4c, 0x0f6b4a4d, 0x0d514b4e, 0x0bb64d4f, 0x0a404d30, 0x583250d1,
    0x4d1c5852, 0x438e5953, 0x3bdd5a54, 0x34ee5b55, 0x2eae5c56, 0x299a5d57, 0x25165647, 0x557058d9, 0x4ca95f5a,
    0x44d9605b, 0x3e22615c, 0x3824635d, 0x32b4635e, 0x2e175d56, 0x56a85fe0, 0x4f466561, 0x47e56662, 0x41cf6763,
    0x3c3d6864, 0x375e635d, 0x52316966, 0x4c0f6a67, 0x46396b68, 0x415e6763, 0x562769ea, 0x50e76c6b, 0x4b856d67,
    0x55976e6d, 0x504f6f6b, 0x5a106eef, 0x5522706d, 0x59eb70ef,
};

typedef struct {
    const uint8_t *p, *end;     // PSCD (stuffed), without the marker
    uint32_t c, a;
    int ct;
    uint8_t st[1024];
} qd_t;

static uint32_t qd_byte(qd_t *d)
{
    if (d->p >= d->end)
        return 0;               // past the end: 0x00 (trailing zeros omitted)
    uint8_t b = *d->p++;
    if (b == 0xff)
        d->p++;                 // the stuffed 0x00
    return b;
}

static void qd_start(qd_t *d, const uint8_t *p, const uint8_t *end)
{
    d->p = p;
    d->end = end;
    d->c = qd_byte(d) << 24;
    d->c |= qd_byte(d) << 16;
    d->c |= qd_byte(d) << 8;
    d->ct = 8;
    d->a = 0x10000;
}

static int qd_decode(qd_t *d, int cx)
{
    uint8_t *s = &d->st[cx];
    uint32_t q = QE[*s & 0x7f], lsz = q >> 16;
    int mps = *s >> 7, pix;
    bool lps;
    d->a -= lsz;
    if ((d->c >> 16) < d->a) {          // the lower sub-interval
        if (d->a >= 0x8000)
            return mps;
        lps = d->a < lsz;               // exchanged: the LPS is below
    } else {
        d->c -= d->a << 16;
        lps = d->a >= lsz;
        d->a = lsz;
    }
    pix = lps ? !mps : mps;
    *s = lps ? ((*s ^ q) & 0x80) | ((q >> 8) & 0x7f) : (*s & 0x80) | (q & 0x7f);
    do {
        if (d->ct == 0) {
            d->c |= qd_byte(d) << 8;
            d->ct = 8;
        }
        d->a <<= 1;
        d->c <<= 1;
        d->ct--;
    } while (d->a < 0x8000);
    return pix;
}

typedef struct {
    int w, h, lb;               // dots
    uint8_t *bits;              // h * lb, 1 = black
} image_t;

// A whole BIE (BIH + stripes). 0 = ok.
static int jbig_decode(const uint8_t *bie, size_t len, image_t *img, int *nstripes)
{
    if (len < 20 || bie[0] || bie[1] || bie[2] != 1) {
        printf("JBIG: not a one-layer one-plane BIH\n");
        return -1;
    }
    uint32_t xd = be32(bie + 4), yd = be32(bie + 8), l0 = be32(bie + 12);
    if (bie[16] || bie[17] || bie[19] != 0x48 || !l0 || xd > 40000 || yd > 40000) {
        printf("JBIG: unexpected BIH options %02x (MX %d)\n", bie[19], bie[16]);
        return -1;
    }
    img->w = xd;
    img->h = yd;
    img->lb = (xd + 7) / 8;
    img->bits = calloc((size_t)yd, img->lb);
    uint8_t *white = calloc(1, img->lb);
    qd_t d;
    memset(d.st, 0, sizeof(d.st));
    const uint8_t *p = bie + 20, *end = bie + len;
    uint32_t y = 0;
    bool reset = true;          // first stripe or after SDRST: white above, TP afresh
    *nstripes = 0;
    while (y < yd) {
        const uint8_t *m = p;
        while (m + 1 < end && !(m[0] == 0xff && m[1] != 0x00))
            m += m[0] == 0xff ? 2 : 1;
        if (m + 1 >= end || (m[1] != 0x02 && m[1] != 0x03)) {
            printf("JBIG: stripe %d: no SDNORM/SDRST marker\n", *nstripes);
            free(white);
            return -1;
        }
        qd_start(&d, p, m);
        uint32_t first = y;
        int ltp_old = reset ? 0 : y < 2 ? !memcmp(img->bits + (y - 1) * img->lb, white, img->lb)
                                        : !memcmp(img->bits + (y - 1) * img->lb, img->bits + (y - 2) * img->lb, img->lb);
        uint32_t stop = y + l0 < yd ? y + l0 : yd;
        for (; y < stop; y++) {
            uint8_t *cur = img->bits + (size_t)y * img->lb;
            const uint8_t *up = y > first || !reset ? cur - img->lb : white;
            int ltp = qd_decode(&d, 0x195) ? ltp_old : !ltp_old;
            ltp_old = ltp;
            if (ltp) {
                memcpy(cur, up, img->lb);
                continue;
            }
            uint32_t c4 = 0;
            for (int i = 0; i < img->lb; i++) {
                // the line above, x-3 .. x+2 of dot k of this byte: bits 18-k .. 13-k
                uint32_t w = (i ? up[i - 1] << 16 : 0) | up[i] << 8 | (i + 1 < img->lb ? up[i + 1] : 0);
                uint32_t b = 0;
                for (int k = 0; k < 8; k++) {
                    int pix = qd_decode(&d, ((w >> (13 - k)) & 0x3f) << 4 | c4);
                    b = b << 1 | pix;
                    c4 = ((c4 << 1) | pix) & 0xf;
                }
                cur[i] = b;
            }
        }
        reset = m[1] == 0x03;   // SDRST: the next stripe starts over
        if (reset)
            memset(d.st, 0, sizeof(d.st));
        p = m + 2;
        (*nstripes)++;
    }
    free(white);
    if (p != end)
        printf("JBIG: %zu bytes after the last stripe\n", (size_t)(end - p));
    return p == end ? 0 : -1;
}

// ---------------------------------------------------------------------------
// UFR: pages of bands

#define MAX_PAGES 16

typedef struct {
    int paper_code, duplex, page_w, page_h, band_w;
    int npages;
    image_t page[MAX_PAGES];
    int nbands[MAX_PAGES], nstripes[MAX_PAGES], max_band;
} ufr_doc_t;

static int ufr_parse(const uint8_t *u, size_t len, ufr_doc_t *doc, bool verbose)
{
    memset(doc, 0, sizeof(*doc));
    // the job: "01 ... c3 <paper> c5 <duplex> c6 00 51 f2 00"
    if (len < 30 || u[0] != 0x01 || u[21] != 0xc3 || u[23] != 0xc5 || u[28] != 0xf2) {
        printf("UFR: unexpected job header\n");
        return -1;
    }
    doc->paper_code = u[22];
    doc->duplex = u[24];
    size_t i = 30;
    buf_t bie = {0};
    while (i < len) {
        if (u[i] == 0x12 && i + 2 == len && u[i + 1] == 0x11)
            return 0;
        if (u[i] != 0x03 || i + 24 > len || u[i + 1] != 0xe7 || u[i + 2] != 0x85 || u[i + 22] != 0xe5) {
            printf("UFR: expected a page at %zu: %02x\n", i, u[i]);
            return -1;
        }
        if (doc->npages == MAX_PAGES)
            return -1;
        doc->page_w = be16(u + i + 3);
        doc->page_h = be16(u + i + 5);
        i += 24;
        bie.len = 0;
        int y = 0, pg = doc->npages;
        while (i < len && u[i] == 0x62) {
            const uint8_t *b = u + i;
            if (b[1] != 0xe3 || b[2] != 0x85 || b[7] != 0xe8) {
                printf("UFR: bad band at %zu\n", i);
                return -1;
            }
            int bw = be16(b + 3), bh = be16(b + 5), by, n, k = 8;
            if (b[k] == 0xa1) {
                by = b[k + 2];
                k += 3;
            } else if (b[k] == 0xa5) {
                by = be16(b + k + 3);
                k += 5;
            } else
                return -1;
            if (b[k] != 0xe1 || b[k + 1] != 0x05 || b[k + 2] != 0xd7)
                return -1;
            k += 3;
            if (b[k] == 0xa0 && b[k + 2] == 0x9c && b[k + 1] == b[k + 3]) {
                n = b[k + 1];
                k += 4;
            } else if (b[k] == 0xa4 && b[k + 3] == 0x9d && be16(b + k + 1) == be16(b + k + 4)) {
                n = be16(b + k + 1);
                k += 6;
            } else if (b[k] == 0xa8 && b[k + 5] == 0x9d && be32(b + k + 1) == be16(b + k + 6)) {
                n = be32(b + k + 1);
                k += 8;
            } else if (b[k] == 0xa8 && b[k + 5] == 0x9e && be32(b + k + 1) == be32(b + k + 6)) {
                n = be32(b + k + 1);
                k += 10;
            } else {
                printf("UFR: bad band length at %zu\n", i);
                return -1;
            }
            CHECK(by == y, "band at line %d, expected %d", by, y);
            doc->band_w = bw;
            y += bh;
            buf_add(&bie, b + k, n);
            if (n > doc->max_band)
                doc->max_band = n;
            doc->nbands[pg]++;
            i += k + n;
        }
        if (i >= len || u[i] != 0x13) {
            printf("UFR: no page end at %zu\n", i);
            return -1;
        }
        i++;
        if (jbig_decode(bie.p, bie.len, &doc->page[pg], &doc->nstripes[pg]))
            return -1;
        CHECK(y == doc->page[pg].h, "bands cover %d lines of %d", y, doc->page[pg].h);
        CHECK(doc->page[pg].w == 2 * doc->band_w, "JBIG width %d, band width %d", doc->page[pg].w, doc->band_w);
        doc->npages++;
        if (verbose)
            printf("page %d: %dx%d dots (page %dx%d at 600 dpi), %d bands, %d stripes\n", pg + 1, doc->page[pg].w,
                   doc->page[pg].h, doc->page_w, doc->page_h, doc->nbands[pg], doc->nstripes[pg]);
    }
    printf("UFR: no job end\n");
    return -1;
}

static void free_doc(ufr_doc_t *doc)
{
    for (int i = 0; i < doc->npages; i++)
        free(doc->page[i].bits);
}

static int decode_stream(const uint8_t *d, size_t len, ufr_doc_t *doc, cpca_t *c, bool verbose)
{
    cpca_parse(d, len, c);
    if (c->err) {
        printf("CPCA: bad stream\n");
        return -1;
    }
    return ufr_parse(c->ufr.p, c->ufr.len, doc, verbose);
}

// ---------------------------------------------------------------------------
// our driver

static int paper_index(int code)
{
    return code == 0x64 ? 1 : 0;
}

// a job of bit-raster pages through the driver
static void encode_bits(int paper, int sides, const image_t *pages, int n, buf_t *out)
{
    const printer_driver_t *d = &canon_mf220_driver;
    printer_io_t io = {.write = buf_write, .ctx = out};
    void *job = d->job_begin(d, &io, 600, paper, sides);
    for (int p = 0; p < n; p++) {
        d->page_begin(job, pages[p].w / 2, pages[p].h, 1);
        CHECK(canon_ufr2_line_bytes(job) == pages[p].lb, "line bytes %d, page %d", canon_ufr2_line_bytes(job),
              pages[p].lb);
        for (int y = 0; y < pages[p].h; y++)
            canon_ufr2_put_line(job, pages[p].bits + (size_t)y * pages[p].lb);
        d->page_end(job);
    }
    CHECK(d->job_end(job) == 0, "job_end");
}

static bool settings_line_compared(const char *line)
{
    // the job ticket and the document name carry the client's names; the
    // total length follows from them
    return strncmp(line, "006b:", 5) && strncmp(line, "0015:082b", 9) && strncmp(line, "0012:07d6", 9);
}

static int recode(const char *path)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) {
        printf("%s: cannot read\n", path);
        return 1;
    }
    ufr_doc_t doc;
    cpca_t canon, ours;
    if (decode_stream(d, len, &doc, &canon, true))
        return 1;
    buf_t out = {0};
    // the binding (07e0 03: short edge) is only in the settings packets
    int sides = !doc.duplex ? PRINTER_SIDES_ONE
                : strstr((char *)canon.other.p, "0018:07e003") ? PRINTER_SIDES_SHORT_EDGE
                                                                : PRINTER_SIDES_LONG_EDGE;
    encode_bits(paper_index(doc.paper_code), sides, doc.page, doc.npages, &out);
    cpca_parse(out.p, out.len, &ours);
    CHECK(!ours.err, "our CPCA stream");
    if (getenv("UFR2_OUT")) {
        FILE *f = fopen(getenv("UFR2_OUT"), "wb");
        fwrite(out.p, 1, out.len, f);
        fclose(f);
    }
    // UFR: byte for byte
    size_t n = canon.ufr.len < ours.ufr.len ? canon.ufr.len : ours.ufr.len, k = 0;
    while (k < n && canon.ufr.p[k] == ours.ufr.p[k])
        k++;
    CHECK(k == canon.ufr.len && k == ours.ufr.len, "UFR data differs at byte %zu (Canon %zu bytes, ours %zu)", k,
          canon.ufr.len, ours.ufr.len);
    if (k == canon.ufr.len && k == ours.ufr.len)
        printf("UFR data identical: %zu bytes, %d pages\n", k, doc.npages);
    // the UFR data cut into packets as Canon's driver cuts it (the printer
    // reads page boundaries from them)
    buf_add(&canon.sizes, "", 1);
    buf_add(&ours.sizes, "", 1);
    {
        const char *a = (char *)canon.sizes.p, *b = (char *)ours.sizes.p;
        int same = 0;
        while (*a && *b) {
            size_t la = strcspn(a, ","), lb = strcspn(b, ",");
            if (la != lb || memcmp(a, b, la))
                break;
            a += la + 1;
            b += lb + 1;
            same++;
        }
        CHECK(!*a && !*b, "UFR packet %d differs: Canon %.20s..., ours %.20s...", same + 1, a, b);
        if (!*a && !*b)
            printf("UFR packets identical: %d\n", same);
    }
    // settings: every packet but the names
    char *a, *b;
    buf_add(&canon.other, "", 1);
    buf_add(&ours.other, "", 1);
    a = (char *)canon.other.p;
    b = (char *)ours.other.p;
    int lines = 0;
    for (;;) {
        while (*a && !settings_line_compared(a))
            a = strchr(a, '\n') + 1;
        while (*b && !settings_line_compared(b))
            b = strchr(b, '\n') + 1;
        if (!*a || !*b) {
            CHECK(!*a && !*b, "settings packets: one stream has more");
            break;
        }
        size_t la = strchr(a, '\n') - a, lb = strchr(b, '\n') - b;
        CHECK(la == lb && !memcmp(a, b, la), "settings packet: Canon %.*s, ours %.*s", (int)la, a, (int)lb, b);
        a += la + 1;
        b += lb + 1;
        lines++;
    }
    // the total length in the trailer is ours
    const char *t = strstr((char *)ours.other.p, "0012:07d6");
    CHECK(t && strtoul(t + 9, NULL, 16) == out.len, "trailer length");
    printf("settings packets compared: %d; CPCA packets: Canon %d, ours %d\n", lines, canon.npackets, ours.npackets);
    free_doc(&doc);
    free(d);
    return failures ? 1 : 0;
}

static int decode_cmd(const char *path, const char *prefix)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d)
        return 1;
    ufr_doc_t doc;
    cpca_t c;
    if (decode_stream(d, len, &doc, &c, true))
        return 1;
    printf("paper code %02x, largest band %d bytes, CPCA packets %d\n%s", doc.paper_code, doc.max_band, c.npackets,
           (char *)c.other.p);
    for (int i = 0; prefix && i < doc.npages; i++) {
        char name[512];
        snprintf(name, sizeof(name), "%s-%d.pbm", prefix, i + 1);
        FILE *f = fopen(name, "wb");
        fprintf(f, "P4\n%d %d\n", doc.page[i].w, doc.page[i].h);
        fwrite(doc.page[i].bits, doc.page[i].lb, doc.page[i].h, f);
        fclose(f);
    }
    free_doc(&doc);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// self test

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1103515245 + 12345;
    return rng >> 8;
}

static void new_image(image_t *img, int w, int h)
{
    img->w = w;
    img->h = h;
    img->lb = (w + 7) / 8;
    img->bits = calloc((size_t)h, img->lb);
}

static int selftest(void)
{
    // 1. bit pages: text-like blocks, lines, a black band at the edges,
    //    repeated lines (typical prediction), a white page
    image_t pages[3];
    for (int p = 0; p < 3; p++) {
        new_image(&pages[p], p == 1 ? 10240 : 9920, p == 1 ? 6600 : 7016);
        image_t *im = &pages[p];
        for (int y = 0; p != 2 && y < im->h; y++) {
            uint8_t *l = im->bits + (size_t)y * im->lb;
            if ((y / 40) % 3 == 0)
                for (int x = 0; x < im->lb; x++)
                    l[x] = (x * 7 + y / 3) % 11 < 3 ? rnd() & 0xff : 0;
            if (y % 300 == 7)
                memset(l, 0xff, im->lb);
            l[0] |= y > 6000 ? 0xff : 0;
            l[im->lb - 1] |= y < 100 ? 0x01 : 0;
        }
    }
    buf_t out = {0};
    encode_bits(0, PRINTER_SIDES_ONE, pages, 1, &out);
    buf_t out2 = {0};
    encode_bits(1, PRINTER_SIDES_ONE, &pages[1], 1, &out2);
    buf_t out3 = {0};
    encode_bits(0, PRINTER_SIDES_ONE, &pages[2], 1, &out3);
    const buf_t *outs[] = {&out, &out2, &out3};
    for (int t = 0; t < 3; t++) {
        ufr_doc_t doc;
        cpca_t c;
        int r = decode_stream(outs[t]->p, outs[t]->len, &doc, &c, false);
        const image_t *want = &pages[t];
        CHECK(r == 0 && doc.npages == 1, "page %d: decode", t);
        if (r == 0) {
            CHECK(doc.page[0].w == want->w && doc.page[0].h == want->h, "page %d: size", t);
            CHECK(!memcmp(doc.page[0].bits, want->bits, (size_t)want->h * want->lb), "page %d: round trip differs", t);
            CHECK(doc.page_w == (t == 1 ? 5100 : 4958), "page %d: page width %d", t, doc.page_w);
            printf("bit page %d: %zu bytes, %d bands, identical after decoding\n", t, outs[t]->len, doc.nbands[0]);
        }
        free_doc(&doc);
    }

    // 1b. duplex: the job header's c5 byte and the settings 07df (sides),
    //     07e0 (binding) as Canon's driver sends them
    {
        static const struct {
            int sides, c5;
            const char *df, *e0;
        } cases[] = {
            {PRINTER_SIDES_ONE, 0, "0018:07df01", "0018:07e004"},
            {PRINTER_SIDES_LONG_EDGE, 1, "0018:07df04", "0018:07e004"},
            {PRINTER_SIDES_SHORT_EDGE, 1, "0018:07df04", "0018:07e003"},
        };
        for (int i = 0; i < 3; i++) {
            buf_t o = {0};
            image_t two[2] = {pages[2], pages[2]};
            encode_bits(0, cases[i].sides, two, 2, &o);
            ufr_doc_t doc;
            cpca_t c;
            int r = decode_stream(o.p, o.len, &doc, &c, false);
            buf_add(&c.other, "", 1);
            CHECK(r == 0 && doc.npages == 2 && doc.duplex == cases[i].c5 && strstr((char *)c.other.p, cases[i].df) &&
                      strstr((char *)c.other.p, cases[i].e0),
                  "sides %d: header %d, settings %s %s", cases[i].sides, doc.duplex, cases[i].df, cases[i].e0);
            free_doc(&doc);
        }
        printf("duplex: header and settings for one-sided, long and short edge\n");
    }

    // 2. noise on the top 2000 lines: bands longer than 65535 bytes (32-bit
    //    length fields)
    image_t noise;
    new_image(&noise, 9920, 7016);
    for (size_t i = 0; i < (size_t)2000 * noise.lb; i++)
        noise.bits[i] = (uint8_t)rnd();
    buf_t on = {0};
    encode_bits(0, PRINTER_SIDES_ONE, &noise, 1, &on);
    {
        ufr_doc_t doc;
        cpca_t c;
        int r = decode_stream(on.p, on.len, &doc, &c, false);
        CHECK(r == 0, "noise page: decode");
        if (r == 0) {
            CHECK(doc.max_band > 65535, "noise page: largest band %d bytes", doc.max_band);
            CHECK(!memcmp(doc.page[0].bits, noise.bits, (size_t)noise.h * noise.lb), "noise page: round trip differs");
            printf("noise page: %zu bytes, %d bands, largest %d bytes, identical after decoding\n", on.len,
                   doc.nbands[0], doc.max_band);
        }
        free_doc(&doc);
    }

    // 3. gray through page_line at 600 and 300 dpi: a ramp on the top 3000
    //    raster lines; darker columns, more black dots
    for (int dpi = 600; dpi >= 300; dpi -= 300) {
        const printer_driver_t *d = &canon_mf220_driver;
        buf_t g = {0};
        printer_io_t io = {.write = buf_write, .ctx = &g};
        void *job = d->job_begin(d, &io, dpi, 0, PRINTER_SIDES_ONE);
        int s = 600 / dpi, w = 4960 / s, h = 7016 / s;
        uint8_t *line = malloc(w);
        for (int x = 0; x < w; x++)
            line[x] = 255 - x * 255 / (w - 1);
        d->page_begin(job, w, h - 50, 1);
        for (int y = 0; y < h - 50; y++)
            d->page_line(job, y, y < 3000 / s ? line : NULL);
        d->page_end(job);
        d->job_end(job);
        ufr_doc_t doc;
        cpca_t c;
        int r = decode_stream(g.p, g.len, &doc, &c, false);
        CHECK(r == 0, "gray page: decode");
        if (r == 0) {
            int prev = -1;
            for (int s = 0; s < 8; s++) {     // 8 column strips of 1240 dots, 2000 lines
                int black = 0;
                for (int y = 0; y < 2000; y++)
                    for (int x = s * 155; x < (s + 1) * 155; x++)
                        black += __builtin_popcount(doc.page[0].bits[(size_t)y * doc.page[0].lb + x]);
                CHECK(black > prev, "gray ramp: strip %d has %d black dots, strip before %d", s, black, prev);
                prev = black;
            }
            int below = 0;
            for (size_t i = (size_t)3000 * doc.page[0].lb; i < (size_t)doc.page[0].h * doc.page[0].lb; i++)
                below |= doc.page[0].bits[i];
            CHECK(!below, "gray page: white below the ramp");
            printf("gray page, %d dpi: %zu bytes, ramp dark to light\n", dpi, g.len);
        }
        free_doc(&doc);
        free(line);
    }
    printf(failures ? "ufr2 selftest: %d FAILED\n" : "ufr2 selftest: ok\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return selftest();
    if (argc >= 3 && !strcmp(argv[1], "decode"))
        return decode_cmd(argv[2], argc > 3 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "recode"))
        return recode(argv[2]);
    fprintf(stderr, "usage: ufr2_host selftest | decode in.prn [out-prefix] | recode canon.prn\n");
    return 2;
}
