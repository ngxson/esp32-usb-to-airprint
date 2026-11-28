// Canon UFR II LT (the MF220 series' language): the parts of the driver
// the host tests reach below the printer_driver_t interface. Portable C.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The printer's raster: 1200 x 600 dpi (twice the job's dpi across), one bit
// per dot, 1 = black, first dot in the top bit. Bytes per line of a job.
int canon_ufr2_line_bytes(void *job);
// Adds the page's next raster line (NULL = white), instead of page_line()'s
// gray pixels. 0 = ok.
int canon_ufr2_put_line(void *job, const uint8_t *bits);

#ifdef __cplusplus
}
#endif
