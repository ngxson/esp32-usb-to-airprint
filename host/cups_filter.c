// CUPS filter: raster in, a printer driver's data out. Lets a Linux CUPS
// server print through the same driver as the firmware, e.g. to a printer
// that has no ESP32 board (tools/cups/README.md).
//
//   rastertoprinter job user title copies options [file]
//
// Input: CUPS raster v3 (what gstoraster / pdftoraster write for a PPD
// queue, 8-bit sGray or sRGB), else PWG raster or URF through raster.c.
//
// The driver is PRINTER_DRIVER from the environment, else FILTER_DRIVER
// (default "tp732h"). CUPS makes the copies (*cupsManualCopies: True).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "printer_driver.h"
#include "raster.h"

#ifndef FILTER_DRIVER
#define FILTER_DRIVER "tp732h"
#endif

typedef struct {
    const printer_driver_t *d;
    printer_io_t io;
    void *job;
    int pages;
    bool failed;        // a callback stopped the decoding
} filter_t;

static int write_out(void *ctx, const uint8_t *data, size_t len)
{
    while (len) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

typedef struct {
    int fd;
    uint8_t magic[4];   // read first, handed to raster.c again
    size_t magic_pos;
} input_t;

static int read_in(void *ctx, uint8_t *buf, size_t n)
{
    input_t *in = ctx;
    if (in->magic_pos < sizeof(in->magic)) {
        size_t k = sizeof(in->magic) - in->magic_pos;
        k = k < n ? k : n;
        memcpy(buf, in->magic + in->magic_pos, k);
        in->magic_pos += k;
        return (int)k;
    }
    for (;;) {
        ssize_t r = read(in->fd, buf, n);
        if (r >= 0 || errno != EINTR)
            return r < 0 ? -1 : (int)r;
    }
}

// n bytes from fd: 0 = ok, 1 = end of data before the first byte, -1 = error
static int read_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r == 0 && got == 0 ? 1 : -1;
        got += (size_t)r;
    }
    return 0;
}

static int on_page_begin(void *user, const raster_page_t *pg)
{
    filter_t *f = user;
    if (!printer_driver_has_dpi(f->d, pg->dpi)) {
        fprintf(stderr, "ERROR: page %d is %d dpi, the %s prints %d dpi\n", pg->page, pg->dpi, f->d->id,
                f->d->resolutions[f->d->nresolutions - 1]);
        return f->failed = true;
    }
    int paper = printer_driver_paper_for(f->d, pg->height, pg->dpi);
    if (!f->job && !(f->job = f->d->job_begin(f->d, &f->io, pg->dpi, paper))) {
        fprintf(stderr, "ERROR: out of memory\n");
        return f->failed = true;
    }
    fprintf(stderr, "INFO: page %d: %dx%d at %d dpi\n", pg->page, pg->width, pg->height, pg->dpi);
    return f->failed = f->d->page_begin(f->job, pg->width, pg->height, pg->bytes_pp) != 0;
}

static int on_line(void *user, int y, const uint8_t *pixels)
{
    filter_t *f = user;
    return f->failed = f->d->page_line(f->job, y, pixels) != 0;
}

static int on_page_end(void *user, const raster_page_t *pg)
{
    filter_t *f = user;
    if ((f->failed = f->d->page_end(f->job) != 0))
        return 1;
    fprintf(stderr, "PAGE: %d 1\n", pg->page);
    f->pages++;
    return 0;
}

// CUPS raster v3: per page a 1796-byte header (the PWG layout, in the
// writer's byte order: "RaS3" big-endian, "3SaR" little-endian), then
// height x bytes-per-line uncompressed. Returns pages, or < 0.
static int decode_cups_v3(int fd, bool le, const raster_cb_t *cb, filter_t *f)
{
    uint8_t h[1796];
    int pages = 0;
    for (;;) {
        int e = read_full(fd, h, sizeof(h));
        if (e)
            return e == 1 ? pages : RASTER_ERR_IO;
#define U32(o) (le ? (uint32_t)h[o] | h[o + 1] << 8 | h[o + 2] << 16 | (uint32_t)h[o + 3] << 24 \
                   : (uint32_t)h[o] << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3])
        uint32_t xres = U32(276), yres = U32(280), bits_pp = U32(388), bpl = U32(392), cspace = U32(400);
        raster_page_t pg = {.page = pages + 1, .width = (int)U32(372), .height = (int)U32(376), .dpi = (int)xres,
                            .bytes_pp = (int)bits_pp / 8};
#undef U32
        if (!((cspace == 18 && bits_pp == 8) || (cspace == 19 && bits_pp == 24)) || xres != yres) {
            fprintf(stderr, "ERROR: CUPS raster: colour space %u, %u bits per pixel, %ux%u dpi: need 8-bit sGray "
                    "or sRGB (the PPD's setpagedevice)\n", (unsigned)cspace, (unsigned)bits_pp, (unsigned)xres,
                    (unsigned)yres);
            return RASTER_ERR_UNSUPPORTED;
        }
        if (pg.width <= 0 || pg.height <= 0 || pg.width > 20000 || pg.height > 30000 ||
            bpl != (uint32_t)pg.width * (uint32_t)pg.bytes_pp)
            return RASTER_ERR_FORMAT;
        uint8_t *line = malloc(bpl);
        if (!line)
            return RASTER_ERR_NOMEM;
        int r = cb->page_begin(f, &pg);
        for (int y = 0; !r && y < pg.height; y++)
            r = read_full(fd, line, bpl) ? RASTER_ERR_IO : cb->line(f, y, line);
        if (!r)
            r = cb->page_end(f, &pg);
        free(line);
        if (r)
            return r;
        pages++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }
    const char *id = getenv("PRINTER_DRIVER") ? getenv("PRINTER_DRIVER") : FILTER_DRIVER;
    filter_t f = {.d = printer_driver_find(id), .io = {.write = write_out}};
    if (!f.d) {
        fprintf(stderr, "ERROR: no printer driver \"%s\"\n", id);
        return 1;
    }
    input_t in = {.fd = argc == 7 ? open(argv[6], O_RDONLY) : STDIN_FILENO};
    if (in.fd < 0) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n", argv[6], strerror(errno));
        return 1;
    }
    static const raster_cb_t cb = {.page_begin = on_page_begin, .line = on_line, .page_end = on_page_end};
    int r = read_full(in.fd, in.magic, sizeof(in.magic)) ? RASTER_ERR_FORMAT
            : !memcmp(in.magic, "RaS3", 4)               ? decode_cups_v3(in.fd, false, &cb, &f)
            : !memcmp(in.magic, "3SaR", 4)               ? decode_cups_v3(in.fd, true, &cb, &f)
                                                         : raster_decode(read_in, &in, &cb, &f);
    if (r < 0)
        fprintf(stderr, "ERROR: raster: %s\n", raster_strerror(r));
    int err = f.job ? f.d->job_end(f.job) : 0;
    if (err)
        fprintf(stderr, "ERROR: cannot write to the printer\n");
    fprintf(stderr, "INFO: %d page(s) for the %s\n", f.pages, f.d->id);
    return r < 0 || f.failed || err ? 1 : 0;
}
