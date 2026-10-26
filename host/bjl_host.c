// Host-side harness for components/canon_bjl (built with plain cc, no IDF).
//
//   bjl_host testpage <300|600> out.prn     render the firmware's test page job
//   bjl_host encode <300|600> <a4|letter> out.prn page1.raw [page2.raw ...]
//                                           encode raw planes (per line: C,M,Y,K
//                                           line_bytes each), one file per page
//   bjl_host selftest                       PackBits / framing unit tests, and the
//                                           printer drivers' test pages against testpage
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
    if (bjl_job_init(&job, mode, BJL_PAPER_A4, file_crc_sink, &o))
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

static int cmd_encode(bjl_mode_id_t mode, bjl_paper_id_t paper, const char *out_path, int npages,
                      char **pages)
{
    FILE *out = fopen(out_path, "wb");
    if (!out)
        return 1;
    bjl_job_t job;
    if (bjl_job_init(&job, mode, paper, file_sink, out))
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
        CHECK(bjl_job_init(&job, m, BJL_PAPER_A4, mem_sink, &mem) == 0, "init");
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
        bjl_job_init(&job, BJL_MODE_300DPI, BJL_PAPER_A4, fail_sink, &budget);
        bjl_emit_start_job(&job);
        bjl_emit_mode_shift(&job);
        int r = bjl_page_begin(&job);
        CHECK(r == -5, "sink error not propagated (r=%d)", r);
        CHECK(bjl_page_add_line(&job, NULL) == -5, "error not sticky");
        CHECK(job.bytes_out <= 800, "bytes counted after error");
        bjl_job_free(&job);
    }

    // the Canon driver's test page (what the firmware's button prints) is
    // the stream of the test page job above, byte for byte
    {
        const printer_driver_t *d = printer_driver_find("canon-mg2500");
        CHECK(d && d->test_page, "canon-mg2500 driver");
        for (int i = 0; d && i < d->nresolutions; i++) {
            int dpi = d->resolutions[i];
            uint32_t want = 0, got = 0;
            bjl_job_t job;
            testpage_t tp;
            char info1[64];
            bjl_job_init(&job, parse_mode(dpi == 600 ? "600" : "300"), BJL_PAPER_A4, crc_sink, &want);
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
            CHECK(d->test_page(&io, dpi, TESTPAGE_DRYRUN_INFO) == 0, "driver test page %d dpi", dpi);
            CHECK(got == want, "driver test page %d dpi: crc32 %08x, expected %08x", dpi, (unsigned)got,
                  (unsigned)want);
        }
    }

    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "testpage"))
        return cmd_testpage(parse_mode(argv[2]), argv[3]);
    if (argc >= 6 && !strcmp(argv[1], "encode"))
        return cmd_encode(parse_mode(argv[2]), !strcmp(argv[3], "letter") ? BJL_PAPER_LETTER : BJL_PAPER_A4, argv[4],
                          argc - 5, argv + 5);
    if (argc >= 2 && !strcmp(argv[1], "selftest"))
        return cmd_selftest();
    fprintf(stderr, "usage: %s testpage <300|600> out.prn | encode <300|600> <a4|letter> out.prn page.raw... | selftest\n",
            argv[0]);
    return 2;
}
