// Helpers shared by every printer driver (printer_driver.h).
#include "printer_driver.h"

#include <stdio.h>
#include <stdlib.h>

int printer_driver_paper_for(const printer_driver_t *d, int height_px, int dpi)
{
    // 1/100 mm; e.g. a Letter page (279.4 mm) is closer to Letter than to A4 (297 mm)
    long h = (long)height_px * 2540 / dpi;
    int best = 0;
    for (int i = 1; i < d->npapers; i++)
        if (labs(d->papers[i].height - h) < labs(d->papers[best].height - h))
            best = i;
    return best;
}

void printer_driver_urf(const printer_driver_t *d, char *buf, size_t n)
{
    // V1.4, one copy, 8-bit gray (+ sRGB), the resolutions, plain paper, 3 qualities
    int lo = d->resolutions[0], hi = d->resolutions[d->nresolutions - 1];
    char rs[24];
    if (lo == hi)
        snprintf(rs, sizeof(rs), "%d", lo);
    else
        snprintf(rs, sizeof(rs), "%d-%d", lo, hi);
    snprintf(buf, n, "V1.4,CP1,W8%s,RS%s,MT1,PQ3-4-5", d->color ? ",SRGB24" : "", rs);
}

bool printer_driver_has_dpi(const printer_driver_t *d, int dpi)
{
    for (int i = 0; i < d->nresolutions; i++)
        if (d->resolutions[i] == dpi)
            return true;
    return false;
}
