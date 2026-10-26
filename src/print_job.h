// The printer driver the firmware is built for (CONFIG_PRINTER_DRIVER), its
// handshake with the printer, and the button's test page job: the driver
// renders the page line by line and it goes to the printer (or, for a dry
// run, into a CRC so the output can be compared with the host harness:
// `host/bjl_host testpage <dpi> out.prn`).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "printer_driver.h"

typedef struct {
    uint32_t bytes;     // stream size
    uint32_t crc32;     // stream CRC-32 (dry run only)
    uint32_t ms;        // wall time
} print_stats_t;

// The driver chosen in menuconfig ("Printer server" menu).
const printer_driver_t *print_job_driver(void);
// A driver's handshake point (printer_io_t.sync): sends what is buffered and
// waits a while for the printer's reply to `what`; 0, or -1 if the printer is gone.
int print_job_sync(const char *what);
// Test page resolution chosen in menuconfig
int print_job_default_dpi(void);
esp_err_t print_job_test_page(bool dry_run, int dpi, print_stats_t *stats);
