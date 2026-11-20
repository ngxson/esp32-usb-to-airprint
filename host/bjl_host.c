// Host-side harness for components/canon_bjl (built with plain cc, no IDF).
//
//   bjl_host testpage <300|600> out.prn     render the firmware's test page job
//   bjl_host encode [--model MG2900] <300|600> <a4|letter> out.prn page1.raw [page2.raw ...]
//                                           encode raw planes (per line: C,M,Y,K
//                                           line_bytes each), one file per page,
//                                           for a model of canon_bjl.h (default MG2400)
//   bjl_host selftest                       PackBits / framing unit tests, and the
//                                           printer drivers' test pages against testpage
#define _GNU_SOURCE     // memmem
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "canon_bjl.h"
#include "printer_driver.h"
#include "testpage.h"

static int file_sink(void *user, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)user) == len ? 0 : -1;
}

static bjl_mode_id_t parse_mode(const char *s)
{
    return atoi(s) == 600 ? BJL_MODE_600DPI : BJL_MODE_300DPI;
}

typedef struct {
    FILE *f;
    uint32_t crc;
} out_t;

static int crc_sink(void *user, const uint8_t *data, size_t len)
{
    *(uint32_t *)user = bjl_crc32(*(uint32_t *)user, data, len);
    return 0;
}

static int crc_write(void *ctx, const uint8_t *data, size_t len)
{
    return crc_sink(ctx, data, len);
}

static int file_crc_sink(void *user, const uint8_t *data, size_t len)
{
    out_t *o = user;
    o->crc = bjl_crc32(o->crc, data, len);
    return fwrite(data, 1, len, o->f) == len ? 0 : -1;
}

// Same call sequence and page content as the firmware's dry run, so the CRC
// printed here must match the one the ESP32 logs.
static int cmd_testpage(bjl_mode_id_t mode, const char *out_path)
{
    out_t o = {fopen(out_path, "wb"), 0};
    if (!o.f)
        return 1;
    bjl_job_t job;
    testpage_t tp;
    if (bjl_job_init(&job, BJL_MODEL_MG2400, mode, BJL_PAPER_A4, file_crc_sink, &o))
        return 1;
    char info1[64];
    testpage_format_mode(&job, info1, sizeof(info1));
    if (testpage_init(&tp, &job, info1, TESTPAGE_DRYRUN_INFO))
        return 1;

    clock_t t0 = clock();
    bjl_emit_start_job(&job);
    bjl_emit_mode_shift(&job);
    bjl_page_begin(&job);
    for (int y = 0; y < job.height; y++)
        bjl_page_add_line(&job, testpage_render_line(&tp, y));
    bjl_page_end(&job);
    int err = bjl_emit_end_job(&job);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("testpage %s: %dx%d dots, %u bytes, crc32 %08x, err=%d, %.2fs host cpu\n", job.mode->name,
           job.width, job.height, (unsigned)job.bytes_out, (unsigned)o.crc, err, secs);
    testpage_free(&tp);
    bjl_job_free(&job);
    fclose(o.f);
    return err != 0;
}

static int cmd_encode(bjl_model_id_t model, bjl_mode_id_t mode, bjl_paper_id_t paper, const char *out_path,
                      int npages, char **pages)
{
    FILE *out = fopen(out_path, "wb");
    if (!out)
        return 1;
    bjl_job_t job;
    if (bjl_job_init(&job, model, mode, paper, file_sink, out))
        return 1;
    uint8_t *buf[4];
    for (int c = 0; c < 4; c++)
        buf[c] = malloc(job.line_bytes[c]);
    bjl_emit_start_job(&job);
    bjl_emit_mode_shift(&job);
    for (int pg = 0; pg < npages; pg++) {
        FILE *in = fopen(pages[pg], "rb");
        if (!in)
            return 1;
        bjl_page_begin(&job);
        int lines = 0;
        for (;;) {
            int ok = 1;
            for (int c = 0; c < 4 && ok; c++)
                ok = fread(buf[c], 1, job.line_bytes[c], in) == (size_t)job.line_bytes[c];
            if (!ok)
                break;
            bjl_page_add_line(&job, (const uint8_t *const *)buf);
            lines++;
        }
        bjl_page_end(&job);
        fclose(in);
        printf("page %d: %d lines\n", pg + 1, lines);
    }
    int err = bjl_emit_end_job(&job);
    printf("encoded %d page(s) -> %u bytes, err=%d\n", npages, (unsigned)job.bytes_out, err);
    for (int c = 0; c < 4; c++)
        free(buf[c]);
    bjl_job_free(&job);
    fclose(out);
    return err != 0;
}

// --- self tests ------------------------------------------------------------

static size_t unpack(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        int8_t c = (int8_t)in[i++];
        if (c >= 0) {
            memcpy(out + o, in + i, c + 1);
            o += c + 1;
            i += c + 1;
        } else if (c != -128) {
            memset(out + o, in[i++], 1 - c);
            o += 1 - c;
        }
    }
    return o;
}

static int failures;
static void brother_selftest(void);
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void roundtrip(const uint8_t *data, size_t len, const char *what)
{
    uint8_t *enc = malloc(BJL_PACKBITS_MAX(len)), *dec = malloc(len + 1);
    size_t n = bjl_packbits(data, len, enc);
    CHECK(n <= BJL_PACKBITS_MAX(len), "%s: encoded size %zu exceeds bound", what, n);
    for (size_t i = 0; i < n;) {        // 0x80 must never appear as a control byte
        int8_t c = (int8_t)enc[i];
        CHECK(c != -128, "%s: 0x80 control byte at %zu", what, i);
        i += c >= 0 ? (size_t)c + 2 : 2;
    }
    size_t m = unpack(enc, n, dec);
    CHECK(m == len && memcmp(dec, data, len) == 0, "%s: roundtrip mismatch (len %zu -> %zu)", what, len, m);
    free(enc);
    free(dec);
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} mem_t;

static int mem_sink(void *user, const uint8_t *data, size_t len)
{
    mem_t *m = user;
    if (m->len + len > m->cap) {
        m->cap = (m->len + len) * 2;
        m->buf = realloc(m->buf, m->cap);
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

static int fail_sink(void *user, const uint8_t *data, size_t len)
{
    (void)data;
    int *budget = user;
    *budget -= (int)len;
    return *budget < 0 ? -5 : 0;
}

static int cmd_selftest(void)
{
    uint8_t buf[2000];
    // edge cases
    for (size_t len = 1; len <= 6; len++) {
        memset(buf, 0xAA, len);
        roundtrip(buf, len, "all-same short");
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)i;
        roundtrip(buf, len, "all-different short");
    }
    memset(buf, 0, sizeof(buf));
    roundtrip(buf, 300, "zeros 300");
    roundtrip(buf, 129, "zeros 129");
    roundtrip(buf, 257, "zeros 257");
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    roundtrip(buf, 1198, "literal 1198");
    roundtrip(buf, 128, "literal 128");
    roundtrip(buf, 129, "literal 129");
    // known encoding: Gutenprint leaves a 2-byte tail as repeat runs
    const uint8_t t1[] = {1, 2, 3, 4};
    uint8_t e[16];
    size_t n = bjl_packbits(t1, 4, e);
    const uint8_t x1[] = {0x01, 1, 2, 0x00, 3, 0x00, 4};
    CHECK(n == sizeof(x1) && !memcmp(e, x1, n), "tail encoding differs from stp_pack_tiff");
    const uint8_t t2[] = {5, 5, 5, 5, 9};
    n = bjl_packbits(t2, 5, e);
    const uint8_t x2[] = {0xFD, 5, 0x00, 9};
    CHECK(n == sizeof(x2) && !memcmp(e, x2, n), "run encoding differs");
    // random fuzz with run structure
    srand(1234);
    for (int it = 0; it < 20000; it++) {
        size_t len = 1 + rand() % 1500;
        for (size_t i = 0; i < len;) {
            size_t run = 1 + rand() % (rand() % 4 == 0 ? 300 : 3);
            uint8_t v = (uint8_t)(rand() % 4 == 0 ? 0 : rand());
            for (size_t k = 0; k < run && i < len; k++)
                buf[i++] = v;
        }
        roundtrip(buf, len, "fuzz");
    }

    // CRC-32 check value
    CHECK(bjl_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(bjl_crc32(bjl_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xCBF43926u,
          "crc32 incremental");

    // framing: block count and blank padding for both modes
    for (int m = 0; m < BJL_MODE_COUNT; m++) {
        mem_t mem = {0};
        bjl_job_t job;
        CHECK(bjl_job_init(&job, BJL_MODEL_MG2400, m, BJL_PAPER_A4, mem_sink, &mem) == 0, "init");
        bjl_page_begin(&job);
        for (int y = 0; y < 13; y++)            // partial: 1 full block + 5 lines
            bjl_page_add_line(&job, NULL);
        CHECK(bjl_page_end(&job) == 0, "page end");
        int blocks = 0;
        for (size_t i = 0; i + 2 < mem.len; i++)
            if (mem.buf[i] == 0x1b && mem.buf[i + 1] == '(' && mem.buf[i + 2] == 'F')
                blocks++;
        int expect = (job.height + 7) / 8 * 4;
        CHECK(blocks == expect, "mode %d: %d F blocks, expected %d", m, blocks, expect);
        CHECK(mem.buf[mem.len - 7] == 0x0c, "form feed before ESC (b 0");
        CHECK(bjl_page_add_line(&job, NULL) != 0, "add_line after page end must fail");
        bjl_job_free(&job);
        free(mem.buf);
    }

    // sink errors must stop the stream and stay sticky
    {
        int budget = 800;     // XML wrapper is 680 bytes, page header ~190
        bjl_job_t job;
        bjl_job_init(&job, BJL_MODEL_MG2400, BJL_MODE_300DPI, BJL_PAPER_A4, fail_sink, &budget);
        bjl_emit_start_job(&job);
        bjl_emit_mode_shift(&job);
        int r = bjl_page_begin(&job);
        CHECK(r == -5, "sink error not propagated (r=%d)", r);
        CHECK(bjl_page_add_line(&job, NULL) == -5, "error not sticky");
        CHECK(job.bytes_out <= 800, "bytes counted after error");
        bjl_job_free(&job);
    }

    // each Canon driver's test page (what the firmware's button prints) is
    // the stream of the test page job above for its model, byte for byte
    CHECK(printer_driver_find("canon-mg2500"), "canon-mg2500 driver");
    for (int k = 0; printer_drivers[k]; k++) {
        const printer_driver_t *d = printer_drivers[k];
        if (strcmp(d->usb_mfg, "Canon"))
            continue;
        CHECK(d->test_page && d->model, "%s: test page", d->id);
        for (int i = 0; d->test_page && d->model && i < d->nresolutions; i++) {
            int dpi = d->resolutions[i];
            uint32_t want = 0, got = 0;
            bjl_job_t job;
            testpage_t tp;
            char info1[64];
            bjl_job_init(&job, *(const bjl_model_id_t *)d->model, parse_mode(dpi == 600 ? "600" : "300"),
                         BJL_PAPER_A4, crc_sink, &want);
            testpage_format_mode(&job, info1, sizeof(info1));
            testpage_init(&tp, &job, info1, TESTPAGE_DRYRUN_INFO);
            bjl_emit_start_job(&job);
            bjl_emit_mode_shift(&job);
            bjl_page_begin(&job);
            for (int y = 0; y < job.height; y++)
                bjl_page_add_line(&job, testpage_render_line(&tp, y));
            bjl_page_end(&job);
            bjl_emit_end_job(&job);
            testpage_free(&tp);
            bjl_job_free(&job);
            printer_io_t io = {.write = crc_write, .ctx = &got};
            CHECK(d->test_page(d, &io, dpi, TESTPAGE_DRYRUN_INFO) == 0, "%s: test page %d dpi", d->id, dpi);
            CHECK(got == want, "%s: test page %d dpi: crc32 %08x, expected %08x", d->id, dpi, (unsigned)got,
                  (unsigned)want);
        }
    }

    // the firmware's choice of driver for the attached printer
    {
        static const struct {
            uint16_t vid, pid;
            const char *device_id, *want;
        } cases[] = {
            {0x04a9, 0x176d, "MFG:Canon;CMD:BJL,BJRaster3,BSCCe,IVEC,IVECPLI;MDL:MG2500 series;CLS:PRINTER;",
             "canon-mg2500"},
            {0x04a9, 0x1746, "", "canon-mp280"},                              // by USB id
            {0x04a9, 0x1234, "MFG:Canon;MDL:iP2700 series;", "canon-ip2700"},  // by model name
            {0x04a9, 0x1234, "MANUFACTURER:Canon;MODEL:iP2800 series;", "canon-ip2800"},
            {0x04a9, 0x1234, "MFG:Canon;MDL:P200;", "canon-p200"},            // " series" optional
            {0x04a9, 0x1234, "MFG:Canon; MDL:mg2900 Series;", "canon-mg2900"},
            {0x04a9, 0x1234, "MFG:Canon;MDL:MG3600 series;", NULL},           // not supported
            {0x03f0, 0x1234, "MFG:HP;MDL:iP2700 series;", NULL},              // another maker
            {0x04a9, 0x1234, "MFG:Canon;MDL:iP2700 series XL;", NULL},
            {0x04a9, 0x1234, "", NULL},
            {0x04f9, 0x1234, "MFG:Brother;CMD:PJL,HBP;MDL:HL-L2300D series;CLS:PRINTER;CID:Brother Laser Type1;",
             "brother-hl-l2300d"},
            {0x04f9, 0x1234, "MFG:Canon;MDL:HL-L2300D series;", NULL},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const printer_driver_t *d = printer_driver_match(cases[i].vid, cases[i].pid, cases[i].device_id);
            const char *got = d ? d->id : NULL;
            CHECK(cases[i].want ? got && !strcmp(got, cases[i].want) : !got, "match %04x:%04x \"%s\": %s, expected %s",
                  cases[i].vid, cases[i].pid, cases[i].device_id, got ? got : "none",
                  cases[i].want ? cases[i].want : "none");
        }
        // every driver is found by its own USB id and by its own model name
        for (int k = 0; printer_drivers[k]; k++) {
            const printer_driver_t *d = printer_drivers[k];
            char id[96];
            snprintf(id, sizeof(id), "MFG:%s;MDL:%s;", d->usb_mfg, d->usb_mdl);
            CHECK(printer_driver_match(0, 0, id) == d, "%s: match by \"%s\"", d->id, id);
            if (d->usb_vid)
                CHECK(printer_driver_match(d->usb_vid, d->usb_pid, "") == d, "%s: match by USB id", d->id);
        }
    }

    brother_selftest();

    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures != 0;
}

// ---------------------------------------------------------------------------
// Brother HBP (brother_hbp.c): its line compression decoded back, and the
// test page stream parsed line by line

int brother_hbp_encode_line(const uint8_t *line, const uint8_t *ref, int len, uint8_t *o);

static int br_overflow(const uint8_t **p, const uint8_t *end)
{
    int v = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        v += b;
        if (b != 255)
            break;
    }
    return v;
}

// one encoded line onto line (which holds the reference); bytes used, -1 bad
static int br_decode_line(const uint8_t *in, const uint8_t *end, uint8_t *line, int len)
{
    const uint8_t *p = in;
    if (p >= end)
        return -1;
    int edits = *p++;
    if (edits == 0xff) {
        memset(line, 0, len);
        return 1;
    }
    int pos = 0;
    for (int e = 0; e < edits; e++) {
        if (p >= end)
            return -1;
        uint8_t c = *p++;
        int off, cnt;
        if (c & 0x80) {
            off = (c >> 5) & 3;
            if (off == 3)
                off += br_overflow(&p, end);
            cnt = c & 31;
            if (cnt == 31)
                cnt += br_overflow(&p, end);
            cnt += 2;
            pos += off;
            if (p >= end || pos + cnt > len)
                return -1;
            memset(line + pos, *p++, cnt);
        } else {
            off = (c >> 3) & 15;
            if (off == 15)
                off += br_overflow(&p, end);
            cnt = c & 7;
            if (cnt == 7)
                cnt += br_overflow(&p, end);
            cnt += 1;
            pos += off;
            if (p + cnt > end || pos + cnt > len)
                return -1;
            memcpy(line + pos, p, cnt);
            p += cnt;
        }
        pos += cnt;
    }
    return (int)(p - in);
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} br_capture_t;

static int br_capture(void *ctx, const uint8_t *data, size_t len)
{
    br_capture_t *c = ctx;
    if (c->len + len > c->cap) {
        c->cap = (c->len + len) * 2;
        c->buf = realloc(c->buf, c->cap);
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return 0;
}

static void brother_selftest(void)
{
    const printer_driver_t *d = printer_driver_find("brother-hl-l2300d");
    CHECK(d, "brother-hl-l2300d driver");
    if (!d)
        return;

    // random lines with runs, against random references, round trip
    enum { LEN = 638 };
    static uint8_t line[LEN], ref[LEN], dec[LEN], enc[LEN + 64];
    uint32_t rnd = 12345;
#define RND() (rnd = rnd * 1103515245u + 12345u, (rnd >> 16) & 0x7fff)
    for (int t = 0; t < 20000; t++) {
        int kind = t % 5;
        for (int i = 0; i < LEN; i++) {
            ref[i] = (kind == 0) ? 0 : (RND() % 4 == 0 ? RND() & 0xff : (i ? ref[i - 1] : 0));
            line[i] = ref[i];
        }
        int nchanges = RND() % (kind == 4 ? 400 : 12);
        for (int c = 0; c < nchanges; c++) {
            int at = RND() % LEN, n = 1 + RND() % (kind == 3 ? 600 : 40);
            uint8_t v = RND() & 0xff;
            bool run = RND() & 1;
            for (int i = at; i < at + n && i < LEN; i++)
                line[i] = run ? v : (uint8_t)(RND() & 0xff);
        }
        if (t % 97 == 0)
            memset(line, 0, LEN);
        bool noref = t % 7 == 0;
        int n = brother_hbp_encode_line(line, noref ? NULL : ref, LEN, enc);
        CHECK(n > 0 && n <= LEN + 32, "brother: encoded length %d", n);
        memcpy(dec, noref ? (uint8_t[LEN]){0} : ref, LEN);
        int used = br_decode_line(enc, enc + n, dec, LEN);
        if (used != n || memcmp(dec, line, LEN)) {
            CHECK(0, "brother: line %d round trip (used %d of %d)", t, used, n);
            break;
        }
    }

    // the test page: PJL header, one raster group of the page's lines in
    // blocks of at most 64 lines, the end of the job
    br_capture_t cap = {0};
    printer_io_t io = {.write = br_capture, .ctx = &cap};
    CHECK(d->test_page(d, &io, 600, "DRY RUN") == 0, "brother: test page");
    const char *s = (const char *)cap.buf;
    // Letter, less the printer's origin (5 mm from the left and the top)
    int width = 21590 * 600 / 2540 - 118, height = 27940 * 600 / 2540 - 118, lb = (width + 7) / 8;
    const char *g = memmem(cap.buf, cap.len, "\033*b1030m", 8);
    CHECK(cap.len > 200 && !memcmp(s + 128, "\033%-12345X@PJL\n", 14), "brother: job header");
    CHECK(memmem(cap.buf, cap.len, "@PJL SET PAPER = LETTER\n", 24) && memmem(cap.buf, cap.len, "@PJL SET RESOLUTION = 600\n", 26),
          "brother: page settings");
    CHECK(g, "brother: raster group");
    if (g) {
        const uint8_t *p = (const uint8_t *)g + 8, *end = cap.buf + cap.len;
        uint8_t *cur = calloc(1, lb);
        int lines = 0, black = 0;
        bool ok = true;
        while (ok && p < end && *p >= '0' && *p <= '9') {
            const uint8_t *num = p;
            int n = 0;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + (*p++ - '0');
            if (*p == 'M') {        // "1030M": the end of the group
                p = num;
                break;
            }
            if (*p++ != 'w' || p + n > end || p[0] != 0) {
                ok = false;
                break;
            }
            int nl = p[1];
            const uint8_t *q = p + 2, *bend = p + n;
            ok = nl >= 1 && nl <= 64;
            for (int i = 0; ok && i < nl; i++) {
                int used = br_decode_line(q, bend, cur, lb);
                ok = used > 0;
                q += used > 0 ? used : 0;
                for (int k = 0; ok && k < lb; k++)
                    black += __builtin_popcount(cur[k]);
                lines++;
            }
            ok = ok && q == bend;
            p = bend;
        }
        CHECK(ok, "brother: blocks decode (%d lines)", lines);
        CHECK(lines == height, "brother: %d lines, expected %d", lines, height);
        CHECK(black > width * height / 50 && black < width * height / 3, "brother: %d black dots", black);
        CHECK(end - p >= 6 && !memcmp(p, "1030M\f", 6), "brother: end of page");
        CHECK(memmem(p, end - p, "@PJL EOJ", 8), "brother: end of job");
        free(cur);
    }
    free(cap.buf);

    // the printer's origin: a dot at (300, 500) on the sheet (600 dpi) is
    // printer pixel (300 - 118, 500 - 118); at 300 dpi a 2x2 dot there
    for (int pass = 0; pass < 2; pass++) {
        int dpi = pass ? 300 : 600, sc = pass ? 2 : 1, sw = 5100 / sc, sh = 6600 / sc;
        br_capture_t c = {0};
        printer_io_t pio = {.write = br_capture, .ctx = &c};
        uint8_t *row = malloc(sw);
        void *job = d->job_begin(d, &pio, dpi, 0);
        d->page_begin(job, sw, sh, 1);
        for (int y = 0; y < sh; y++) {
            memset(row, 255, sw);
            if (y == 500 / sc)
                row[300 / sc] = 0;
            d->page_line(job, y, row);
        }
        d->page_end(job);
        d->job_end(job);
        free(row);
        const uint8_t *g2 = memmem(c.buf, c.len, "\033*b1030m", 8), *p = g2 + 8, *end = c.buf + c.len;
        int lb2 = (5100 - 118 + 7) / 8, line = 0, dots = 0, dx = -1, dy = -1;
        uint8_t *cur = calloc(1, lb2);
        while (p < end && *p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + (*p++ - '0');
            if (*p++ != 'w')
                break;
            const uint8_t *q = p + 2;
            for (int i = 0; i < p[1]; i++, line++) {
                q += br_decode_line(q, p + n, cur, lb2);
                for (int x = 0; x < lb2 * 8; x++)
                    if (cur[x >> 3] & (0x80 >> (x & 7))) {
                        if (!dots++)
                            dx = x, dy = line;
                    }
            }
            p += n;
        }
        free(cur);
        free(c.buf);
        CHECK(dx == 300 - 118 && dy == 500 - 118 && dots == sc * sc, "brother: origin at %d dpi: first dot (%d, %d) of %d, expected (182, 382) of %d",
              dpi, dx, dy, dots, sc * sc);
    }

    // a 300 dpi job is the same page doubled at 600 dpi (RGB, a ragged
    // right edge, NULL lines)
    {
        br_capture_t a = {0}, b = {0};
        enum { SW = 2400, SH = 3508 };
        uint8_t *row = malloc(SW * 3), *row2 = malloc(SW * 6);
        for (int pass = 0; pass < 2; pass++) {
            printer_io_t pio = {.write = br_capture, .ctx = pass ? &b : &a};
            int dpi = pass ? 600 : 300, sc = pass ? 2 : 1;
            void *job = d->job_begin(d, &pio, dpi, 0);
            d->page_begin(job, SW * sc, SH * sc, 3);
            for (int y = 0; y < SH; y++) {
                bool blank = (y / 100) % 5 == 4;
                for (int x = 0; x < SW; x++)
                    for (int c = 0; c < 3; c++)
                        row[3 * x + c] = (uint8_t)((x * 7 + y * 3 + c * 50) % 256);
                for (int x = 0; x < SW * 2; x++)
                    memcpy(row2 + 3 * x, row + 3 * (x / 2), 3);
                for (int k = 0; k < sc; k++)
                    d->page_line(job, y * sc + k, blank ? NULL : (pass ? row2 : row));
            }
            d->page_end(job);
            d->job_end(job);
        }
        CHECK(a.len > 1000 && a.len == b.len && !memcmp(a.buf, b.buf, a.len), "brother: 300 dpi = doubled 600 dpi (%zu, %zu bytes)",
              a.len, b.len);
        free(row);
        free(row2);
        free(a.buf);
        free(b.buf);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "testpage"))
        return cmd_testpage(parse_mode(argv[2]), argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "encode")) {
        bjl_model_id_t model = BJL_MODEL_MG2400;
        if (argc >= 4 && !strcmp(argv[2], "--model")) {
            model = bjl_find_model(argv[3]);
            if (model == BJL_MODEL_COUNT) {
                fprintf(stderr, "unknown model %s\n", argv[3]);
                return 2;
            }
            argc -= 2;
            argv += 2;
        }
        if (argc >= 6)
            return cmd_encode(model, parse_mode(argv[2]), !strcmp(argv[3], "letter") ? BJL_PAPER_LETTER : BJL_PAPER_A4,
                              argv[4], argc - 5, argv + 5);
    }
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return cmd_selftest();
    fprintf(stderr,
            "usage: %s testpage <300|600> out.prn | encode [--model MG2400] <300|600> <a4|letter> out.prn page.raw... "
            "| selftest\n",
            argv[0]);
    return 2;
}
