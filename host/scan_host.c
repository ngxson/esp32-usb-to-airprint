// Host tests of the scanner code: the pixma protocol against the emulated
// scanner, and the JPEG encoder (decoded and compared by tools/check_jpeg.py).
//
//   ./scan_host selftest
//   ./scan_host jpeg <dpi> <w> <h> <quality> out.jpg out.ppm   test page area, encoder only
//   ./scan_host scan <dpi> <a4|letter> out.jpg out.ppm         full pipeline: protocol + encoder
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_scanner.h"
#include "jpeg_enc.h"
#include "pixma_scan.h"

static int s_fail;

#define CHECK(cond, ...)                                                                                 \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
            printf(__VA_ARGS__);                                                                         \
            printf("\n");                                                                                \
            s_fail++;                                                                                    \
        }                                                                                                \
    } while (0)

// --- line sink that checks the pixels against the test page

typedef struct {
    int dpi, x, y, w;
    int lines, mismatches;
    int stop_after;             // > 0: ask to stop after this many lines
    FILE *ppm;
    jpeg_enc_t *jpeg;
} sink_t;

static int check_line(void *user, int y, const uint8_t *rgb)
{
    sink_t *s = user;
    if (y != s->lines)
        s->mismatches++;
    for (int i = 0; i < s->w; i++) {
        uint8_t ref[3];
        fake_scanner_pixel(s->dpi, s->x + i, s->y + y, ref);
        if (memcmp(ref, rgb + 3 * i, 3) != 0) {
            s->mismatches++;
            break;
        }
    }
    s->lines++;
    if (s->ppm)
        fwrite(rgb, 3, (size_t)s->w, s->ppm);
    if (s->jpeg && jpeg_enc_add_line(s->jpeg, rgb) < 0)
        return 1;
    return s->stop_after && s->lines >= s->stop_after;
}

static int run(fake_scanner_t *f, pixma_params_t p, sink_t *s, volatile bool *cancel)
{
    s->dpi = p.dpi;
    s->x = p.x;
    s->y = p.y;
    s->w = p.w;
    s->lines = s->mismatches = 0;
    pixma_io_t io = fake_scanner_io(f);
    return pixma_scan(&io, &p, check_line, s, cancel);
}

static void expect_closed(fake_scanner_t *f, const char *what)
{
    CHECK(!f->job_open && !f->session_open, "%s: scanner left with job %d session %d", what, f->job_open,
          f->session_open);
    CHECK(f->errors == 0, "%s: %d protocol violations, first: %s", what, f->errors, f->error);
}

// --- JPEG output to a file

static int file_write(void *user, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, user) == len ? 0 : -1;
}

typedef struct {
    uint8_t *data;
    size_t len, cap;
} mem_t;

static int mem_write(void *user, const uint8_t *data, size_t len)
{
    mem_t *m = user;
    if (m->len + len > m->cap) {
        m->cap = (m->len + len) * 2;
        m->data = realloc(m->data, m->cap);
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return 0;
}

// ---------------------------------------------------------------------------

static void test_protocol(void)
{
    fake_scanner_t f;
    fake_scanner_init(&f, 0);
    sink_t s = {0};

    // whole A4 width at low resolutions, an inner area at 600 dpi
    const pixma_params_t areas[] = {
        {.dpi = 75, .w = 620, .h = 877, .calibrate = true},
        {.dpi = 150, .w = 1240, .h = 1754, .calibrate = true},
        {.dpi = 300, .w = 2480, .h = 3508, .calibrate = true},
        {.dpi = 600, .x = 1000, .y = 2000, .w = 700, .h = 300, .calibrate = true},
        {.dpi = 300, .x = 45, .y = 30, .w = 333, .h = 77},       // x not a multiple of 32
        {.dpi = 150, .x = 1270, .w = 6, .h = 10},                 // right edge, 6 px wide
    };
    int scans = 0;
    for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
        pixma_params_t p = areas[i];
        int r = run(&f, p, &s, NULL);
        CHECK(r == PIXMA_OK, "scan %zu: %s", i, pixma_strerror(r));
        CHECK(s.lines == p.h, "scan %zu: %d of %d lines", i, s.lines, p.h);
        CHECK(s.mismatches == 0, "scan %zu: %d lines differ from the test page", i, s.mismatches);
        CHECK(f.x == p.x - p.x % 32 && f.wx % 32 == 0 && f.wx >= p.w + p.x % 32, "scan %zu: x %d wx %d", i, f.x, f.wx);
        expect_closed(&f, "scan");
        CHECK(f.scans == ++scans, "scan %zu: scanner counted %d complete scans", i, f.scans);
        printf("scan %d dpi %dx%d at (%d,%d): ok\n", p.dpi, p.w, p.h, p.x, p.y);
    }

    pixma_params_t small = {.dpi = 150, .x = 100, .y = 100, .w = 500, .h = 300};

    f.busy_starts = 2;      // start session answered "busy" twice
    int r = run(&f, small, &s, NULL);
    CHECK(r == PIXMA_OK && s.lines == small.h, "busy retry: %s, %d lines", pixma_strerror(r), s.lines);
    expect_closed(&f, "busy retry");
    printf("busy scanner, retried: ok\n");

    s.stop_after = 50;      // the consumer stops the scan
    r = run(&f, small, &s, NULL);
    s.stop_after = 0;
    CHECK(r == PIXMA_ECANCELED && s.lines == 50, "stop: %s after %d lines", pixma_strerror(r), s.lines);
    expect_closed(&f, "stop");
    printf("stopped after 50 lines: session closed\n");

    volatile bool cancel = true;    // canceled before it started
    r = run(&f, small, &s, &cancel);
    CHECK(r == PIXMA_ECANCELED && s.lines == 0, "cancel: %s after %d lines", pixma_strerror(r), s.lines);
    expect_closed(&f, "cancel");
    printf("canceled while warming up: session closed\n");

    f.fail_at_block = 4;    // the scanner fails a block
    r = run(&f, small, &s, NULL);
    f.fail_at_block = 0;
    CHECK(r == PIXMA_ECANCELED, "failed block: %s", pixma_strerror(r));
    expect_closed(&f, "failed block");
    printf("scanner error mid-image: %s after %d lines, session closed\n", pixma_strerror(r), s.lines);

    r = run(&f, small, &s, NULL);   // and the next scan works
    CHECK(r == PIXMA_OK && s.lines == small.h && s.mismatches == 0, "after errors: %s", pixma_strerror(r));
    expect_closed(&f, "after errors");

    pixma_params_t bad[] = {
        {.dpi = 200, .w = 10, .h = 10},
        {.dpi = 150, .x = 1270, .w = 20, .h = 10},     // beyond the right edge (1276 px)
        {.dpi = 150, .w = 0, .h = 10},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(run(&f, bad[i], &s, NULL) == PIXMA_EINVAL, "bad parameters %zu accepted", i);
    CHECK(f.errors == 0, "protocol violations: %s", f.error);
    fake_scanner_free(&f);
}

static void test_jpeg(void)
{
    // odd sizes, padding, early finish: must produce SOI ... EOI
    const int sizes[][2] = {{1, 1}, {7, 5}, {16, 16}, {17, 33}, {100, 1}, {1, 100}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int w = sizes[i][0], h = sizes[i][1];
        mem_t m = {0};
        jpeg_enc_t *e = jpeg_enc_create(w, h, 85, 300, mem_write, &m);
        CHECK(e, "jpeg %dx%d: create failed", w, h);
        if (!e)
            continue;
        uint8_t *line = malloc((size_t)w * 3);
        for (int y = 0; y < h / 2; y++) {   // half the lines, the rest is padding
            for (int x = 0; x < w; x++)
                fake_scanner_pixel(75, x * 7, y * 9, line + 3 * x);
            jpeg_enc_add_line(e, line);
        }
        CHECK(jpeg_enc_finish(e) == 0 && jpeg_enc_lines(e) == h, "jpeg %dx%d: finish", w, h);
        CHECK(m.len > 4 && m.data[0] == 0xff && m.data[1] == 0xd8 && m.data[m.len - 2] == 0xff &&
                  m.data[m.len - 1] == 0xd9 && jpeg_enc_bytes(e) == m.len,
              "jpeg %dx%d: not a complete JPEG", w, h);
        jpeg_enc_free(e);
        free(line);
        free(m.data);
    }
    CHECK(!jpeg_enc_create(0, 10, 85, 0, mem_write, NULL), "jpeg: zero width accepted");
    printf("jpeg odd sizes: ok\n");
}

static FILE *open_ppm(const char *path, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (f)
        fprintf(f, "P6\n%d %d\n255\n", w, h);
    return f;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        test_protocol();
        test_jpeg();
        printf(s_fail ? "%d FAILED\n" : "scanner selftest: all passed\n", s_fail);
        return s_fail != 0;
    }
    if (argc == 8 && !strcmp(argv[1], "jpeg")) {
        int dpi = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]), q = atoi(argv[5]);
        FILE *out = fopen(argv[6], "wb"), *ppm = open_ppm(argv[7], w, h);
        jpeg_enc_t *e = out && ppm ? jpeg_enc_create(w, h, q, dpi, file_write, out) : NULL;
        if (!e)
            return 1;
        uint8_t *line = malloc((size_t)w * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++)
                fake_scanner_pixel(dpi, x, y, line + 3 * x);
            fwrite(line, 3, (size_t)w, ppm);
            jpeg_enc_add_line(e, line);
        }
        int r = jpeg_enc_finish(e);
        printf("jpeg %dx%d q%d: %llu bytes\n", w, h, q, (unsigned long long)jpeg_enc_bytes(e));
        jpeg_enc_free(e);
        free(line);
        fclose(out);
        fclose(ppm);
        return r != 0;
    }
    if (argc == 6 && !strcmp(argv[1], "scan")) {
        int dpi = atoi(argv[2]);
        bool letter = !strcmp(argv[3], "letter");
        int w = (letter ? 5100 : 4961) * dpi / 600, h = (letter ? 6600 : 7016) * dpi / 600;
        FILE *out = fopen(argv[4], "wb");
        sink_t s = {.ppm = open_ppm(argv[5], w, h)};
        s.jpeg = out && s.ppm ? jpeg_enc_create(w, h, 85, dpi, file_write, out) : NULL;
        if (!s.jpeg)
            return 1;
        fake_scanner_t f;
        fake_scanner_init(&f, 0);
        pixma_params_t p = {.dpi = dpi, .w = w, .h = h, .calibrate = true};
        int r = run(&f, p, &s, NULL);
        int fr = jpeg_enc_finish(s.jpeg);
        printf("scan %d dpi %s %dx%d: %s, %d lines, %llu bytes of JPEG, %d protocol violations\n", dpi, argv[3], w, h,
               pixma_strerror(r), s.lines, (unsigned long long)jpeg_enc_bytes(s.jpeg), f.errors);
        jpeg_enc_free(s.jpeg);
        fake_scanner_free(&f);
        fclose(out);
        fclose(s.ppm);
        return r != 0 || fr != 0 || f.errors || s.mismatches;
    }
    fprintf(stderr, "usage: %s selftest | jpeg <dpi> <w> <h> <q> out.jpg out.ppm | scan <dpi> <a4|letter> out.jpg out.ppm\n",
            argv[0]);
    return 2;
}
