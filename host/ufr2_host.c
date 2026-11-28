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
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

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
    if (!len)
        return;
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
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *p = NULL;
    if (fseek(f, 0, SEEK_END))
        goto done;
    long size = ftell(f);
    if (size < 0 || (uintmax_t)size > SIZE_MAX || fseek(f, 0, SEEK_SET))
        goto done;
    p = malloc(size ? (size_t)size : 1);
    if (!p)
        goto done;
    if (fread(p, 1, (size_t)size, f) != (size_t)size) {
        free(p);
        p = NULL;
    } else
        *len = (size_t)size;
done:
    if (fclose(f)) {
        free(p);
        p = NULL;
    }
    if (!p)
        *len = 0;
    return p;
}

// Both binary captures and PBMs must reach the filesystem before success.
static int write_file(const char *path, const char *header, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ret = header && fputs(header, f) == EOF ? -1 : 0;
    if (!ret && len && fwrite(data, 1, len, f) != len)
        ret = -1;
    if (fclose(f))
        ret = -1;
    return ret;
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
    buf_t sizes;        // the UFR packets' payload lengths and kinds, as text ("49k2,49k2,0k0," ...)
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
            goto done;
        }
        unsigned op = be16(d + i + 4), n = be16(d + i + 8);
        const uint8_t *p = d + i + 20;
        if (i + 20 + n > len) {
            c->err = 1;
            goto done;
        }
        if (op == 0x1a) {
            char s[24];
            int k = snprintf(s, sizeof(s), "%uk%u,", n ? n - 1 : 0, d[i + 3]);
            buf_add(&c->sizes, s, k);
            // data: kind 2; the last packet, empty: kind 0
            if (n < 1 || p[0] != 0x01 || (d[i + 3] != 0x02 && !(n == 1 && d[i + 3] == 0x00)))
                c->err = 1;
            c->ufr_end = d[i + 3] == 0x00;
            if (n > 1)
                buf_add(&c->ufr, p + 1, n - 1);
        } else {
            size_t size = 6 + (size_t)n * 2;     // op, colon, complete hex payload, newline
            char *line = malloc(size);
            if (!line) {
                c->err = 1;
                goto done;
            }
            snprintf(line, 6, "%04x:", op);
            static const char hex[] = "0123456789abcdef";
            for (unsigned j = 0; j < n; j++) {
                line[5 + j * 2] = hex[p[j] >> 4];
                line[6 + j * 2] = hex[p[j] & 15];
            }
            line[size - 1] = '\n';
            buf_add(&c->other, line, size);
            free(line);
        }
        c->npackets++;
        i += 20 + n;
    }
    if (i != len)
        c->err = 1;
done:
    buf_add(&c->other, "", 1);
    buf_add(&c->sizes, "", 1);
}

static void free_cpca(cpca_t *c)
{
    free(c->ufr.p);
    free(c->other.p);
    free(c->sizes.p);
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
    if (b == 0xff && d->p < d->end)
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
    if (!xd || !yd || bie[16] || bie[17] || bie[19] != 0x48 || !l0 || xd > 40000 || yd > 40000) {
        printf("JBIG: unexpected BIH options %02x (MX %d)\n", bie[19], bie[16]);
        return -1;
    }
    img->w = xd;
    img->h = yd;
    img->lb = (xd + 7) / 8;
    img->bits = calloc((size_t)yd, img->lb);
    uint8_t *white = calloc(1, img->lb);
    if (!img->bits || !white) {
        free(white);
        return -1;
    }
    qd_t d;
    memset(d.st, 0, sizeof(d.st));
    const uint8_t *p = bie + 20, *end = bie + len;
    uint32_t y = 0;
    bool reset = true;          // first stripe or after SDRST: white above, TP afresh
    *nstripes = 0;
    while (y < yd) {
        const uint8_t *m = p;
        while ((size_t)(end - m) >= 2 && !(m[0] == 0xff && m[1] != 0x00))
            m += m[0] == 0xff ? 2 : 1;
        if ((size_t)(end - m) < 2 || (m[1] != 0x02 && m[1] != 0x03)) {
            printf("JBIG: stripe %d: no SDNORM/SDRST marker\n", *nstripes);
            free(white);
            return -1;
        }
        qd_start(&d, p, m);
        uint32_t first = y;
        int ltp_old = reset ? 0 : y < 2 ? !memcmp(img->bits + (y - 1) * img->lb, white, img->lb)
                                        : !memcmp(img->bits + (y - 1) * img->lb, img->bits + (y - 2) * img->lb, img->lb);
        uint32_t stop = y + (l0 < yd - y ? l0 : yd - y);
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

static void free_doc(ufr_doc_t *doc)
{
    // Include the current page if decoding failed before npages advanced.
    for (int i = 0; i < MAX_PAGES; i++)
        free(doc->page[i].bits);
    memset(doc, 0, sizeof(*doc));
}

static int ufr_parse(const uint8_t *u, size_t len, ufr_doc_t *doc, bool verbose)
{
    memset(doc, 0, sizeof(*doc));
    buf_t bie = {0};
    // the job: "01 ... c3 <paper> c5 <duplex> c6 00 51 f2 00"
    if (len < 30 || u[0] != 0x01 || u[21] != 0xc3 || u[23] != 0xc5 || u[28] != 0xf2) {
        printf("UFR: unexpected job header\n");
        goto fail;
    }
    doc->paper_code = u[22];
    doc->duplex = u[24];
    size_t i = 30;
    while (i < len) {
        if (u[i] == 0x12 && len - i == 2 && u[i + 1] == 0x11) {
            free(bie.p);
            return 0;
        }
        if (u[i] != 0x03 || len - i < 24 || u[i + 1] != 0xe7 || u[i + 2] != 0x85 || u[i + 22] != 0xe5) {
            printf("UFR: expected a page at %zu: %02x\n", i, u[i]);
            goto fail;
        }
        if (doc->npages == MAX_PAGES)
            goto fail;
        doc->page_w = be16(u + i + 3);
        doc->page_h = be16(u + i + 5);
        i += 24;
        bie.len = 0;
        int y = 0, pg = doc->npages;
        while (i < len && u[i] == 0x62) {
            const uint8_t *b = u + i;
            size_t remaining = len - i;
            if (remaining < 8 || b[1] != 0xe3 || b[2] != 0x85 || b[7] != 0xe8) {
                printf("UFR: bad band at %zu\n", i);
                goto fail;
            }
            int bw = be16(b + 3), bh = be16(b + 5), by;
            uint32_t n;
            size_t k = 8;
            if (remaining - k < 1)
                goto fail;
            if (b[k] == 0xa1 && remaining - k >= 3) {
                by = b[k + 2];
                k += 3;
            } else if (b[k] == 0xa5 && remaining - k >= 5) {
                by = be16(b + k + 3);
                k += 5;
            } else
                goto fail;
            if (remaining - k < 3 || b[k] != 0xe1 || b[k + 1] != 0x05 || b[k + 2] != 0xd7)
                goto fail;
            k += 3;
            if (remaining - k >= 4 && b[k] == 0xa0 && b[k + 2] == 0x9c && b[k + 1] == b[k + 3]) {
                n = b[k + 1];
                k += 4;
            } else if (remaining - k >= 6 && b[k] == 0xa4 && b[k + 3] == 0x9d &&
                       be16(b + k + 1) == be16(b + k + 4)) {
                n = be16(b + k + 1);
                k += 6;
            } else if (remaining - k >= 8 && b[k] == 0xa8 && b[k + 5] == 0x9d &&
                       be32(b + k + 1) == be16(b + k + 6)) {
                n = be32(b + k + 1);
                k += 8;
            } else if (remaining - k >= 10 && b[k] == 0xa8 && b[k + 5] == 0x9e &&
                       be32(b + k + 1) == be32(b + k + 6)) {
                n = be32(b + k + 1);
                k += 10;
            } else {
                goto fail;
            }
            if (n > remaining - k || n > INT_MAX)
                goto fail;
            CHECK(by == y, "band at line %d, expected %d", by, y);
            doc->band_w = bw;
            y += bh;
            buf_add(&bie, b + k, n);
            if (n > (uint32_t)doc->max_band)
                doc->max_band = n;
            doc->nbands[pg]++;
            i += k + n;
        }
        if (i >= len || u[i] != 0x13) {
            printf("UFR: no page end at %zu\n", i);
            goto fail;
        }
        i++;
        if (jbig_decode(bie.p, bie.len, &doc->page[pg], &doc->nstripes[pg]))
            goto fail;
        CHECK(y == doc->page[pg].h, "bands cover %d lines of %d", y, doc->page[pg].h);
        CHECK(doc->page[pg].w == 2 * doc->band_w, "JBIG width %d, band width %d", doc->page[pg].w, doc->band_w);
        doc->npages++;
        if (verbose)
            printf("page %d: %dx%d dots (page %dx%d at 600 dpi), %d bands, %d stripes\n", pg + 1, doc->page[pg].w,
                   doc->page[pg].h, doc->page_w, doc->page_h, doc->nbands[pg], doc->nstripes[pg]);
    }
    printf("UFR: no job end\n");
fail:
    free(bie.p);
    free_doc(doc);
    return -1;
}

static int decode_stream(const uint8_t *d, size_t len, ufr_doc_t *doc, cpca_t *c, bool verbose)
{
    memset(doc, 0, sizeof(*doc));
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
static int encode_bits(int paper, int sides, const image_t *pages, int n, buf_t *out)
{
    const printer_driver_t *d = &canon_mf220_driver;
    printer_io_t io = {.write = buf_write, .ctx = out};
    void *job = d->job_begin(d, &io, 600, paper, sides);
    if (!job)
        return -1;
    int ret = 0;
    for (int p = 0; p < n; p++) {
        // The driver consumes a whole paper-width line, regardless of the
        // source dimensions. Reject incompatible strides before reading pixels.
        if (!pages[p].bits || pages[p].w <= 0 || pages[p].h <= 0 ||
            pages[p].lb != canon_ufr2_line_bytes(job) || pages[p].lb != (pages[p].w + 7) / 8) {
            ret = -1;
            break;
        }
        if (d->page_begin(job, pages[p].w / 2, pages[p].h, 1)) {
            ret = -1;
            break;
        }
        for (int y = 0; y < pages[p].h; y++) {
            if (canon_ufr2_put_line(job, pages[p].bits + (size_t)y * pages[p].lb)) {
                ret = -1;
                break;
            }
        }
        if (d->page_end(job))
            ret = -1;
        if (ret)
            break;
    }
    int end = d->job_end(job);
    return ret ? ret : end;
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
    if (decode_stream(d, len, &doc, &canon, true)) {
        free_cpca(&canon);
        free(d);
        return 1;
    }
    buf_t out = {0};
    // the binding (07e0 03: short edge) is only in the settings packets
    int sides = !doc.duplex ? PRINTER_SIDES_ONE
                : strstr((char *)canon.other.p, "0018:07e003") ? PRINTER_SIDES_SHORT_EDGE
                                                                : PRINTER_SIDES_LONG_EDGE;
    if (encode_bits(paper_index(doc.paper_code), sides, doc.page, doc.npages, &out)) {
        free_doc(&doc);
        free(d);
        free(out.p);
        free_cpca(&canon);
        return 1;
    }
    cpca_parse(out.p, out.len, &ours);
    CHECK(!ours.err, "our CPCA stream");
    int output_error = 0;
    const char *output = getenv("UFR2_OUT");
    if (output && write_file(output, NULL, out.p, out.len)) {
        fprintf(stderr, "%s: cannot write UFR output\n", output);
        output_error = 1;
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
        CHECK(la == lb && !memcmp(a, b, la), "settings packet: Canon %.*s, ours %.*s",
              (int)(la < 1100 ? la : 1100), a, (int)(lb < 1100 ? lb : 1100), b);
        a += la + 1;
        b += lb + 1;
        lines++;
    }
    // the total length in the trailer is ours
    const char *t = strstr((char *)ours.other.p, "0012:07d6");
    CHECK(t && strtoul(t + 9, NULL, 16) == out.len, "trailer length");
    printf("settings packets compared: %d; CPCA packets: Canon %d, ours %d\n", lines, canon.npackets, ours.npackets);
    free_doc(&doc);
    free_cpca(&canon);
    free_cpca(&ours);
    free(out.p);
    free(d);
    return failures || output_error ? 1 : 0;
}

static int decode_cmd(const char *path, const char *prefix)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d)
        return 1;
    ufr_doc_t doc;
    cpca_t c;
    if (decode_stream(d, len, &doc, &c, true)) {
        free_cpca(&c);
        free(d);
        return 1;
    }
    printf("paper code %02x, largest band %d bytes, CPCA packets %d\n%s", doc.paper_code, doc.max_band, c.npackets,
           (char *)c.other.p);
    int output_error = 0;
    for (int i = 0; prefix && i < doc.npages; i++) {
        char name[512], header[64];
        int size = snprintf(name, sizeof(name), "%s-%d.pbm", prefix, i + 1);
        snprintf(header, sizeof(header), "P4\n%d %d\n", doc.page[i].w, doc.page[i].h);
        if (size < 0 || (size_t)size >= sizeof(name) ||
            write_file(name, header, doc.page[i].bits, (size_t)doc.page[i].lb * doc.page[i].h)) {
            fprintf(stderr, "%s: cannot write PBM output\n", name);
            output_error = 1;
            break;
        }
    }
    free_doc(&doc);
    free_cpca(&c);
    free(d);
    return failures || output_error ? 1 : 0;
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

typedef struct {
    buf_t bytes;
    int calls, result;
    size_t flushed;
    bool named_sync;
} flush_test_t;

static int flush_test_write(void *ctx, const uint8_t *data, size_t len)
{
    flush_test_t *t = ctx;
    return buf_write(&t->bytes, data, len);
}

static int flush_test_sync(void *ctx, const char *what)
{
    flush_test_t *t = ctx;
    t->calls++;
    t->named_sync |= what != NULL;
    t->flushed = t->bytes.len;
    return t->result;
}

static bool page_matches(const ufr_doc_t *doc, int w, int h, int lb)
{
    return doc->npages == 1 && doc->page[0].bits && doc->page[0].w == w && doc->page[0].h == h &&
           doc->page[0].lb == lb;
}

static void minor_regression_tests(const buf_t *capture)
{
    buf_t empty = {0};
    buf_add(&empty, NULL, 0);
    CHECK(!empty.p && !empty.len && !empty.cap, "empty append needs no pointers");
    uint8_t packet[2020] = {0xcd, 0xca, 0x10};
    packet[4] = 0x12;
    packet[8] = 2000 >> 8;
    packet[9] = 2000 & 255;
    cpca_t a, b;
    cpca_parse(packet, sizeof(packet), &a);
    packet[2019] = 1;
    cpca_parse(packet, sizeof(packet), &b);
    CHECK(a.other.len == 4007 && b.other.len == 4007 && memcmp(a.other.p, b.other.p, a.other.len),
          "settings comparison retains bytes after the diagnostic prefix");
    free_cpca(&a);
    free_cpca(&b);
    uint8_t end[21] = {0xcd, 0xca, 0x10, 0, 0, 0x1a, 0, 0, 0, 1};
    end[20] = 1;
    for (int kind = 0; kind < 4; kind++) {
        end[3] = kind;
        cpca_parse(end, sizeof(end), &a);
        char want[24];
        snprintf(want, sizeof(want), "0k%d,", kind);
        CHECK((a.err != 0) == (kind != 0 && kind != 2) && !strcmp((char *)a.sizes.p, want),
              "one-byte UFR packet kind %d is preserved and validated", kind);
        free_cpca(&a);
    }
    // Oversized stripe heights must still decode the remaining rows.
    uint8_t bie[22] = {0, 0, 1};
    bie[7] = 8;
    bie[11] = 2;
    memset(bie + 12, 255, 4);
    bie[19] = 0x48;
    bie[20] = 255;
    bie[21] = 2;
    image_t im = {0};
    int stripes;
    CHECK(!jbig_decode(bie, sizeof(bie), &im, &stripes) && im.h == 2, "maximal JBIG stripe height");
    free(im.bits);

    char dir[] = "/tmp/ufr2-io-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "output regression temporary directory");
    char input[256], output[256];
    snprintf(input, sizeof(input), "%s/input.prn", dir);
    FILE *f = fopen(input, "wb");
    CHECK(f != NULL, "output regression capture file");
    if (!f)
        return;
    bool saved = fwrite(capture->p, 1, capture->len, f) == capture->len;
    saved &= fclose(f) == 0;
    CHECK(saved, "output regression save capture");
    for (int command = 0; command < 2; command++) {
        for (int limited = 0; limited < 2; limited++) {
            snprintf(output, sizeof(output), limited ? "%s/output" : "%s/missing/output", dir);
            fflush(NULL);
            pid_t pid = fork();
            if (pid == 0) {
                failures = 0;
                freopen("/dev/null", "w", stdout);
                freopen("/dev/null", "w", stderr);
                if (limited) {
                    signal(SIGXFSZ, SIG_IGN);
                    struct rlimit limit = {1, 1};
                    if (setrlimit(RLIMIT_FSIZE, &limit))
                        _exit(2);
                }
                int r;
                if (command) {
                    setenv("UFR2_OUT", output, 1);
                    r = recode(input);
                } else
                    r = decode_cmd(input, output);
                _exit(r ? 1 : 0);
            }
            int status = 0;
            CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 1,
                  "%s reports %s output failure", command ? "UFR2_OUT" : "PBM", limited ? "write/close" : "open");
        }
    }
    // A pipe is readable but cannot be sized by seeking.
    int pipefd[2];
    if (!pipe(pipefd)) {
        write(pipefd[1], "x", 1);
        close(pipefd[1]);
        snprintf(output, sizeof(output), "/dev/fd/%d", pipefd[0]);
        pid_t pid = fork();
        if (!pid) {
            size_t len = 123;
            uint8_t *data = read_file(output, &len);
            bool rejected = !data && !len;
            free(data);
            _exit(rejected ? 0 : 1);
        }
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
              "nonseekable capture is rejected cleanly");
        close(pipefd[0]);
    } else
        CHECK(false, "nonseekable capture pipe");
    unlink(input);
    snprintf(output, sizeof(output), "%s/output", dir);
    unlink(output);
    snprintf(output, sizeof(output), "%s/output-1.pbm", dir);
    unlink(output);
    rmdir(dir);
}

static void regression_tests(void)
{
    const printer_driver_t *driver = &canon_mf220_driver;
    for (int fail = 0; fail < 2; fail++) {
        flush_test_t t = {.result = fail ? -1 : 0};
        printer_io_t io = {.write = flush_test_write, .sync = flush_test_sync, .ctx = &t};
        void *job = driver->job_begin(driver, &io, 600, 0, PRINTER_SIDES_ONE);
        CHECK(job != NULL, "flush: job allocation");
        if (!job)
            continue;
        int r = driver->job_end(job);
        CHECK((r < 0) == (fail != 0), "final flush failure is returned (%d)", fail);
        CHECK(t.calls == 1 && !t.named_sync && t.flushed == t.bytes.len,
              "final flush follows the complete trailer (%d): calls %d, flushed %zu/%zu", fail, t.calls,
              t.flushed, t.bytes.len);
        cpca_t c;
        cpca_parse(t.bytes.p, t.bytes.len, &c);
        CHECK(!c.err && c.ufr_end, "flush preserves a complete CPCA stream");
        free_cpca(&c);
        free(t.bytes.p);
    }
    cpca_t c;
    cpca_parse(NULL, 0, &c);
    CHECK(c.other.p && c.other.len == 1 && !c.other.p[0], "empty CPCA text is terminated");
    free_cpca(&c);
    uint8_t bad[40] = {0xcd, 0xca, 0x10};
    cpca_parse(bad, sizeof(bad), &c);
    CHECK(c.err && c.other.p && c.other.len && !c.other.p[c.other.len - 1],
          "CPCA text is terminated after an error");
    free_cpca(&c);
    ufr_doc_t doc;
    memset(&doc, 0xa5, sizeof(doc));
    CHECK(decode_stream(bad, sizeof(bad), &doc, &c, false) < 0 && doc.npages == 0,
          "malformed CPCA initializes the document");
    free_cpca(&c);

    // Put each truncated band against inaccessible memory: a parser must
    // reject it without inspecting bytes beyond the supplied length.
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(guard != MAP_FAILED, "truncated band: guard allocation");
    if (guard == MAP_FAILED)
        return;
    int protected = mprotect(guard + page_size, page_size, PROT_NONE);
    CHECK(!protected, "truncated band: guard protection");
    if (protected) {
        munmap(guard, page_size * 2);
        return;
    }
    image_t bad_stride = {.w = 8, .h = 1, .lb = 1, .bits = guard + page_size - 1};
    buf_t out = {0};
    CHECK(encode_bits(0, PRINTER_SIDES_ONE, &bad_stride, 1, &out) < 0, "short row stride is rejected");
    free(out.p);
    out = (buf_t){0};
    bad_stride.w = 9920;
    bad_stride.lb = 1241;
    CHECK(encode_bits(0, PRINTER_SIDES_ONE, &bad_stride, 1, &out) < 0, "long row stride is rejected");
    free(out.p);
    static const uint8_t ys[][5] = {{0xa1, 0, 0}, {0xa5, 0, 0, 0, 0}};
    static const uint8_t lens[][10] = {
        {0xa0, 20, 0x9c, 20}, {0xa4, 0, 20, 0x9d, 0, 20},
        {0xa8, 0, 0, 0, 20, 0x9d, 0, 20}, {0xa8, 0, 0, 0, 20, 0x9e, 0, 0, 0, 20},
    };
    const size_t ylen[] = {3, 5}, nlen[] = {4, 6, 8, 10};
    for (int y = 0; y < 2; y++) {
        for (int n = 0; n < 4; n++) {
            uint8_t u[128] = {0x01};
            u[21] = 0xc3;
            u[23] = 0xc5;
            u[28] = 0xf2;
            u[30] = 0x03;
            u[31] = 0xe7;
            u[32] = 0x85;
            u[52] = 0xe5;
            const uint8_t band[] = {0x62, 0xe3, 0x85, 0, 8, 0, 1, 0xe8};
            size_t k = 54;
            memcpy(u + k, band, sizeof(band));
            k += sizeof(band);
            memcpy(u + k, ys[y], ylen[y]);
            k += ylen[y];
            memcpy(u + k, "\xe1\x05\xd7", 3);
            k += 3;
            memcpy(u + k, lens[n], nlen[n]);
            k += nlen[n];
            for (size_t len = 55; len < k + 20; len++) {
                uint8_t *data = guard + page_size - len;
                memcpy(data, u, len);
                CHECK(ufr_parse(data, len, &doc, false) < 0, "truncated band: y%d length%d prefix %zu", y, n, len);
                free_doc(&doc);
            }
            // A complete band with a BIH but no stripe marker must release
            // both the accumulated BIE and its partially allocated bitmap.
            uint8_t *bih = u + k;
            bih[2] = 1;
            bih[7] = 16;
            bih[11] = 1;
            bih[15] = 1;
            bih[19] = 0x48;
            u[k + 20] = 0x13;
            u[k + 21] = 0x12;
            u[k + 22] = 0x11;
            CHECK(ufr_parse(u, k + 23, &doc, false) < 0 && !doc.npages && !doc.page[0].bits,
                  "malformed JBIG releases its partially decoded page");
            free_doc(&doc);
        }
    }
    munmap(guard, page_size * 2);
    printf("parser regressions: all band layouts reject every truncated prefix\n");
}

static int selftest(void)
{
    regression_tests();
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
    CHECK(!encode_bits(0, PRINTER_SIDES_ONE, pages, 1, &out), "encode bit page 0");
    buf_t out2 = {0};
    CHECK(!encode_bits(1, PRINTER_SIDES_ONE, &pages[1], 1, &out2), "encode bit page 1");
    buf_t out3 = {0};
    CHECK(!encode_bits(0, PRINTER_SIDES_ONE, &pages[2], 1, &out3), "encode bit page 2");
    minor_regression_tests(&out3);
    const buf_t *outs[] = {&out, &out2, &out3};
    for (int t = 0; t < 3; t++) {
        ufr_doc_t doc;
        cpca_t c;
        int r = decode_stream(outs[t]->p, outs[t]->len, &doc, &c, false);
        const image_t *want = &pages[t];
        CHECK(r == 0 && doc.npages == 1, "page %d: decode", t);
        bool shape = r == 0 && page_matches(&doc, want->w, want->h, want->lb);
        CHECK(shape, "page %d: count, dimensions and stride", t);
        if (shape) {
            CHECK(!memcmp(doc.page[0].bits, want->bits, (size_t)want->h * want->lb), "page %d: round trip differs", t);
            CHECK(doc.page_w == (t == 1 ? 5100 : 4958), "page %d: page width %d", t, doc.page_w);
            printf("bit page %d: %zu bytes, %d bands, identical after decoding\n", t, outs[t]->len, doc.nbands[0]);
        }
        free_doc(&doc);
        free_cpca(&c);
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
            CHECK(!encode_bits(0, cases[i].sides, two, 2, &o), "encode duplex");
            ufr_doc_t doc;
            cpca_t c;
            int r = decode_stream(o.p, o.len, &doc, &c, false);
            CHECK(r == 0 && doc.npages == 2 && doc.duplex == cases[i].c5 && strstr((char *)c.other.p, cases[i].df) &&
                      strstr((char *)c.other.p, cases[i].e0),
                  "sides %d: header %d, settings %s %s", cases[i].sides, doc.duplex, cases[i].df, cases[i].e0);
            free_doc(&doc);
            free_cpca(&c);
            free(o.p);
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
    CHECK(!encode_bits(0, PRINTER_SIDES_ONE, &noise, 1, &on), "encode noise");
    {
        ufr_doc_t doc;
        cpca_t c;
        int r = decode_stream(on.p, on.len, &doc, &c, false);
        CHECK(r == 0, "noise page: decode");
        bool shape = r == 0 && page_matches(&doc, noise.w, noise.h, noise.lb);
        CHECK(shape, "noise page: count, dimensions and stride");
        if (shape) {
            CHECK(doc.max_band > 65535, "noise page: largest band %d bytes", doc.max_band);
            CHECK(!memcmp(doc.page[0].bits, noise.bits, (size_t)noise.h * noise.lb), "noise page: round trip differs");
            printf("noise page: %zu bytes, %d bands, largest %d bytes, identical after decoding\n", on.len,
                   doc.nbands[0], doc.max_band);
        }
        free_doc(&doc);
        free_cpca(&c);
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
        bool shape = r == 0 && page_matches(&doc, 9920, 7016, 1240);
        CHECK(shape, "gray page: count, dimensions and stride");
        if (shape) {
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
        free_cpca(&c);
        free(g.p);
        free(line);
    }
    for (int p = 0; p < 3; p++)
        free(pages[p].bits);
    free(noise.bits);
    free(out.p);
    free(out2.p);
    free(out3.p);
    free(on.p);
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
