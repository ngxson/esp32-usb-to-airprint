// Checks the Kyocera driver against rastertokpsl-re: reads a CUPS raster made
// with its PPD (the imageable area, 8-bit K), hands it to the driver as
// AirPrint would (whole page, gray, top left corner at (0, 0)) and writes the
// KPSL on stdout. On a Mac (cupsfilter, libcups):
//
//   cupsfilter -p Kyocera_FS-1020MFPGDI_RE.ppd -m application/vnd.cups-raster -o PageSize=A4 doc.pdf > a4.ras
//   rastertokpsl-re 1 airprint "" 1 PageSize=A4 a4.ras > ref.kpsl
//   ./kpsl_host A4 < a4.ras > out.kpsl
//   cmp -l ref.kpsl out.kpsl     # only bytes 53-66 differ: the time stamp
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_driver.h"

static int out(void *ctx, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, stdout) == len ? 0 : -1;
}

int main(int argc, char **argv)
{
    int paper = argc > 1 && !strcmp(argv[1], "Letter") ? 1 : 0;
    // the page in points and the PPD's imageable area's top left corner
    static const int PAGE[2][4] = {{595, 842, 12, 10}, {612, 792, 12, 16}};
    const printer_driver_t *d = printer_driver_find("kyocera-fs1040");
    cups_raster_t *ras = cupsRasterOpen(0, CUPS_RASTER_READ);
    cups_page_header2_t h;
    printer_io_t io = {.write = out};
    void *job = NULL;
    uint8_t *k = NULL, *gray = NULL;
    int err = 0;
    while (!err && cupsRasterReadHeader2(ras, &h)) {
        int dpi = h.HWResolution[0];
        int src_w = PAGE[paper][0] * dpi / 72 + 1, src_h = PAGE[paper][1] * dpi / 72 + 1;
        int left = (PAGE[paper][2] * dpi + 36) / 72, top = (PAGE[paper][3] * dpi + 36) / 72;
        if (h.cupsBitsPerPixel != 8 || h.cupsBytesPerLine < h.cupsWidth || left + h.cupsWidth > (unsigned)src_w ||
            top + h.cupsHeight > (unsigned)src_h) {
            fprintf(stderr, "kpsl_host: a %ux%u, %u-bit raster is not the %s PPD's\n", h.cupsWidth, h.cupsHeight,
                    h.cupsBitsPerPixel, paper ? "Letter" : "A4");
            err = 1;
            break;
        }
        if (!job && !(job = d->job_begin(d, &io, dpi, paper))) {
            err = 1;
            break;
        }
        free(k);
        free(gray);
        k = malloc(h.cupsBytesPerLine);
        gray = malloc(src_w);
        if (!k || !gray || d->page_begin(job, src_w, src_h, 1)) {
            err = 1;
            break;
        }
        for (int y = 0; y < src_h && !err; y++) {
            const uint8_t *px = NULL;
            if (y >= top && y < top + (int)h.cupsHeight) {
                if (cupsRasterReadPixels(ras, k, h.cupsBytesPerLine) != h.cupsBytesPerLine) {
                    fprintf(stderr, "kpsl_host: the raster ends within a page\n");
                    err = 1;
                    break;
                }
                memset(gray, 255, src_w);
                for (unsigned x = 0; x < h.cupsWidth; x++)
                    gray[left + x] = 255 - k[x];
                px = gray;
            }
            err = d->page_line(job, y, px) != 0;
        }
        if (!err)
            err = d->page_end(job) != 0;
    }
    free(k);
    free(gray);
    cupsRasterClose(ras);
    // after an error too: the driver ends the page and the job
    if (job && d->job_end(job))
        err = 1;
    return err;
}
