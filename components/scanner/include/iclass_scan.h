// Flatbed scanning on Canon's imageCLASS / i-SENSYS MF lasers of SANE's
// "generation 2" (the MF220 Series: MF221d, USB 04a9:27a8, interface 0),
// following SANE's pixma backend (backend/pixma/pixma_imageclass.c). Same
// interface as pixma_scan(): the platform supplies the bulk transfers.
//
// Session: status, start session, select source (flatbed), scan parameters,
// image blocks (command 0xd460) until the end flags, abort session, status.
// Image data: 8-bit RGB lines (interleaved), padded to a multiple of 32 pixels.
#pragma once

#include "pixma_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Scan area of the MF220 Series' glass in 1/75 inch (SANE: 637 x 877 on the flatbed)
#define ICLASS_MAX_WIDTH_75 637
#define ICLASS_MAX_HEIGHT_75 877

// A complete scan (pixma_scan's contract): p->calibrate is ignored (the
// scanner calibrates itself); columns beyond the glass come back white.
int iclass_scan(const pixma_io_t *io, const pixma_params_t *p, pixma_line_fn line, void *user,
                volatile bool *cancel);

#ifdef __cplusplus
}
#endif
