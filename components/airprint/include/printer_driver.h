// A printer model: what it can print and how full-page raster lines become
// the bytes its USB port expects. The print server (airprint.c), the test
// page and the platform glue only use this interface; a driver lives in
// components/printers/ and is listed in printers.c. Portable C.
//
// To support another printer: write a driver (see canon_pixma.c), add it
// to printers.c (see README). The firmware uses the driver that matches the
// attached printer (printer_driver_match), else CONFIG_PRINTER_DRIVER.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airprint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRINTER_MAX_RESOLUTIONS 4

// Where the printer data goes. Set up by the caller of job_begin/test_page.
typedef struct {
    // the printer's bytes, in order; < 0 aborts the job
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    // optional: flush, then wait for the printer's reply to `what` (e.g.
    // Canon's "StartJob"); < 0 = the printer is gone. Drivers carry on
    // without a reply.
    int (*sync)(void *ctx, const char *what);
    // optional: called every 64 lines of a test page (yield, log progress)
    void (*progress)(void *ctx, int line, int lines);
    void *ctx;
} printer_io_t;

typedef struct {
    const char *name;           // "A4"
    const char *pwg;            // IPP media name, "iso_a4_210x297mm"
    int width, height;          // 1/100 mm
} printer_paper_t;

typedef struct printer_driver {
    const char *id;             // CONFIG_PRINTER_DRIVER value, e.g. "canon-mg2500"
    const char *make_model;     // IPP / mDNS / eSCL, "Canon MG2500 series"
    const char *usb_mfg;        // IEEE 1284 MFG and MDL: the mDNS record, and how
    const char *usb_mdl;        // printer_driver_match recognises the device
    uint16_t usb_vid, usb_pid;  // the device's USB ids, 0 = unknown (matched by usb_mdl)
    bool color;                 // colour printing (else gray only)
    bool scanner;               // the device has a Canon pixma scanner (components/scanner)
    int resolutions[PRINTER_MAX_RESOLUTIONS];   // dpi, ascending; the last is the default
    int nresolutions;
    const printer_paper_t *papers;              // the first is the default
    int npapers;
    int margin_left, margin_right, margin_top, margin_bottom;   // unprintable borders, 1/100 mm
    const void *model;          // the driver's own data (e.g. the Canon stream variant)

    // --- a job: pages of raster lines at one dpi on one paper.
    // Starts the job (writes its header); NULL if out of memory.
    void *(*job_begin)(const struct printer_driver *d, const printer_io_t *io, int dpi, int paper);
    // A page whose source raster is src_w x src_h pixels at the job's dpi,
    // paper corner at (0, 0); bytes_pp 1 = gray, 3 = RGB. 0 = ok.
    int (*page_begin)(void *job, int src_w, int src_h, int bytes_pp);
    // Source line y (in order); px NULL = a white line. 0 = ok.
    int (*page_line)(void *job, int y, const uint8_t *px);
    int (*page_end)(void *job);
    // Finishes the job, also after an error or a cancel (ejects a partly
    // printed page), and frees it. Returns 0, or < 0 if the printer link
    // failed (the platform then resets the printer).
    int (*job_end)(void *job);
    // optional: a line about the job for the log (render time, ...)
    void (*job_stats)(void *job, char *buf, size_t n);

    // --- optional: the button's test page (label: a line printed on it)
    int (*test_page)(const struct printer_driver *d, const printer_io_t *io, int dpi, const char *label);

    // --- optional: the printer's own status from its back channel (the
    // fields of the last status reply the USB layer reassembled) into IPP
    // terms: reasons, message, ink levels.
    void (*status)(const char *state, const char *detail, const char *data, airprint_status_t *st);
} printer_driver_t;

// The driver with this id (printers.c), NULL if none.
const printer_driver_t *printer_driver_find(const char *id);
// The driver of a device: same USB vid:pid, else same IEEE 1284 MFG and MDL
// (from its Device ID, "MFG:Canon;...;MDL:MG2500 series;"; case and a
// " series" suffix ignored). NULL if none.
const printer_driver_t *printer_driver_match(uint16_t vid, uint16_t pid, const char *device_id);
// Every driver, NULL-terminated.
extern const printer_driver_t *const printer_drivers[];

// The paper of a page: the driver's paper closest in height.
int printer_driver_paper_for(const printer_driver_t *d, int height_px, int dpi);
// The URF capability string (DNS-SD TXT and IPP), e.g.
// "V1.4,CP1,W8,SRGB24,RS300-600,MT1,PQ3-4-5".
void printer_driver_urf(const printer_driver_t *d, char *buf, size_t n);
bool printer_driver_has_dpi(const printer_driver_t *d, int dpi);

#ifdef __cplusplus
}
#endif
