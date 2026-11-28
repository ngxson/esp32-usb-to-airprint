// Host tests of the scanner code: the pixma protocol against the emulated
// scanner, and the JPEG encoder (decoded and compared by tools/check_jpeg.py).
//
//   ./scan_host selftest
//   ./scan_host pixma | iclass [format|retry|sequence|lines|abort|pad|transport|cleanup|guards]   one protocol's tests
//   ./scan_host jpeg <dpi> <w> <h> <quality> out.jpg out.ppm   test page area, encoder only
//   ./scan_host scan <dpi> <a4|letter> out.jpg out.ppm         full pipeline: protocol + encoder
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_iclass.h"
#include "fake_scanner.h"
#include "iclass_scan.h"
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
    int white_from;             // > 0: pixels from this x of the scan area on are white (beyond the glass)
    volatile bool *cancel_at_stop; // raise cancel at stop_after instead of returning stop
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
        if (s->white_from && s->x + i >= s->white_from)
            ref[0] = ref[1] = ref[2] = 0xff;
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
    if (s->cancel_at_stop && s->stop_after && s->lines >= s->stop_after) {
        *s->cancel_at_stop = true;
        return 0;
    }
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
        CHECK(f.ready_polls == (p.calibrate ? 8 : 1), "scan %zu: calibration flag not seen", i);
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

// ---------------------------------------------------------------------------
// imageCLASS (MF220 Series) protocol against fake_iclass

static int iclass_run(fake_iclass_t *f, pixma_params_t p, sink_t *s, volatile bool *cancel)
{
    s->dpi = p.dpi;
    s->x = p.x;
    s->y = p.y;
    s->w = p.w;
    s->lines = s->mismatches = 0;
    pixma_io_t io = fake_iclass_io(f);
    return iclass_scan(&io, &p, check_line, s, cancel);
}

static void iclass_expect_clean(fake_iclass_t *f, const char *what)
{
    CHECK(!f->session_open, "%s: session left open", what);
    CHECK(f->errors == 0, "%s: %d protocol violations, first: %s", what, f->errors, f->error);
}

// one case, or all with NULL
static bool test_iclass(const char *only)
{
    const char *selectors[] = {"format", "retry", "sequence", "lines", "abort", "pad", "transport", "cleanup", "guards"};
    bool known = !only;
    for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); i++)
        if (only && !strcmp(only, selectors[i]))
            known = true;
    if (!known) {
        fprintf(stderr, "Usage: scan_host iclass [format|retry|sequence|lines|abort|pad|transport|cleanup|guards]\n");
        return false;
    }
    bool all = !only;
    if (all || !strcmp(only, "format")) {
        // header, lengths and checksums are checked by the emulator on a whole scan;
        // a status reply with a bad checksum is refused
        fake_iclass_t f;
        fake_iclass_init(&f);
        sink_t s = {0};
        int r = iclass_run(&f, (pixma_params_t){.dpi = 150, .x = 10, .y = 20, .w = 333, .h = 77}, &s, NULL);
        CHECK(r == PIXMA_OK, "format: scan: %s", pixma_strerror(r));
        iclass_expect_clean(&f, "format");
        fake_iclass_init(&f);
        f.corrupt_status = 1;
        r = iclass_run(&f, (pixma_params_t){.dpi = 150, .w = 100, .h = 10}, &s, NULL);
        CHECK(r == PIXMA_EPROTO, "format: bad status checksum accepted: %s", pixma_strerror(r));
        CHECK(f.errors == 0, "format: %s", f.error);
        // a reply other than 0x0606 is an error; so is a bare "ok" where data was due
        fake_iclass_init(&f);
        f.fail_command = 0xdb20;
        r = iclass_run(&f, (pixma_params_t){.dpi = 150, .w = 100, .h = 10}, &s, NULL);
        CHECK(r == PIXMA_ECANCELED && s.lines == 0 && !f.aborts,
              "format: rejected start session: %s, aborts %d", pixma_strerror(r), f.aborts);
        CHECK(f.errors == 0, "format: %s", f.error);
        fake_iclass_init(&f);
        f.cut_command = 0xf320;
        r = iclass_run(&f, (pixma_params_t){.dpi = 150, .w = 100, .h = 10}, &s, NULL);
        CHECK(r == PIXMA_ECANCELED && s.lines == 0, "format: status cut short accepted: %s", pixma_strerror(r));
    }
    if (all || !strcmp(only, "retry")) {
        // the first status gets no reply: asked again (SANE's step1), then the scan goes on
        fake_iclass_t f;
        fake_iclass_init(&f);
        f.mute_status = 1;
        sink_t s = {0};
        int r = iclass_run(&f, (pixma_params_t){.dpi = 75, .w = 200, .h = 50}, &s, NULL);
        CHECK(r == PIXMA_OK && s.lines == 50 && s.mismatches == 0, "retry: %s, %d lines", pixma_strerror(r), s.lines);
        CHECK(!strncmp(f.order, "sSBRP0", 6), "retry: commands %s", f.order);
        iclass_expect_clean(&f, "retry");
    }
    if (all || !strcmp(only, "sequence")) {
        // status, begin, source, parameters, first request, image requests,
        // abort, status x 2, abort again (flatbed: no paper in the ADF)
        fake_iclass_t f;
        fake_iclass_init(&f);
        sink_t s = {0};
        int r = iclass_run(&f, (pixma_params_t){.dpi = 300, .x = 64, .y = 0, .w = 1000, .h = 400}, &s, NULL);
        CHECK(r == PIXMA_OK, "sequence: %s", pixma_strerror(r));
        size_t n = strlen(f.order);
        bool ok = n > 9 && !strncmp(f.order, "SBRP0I", 6) && !strcmp(f.order + n - 4, "ASSA");
        for (size_t i = 6; ok && i < n - 4; i++)
            ok = f.order[i] == 'I';
        CHECK(ok, "sequence: commands %s", f.order);
        CHECK(f.dpi == 300 && f.x == 64 && f.y == 0 && f.wx == 1024 && f.h == 400,
              "sequence: parameters %d dpi (%d,%d) %dx%d", f.dpi, f.x, f.y, f.wx, f.h);
        iclass_expect_clean(&f, "sequence");
    }
    if (all || !strcmp(only, "lines")) {
        const pixma_params_t areas[] = {
            {.dpi = 75, .w = 620, .h = 877},
            {.dpi = 150, .x = 3, .y = 5, .w = 1239, .h = 1749},  // down to the bottom of the glass
            {.dpi = 300, .x = 100, .y = 200, .w = 1001, .h = 333},
            {.dpi = 600, .x = 1000, .y = 3000, .w = 1500, .h = 600},
            {.dpi = 600, .x = 5090, .y = 10, .w = 6, .h = 40},     // a 6 px strip at the right edge
        };
        for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
            fake_iclass_t f;
            fake_iclass_init(&f);
            f.not_ready = (int)i;       // also: blocks that are not ready yet
            sink_t s = {0};
            int r = iclass_run(&f, areas[i], &s, NULL);
            CHECK(r == PIXMA_OK, "lines %d dpi: %s", areas[i].dpi, pixma_strerror(r));
            CHECK(s.lines == areas[i].h && s.mismatches == 0, "lines %d dpi: %d of %d lines, %d wrong", areas[i].dpi,
                  s.lines, areas[i].h, s.mismatches);
            CHECK(f.scans == 1, "lines %d dpi: image not read whole (%llu of %llu bytes)", areas[i].dpi,
                  (unsigned long long)f.image_bytes, (unsigned long long)f.image_total);
            iclass_expect_clean(&f, "lines");
        }
    }
    if (all || !strcmp(only, "abort")) {
        // the consumer stops; a block fails: the session is aborted either way
        fake_iclass_t f;
        fake_iclass_init(&f);
        sink_t s = {.stop_after = 50};
        int r = iclass_run(&f, (pixma_params_t){.dpi = 300, .w = 2480, .h = 3000}, &s, NULL);
        CHECK(r == PIXMA_ECANCELED, "abort: stop: %s", pixma_strerror(r));
        CHECK(f.aborts >= 1, "abort: stop: session not aborted (%s)", f.order);
        iclass_expect_clean(&f, "abort: stop");
        volatile bool cancel = true;
        fake_iclass_init(&f);
        s = (sink_t){0};
        r = iclass_run(&f, (pixma_params_t){.dpi = 150, .w = 500, .h = 500}, &s, &cancel);
        CHECK(r == PIXMA_ECANCELED && f.aborts >= 1, "abort: cancel: %s, %d aborts", pixma_strerror(r), f.aborts);
        iclass_expect_clean(&f, "abort: cancel");
        fake_iclass_init(&f);
        f.fail_at_block = 3;
        s = (sink_t){0};
        r = iclass_run(&f, (pixma_params_t){.dpi = 600, .w = 4000, .h = 2000}, &s, NULL);
        CHECK(r < 0 && f.error_infos == 1 && f.aborts >= 1, "abort: failed block: %s, %d error infos, %d aborts",
              pixma_strerror(r), f.error_infos, f.aborts);
        iclass_expect_clean(&f, "abort: failed block");
        // a command inside the session fails: the session is still ended
        unsigned codes[] = {0xdd20, 0xde20};
        for (int i = 0; i < 2; i++) {
            fake_iclass_init(&f);
            f.fail_command = codes[i];
            s = (sink_t){0};
            r = iclass_run(&f, (pixma_params_t){.dpi = 150, .w = 500, .h = 500}, &s, NULL);
            CHECK(r < 0 && f.aborts >= 1, "abort: failed %04x: %s, %d aborts", codes[i], pixma_strerror(r), f.aborts);
            iclass_expect_clean(&f, "abort: failed command");
        }
    }
    if (all || !strcmp(only, "transport")) {
        fake_iclass_t f;
        fake_iclass_init(&f);
        f.raw_chunk = 37;       // valid short USB transfers inside a declared block
        sink_t s = {0};
        pixma_params_t p = {.dpi = 75, .w = 100, .h = 30};
        int r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_OK && s.lines == p.h && !s.mismatches,
              "transport: short reads: %s, %d lines", pixma_strerror(r), s.lines);
        iclass_expect_clean(&f, "transport: short reads");

        fake_iclass_init(&f);
        f.zero_reads = 100;     // finite backstop also makes the old unbounded loop fail promptly
        s = (sink_t){0};
        r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_ETIMEDOUT, "transport: endless ZLPs accepted: %s", pixma_strerror(r));
        CHECK(f.raw_reads < 20, "transport: ZLP reads not bounded: %d", f.raw_reads);
        iclass_expect_clean(&f, "transport: ZLP timeout");

        fake_iclass_init(&f);
        volatile bool cancel = false;
        f.zero_reads = 100;
        f.cancel = &cancel;
        f.cancel_at_read = 1;
        s = (sink_t){0};
        r = iclass_run(&f, p, &s, &cancel);
        CHECK(r == PIXMA_ECANCELED && s.lines == 1,
              "transport: cancel during ZLPs: %s, %d lines", pixma_strerror(r), s.lines);
        CHECK(f.raw_reads < 20, "transport: canceled ZLP reads not bounded: %d", f.raw_reads);
        iclass_expect_clean(&f, "transport: canceled ZLP");

        // A tiny image fits in the last block: the end flags must not hide stop/cancel.
        p.h = 1;
        for (int external = 0; external < 2; external++) {
            fake_iclass_init(&f);
            cancel = false;
            s = (sink_t){.stop_after = 1, .cancel_at_stop = external ? &cancel : NULL};
            r = iclass_run(&f, p, &s, external ? &cancel : NULL);
            CHECK(r == PIXMA_ECANCELED && s.lines == 1,
                  "transport: final block %s: %s", external ? "cancel" : "stop", pixma_strerror(r));
            iclass_expect_clean(&f, "transport: final block stop");
        }

        // Asking below the glass legitimately clips the height; losing an in-glass line fails.
        p = (pixma_params_t){.dpi = 75, .w = 100, .y = 870, .h = 20};
        for (int missing = 0; missing < 2; missing++) {
            fake_iclass_init(&f);
            f.short_lines = missing;
            s = (sink_t){0};
            r = iclass_run(&f, p, &s, NULL);
            CHECK(r == (missing ? PIXMA_EPROTO : PIXMA_OK) && s.lines == 7 - missing,
                  "transport: clipped image missing %d: %s, %d lines", missing, pixma_strerror(r), s.lines);
            iclass_expect_clean(&f, "transport: clipped image");
        }
    }
    if (all || !strcmp(only, "cleanup")) {
        fake_iclass_t f;
        fake_iclass_init(&f);
        f.fail_command = 0xef20; // both aborts fail and leave the device session open
        sink_t s = {0};
        pixma_params_t p = {.dpi = 75, .w = 100, .h = 10};
        int r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_EBUSY && f.session_open && s.lines == 10,
              "cleanup: abort failures hidden: %s, session %d", pixma_strerror(r), f.session_open);
        CHECK(f.errors == 0, "cleanup: %s", f.error);
        s = (sink_t){0};
        size_t before_busy = strlen(f.order);
        r = iclass_run(&f, p, &s, NULL);
        CHECK(!strcmp(f.order + before_busy, "SB"), "cleanup: rejected busy start was aborted: %s", f.order + before_busy);
        CHECK(r == PIXMA_EBUSY && s.lines == 0 && f.session_open,
              "cleanup: open session was reused: %s, %d lines", pixma_strerror(r), s.lines);
        CHECK(f.errors == 0, "cleanup repeat: %s", f.error);
        // Either acknowledged abort suffices to close the session.
        for (unsigned failures = 1; failures <= 2; failures++) {
            fake_iclass_init(&f);
            f.fail_aborts = failures;
            s = (sink_t){0};
            r = iclass_run(&f, p, &s, NULL);
            CHECK(r == PIXMA_OK && s.lines == 10,
                  "cleanup: single abort failure %u: %s", failures, pixma_strerror(r));
            iclass_expect_clean(&f, "cleanup: single abort failure");
            s = (sink_t){0};
            r = iclass_run(&f, p, &s, NULL);
            CHECK(r == PIXMA_OK && s.lines == 10, "cleanup: repeat after recovery: %s", pixma_strerror(r));
            iclass_expect_clean(&f, "cleanup: repeated scan");
        }
    }
    if (all || !strcmp(only, "guards")) {
        fake_iclass_t f;
        fake_iclass_init(&f);
        sink_t s = {0};
        pixma_params_t p = {.dpi = INT_MAX, .w = 100, .h = 10};
        int r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_EINVAL && !f.order[0], "guards: invalid DPI was used: %s", pixma_strerror(r));
        p.dpi = 75;
        f.mute_start = true;
        r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_ETIMEDOUT && f.aborts > 0 && !f.session_open,
              "guards: lost start reply: %s, aborts %d, open %d", pixma_strerror(r), f.aborts, f.session_open);
        iclass_expect_clean(&f, "guards: lost start reply");
        r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_OK && s.lines == 10, "guards: scan after lost start reply: %s", pixma_strerror(r));
        iclass_expect_clean(&f, "guards: next scan");

        fake_iclass_init(&f);
        f.fail_start_write = true;
        r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_EIO && f.aborts > 0 && !f.session_open,
              "guards: failed start write: %s, aborts %d", pixma_strerror(r), f.aborts);
        iclass_expect_clean(&f, "guards: start write failed");

        // Transport error codes do not prove that START_SESSION was rejected.
        const int ambiguous[] = {PIXMA_EIO, PIXMA_ETIMEDOUT, PIXMA_EBUSY, PIXMA_ECANCELED};
        for (int read_error = 0; read_error < 2; read_error++) {
            for (size_t i = 0; i < sizeof(ambiguous) / sizeof(ambiguous[0]); i++) {
                // A lone read timeout is retried, so use the lost-reply test above for that case.
                if (read_error && ambiguous[i] == PIXMA_ETIMEDOUT)
                    continue;
                fake_iclass_init(&f);
                if (read_error)
                    f.start_reply_error = ambiguous[i];
                else
                    f.accepted_start_write_error = ambiguous[i];
                r = iclass_run(&f, p, &s, NULL);
                CHECK(r == ambiguous[i] && f.aborts > 0 && !f.session_open,
                      "guards: accepted start %s error %d: %s, aborts %d, open %d",
                      read_error ? "read" : "write", ambiguous[i], pixma_strerror(r), f.aborts, f.session_open);
                iclass_expect_clean(&f, "guards: ambiguous start error");
                r = iclass_run(&f, p, &s, NULL);
                CHECK(r == PIXMA_OK && s.lines == 10,
                      "guards: scan after ambiguous start error: %s", pixma_strerror(r));
                iclass_expect_clean(&f, "guards: scan after ambiguous error");
            }
        }

        fake_iclass_init(&f);
        f.first_block_data = true;
        r = iclass_run(&f, p, &s, NULL);
        CHECK(r == PIXMA_EPROTO && f.raw_reads > 0,
              "guards: initial image failure not drained: %s, reads %d", pixma_strerror(r), f.raw_reads);
        iclass_expect_clean(&f, "guards: malformed initial response");

        // The emulator must reject malformed initial requests, including reserved fields.
        const int bytes[] = {2, 3, 4, 5, 6, 7, 8, 9, 10};
        for (size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); i++) {
            fake_iclass_init(&f);
            f.stage = 3;
            uint8_t req[11] = {0xd4, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 6};
            req[bytes[i]] = 1;
            pixma_io_t io = fake_iclass_io(&f);
            io.write(io.ctx, req, sizeof(req), 1000);
            CHECK(f.errors > 0, "guards: emulator accepted bad initial request byte %d", bytes[i]);
        }
    }
    if (all || !strcmp(only, "pad")) {
        // Letter at 600 dpi is 5100 px wide, the glass 5096: the last columns are white
        fake_iclass_t f;
        fake_iclass_init(&f);
        sink_t s = {.white_from = ICLASS_MAX_WIDTH_75 * 600 / 75};
        int r = iclass_run(&f, (pixma_params_t){.dpi = 600, .w = 5100, .h = 64}, &s, NULL);
        CHECK(r == PIXMA_OK, "pad: %s", pixma_strerror(r));
        CHECK(s.lines == 64 && s.mismatches == 0, "pad: %d lines, %d wrong", s.lines, s.mismatches);
        CHECK(f.x + f.wx <= ICLASS_MAX_WIDTH_75 * 600 / 75 + 31, "pad: asked for %d px from %d", f.wx, f.x);
        iclass_expect_clean(&f, "pad");
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "pixma")) {
        test_protocol();
        printf(s_fail ? "%d FAILED\n" : "pixma: all passed\n", s_fail);
        return s_fail ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "iclass")) {
        if (!test_iclass(argc >= 3 ? argv[2] : NULL))
            return 2;
        printf(s_fail ? "%d FAILED\n" : "iclass: all passed\n", s_fail);
        return s_fail ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        test_protocol();
        test_iclass(NULL);
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
