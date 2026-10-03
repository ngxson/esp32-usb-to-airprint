// Test page print job: renders the page line by line and streams it to the
// printer (or, for a dry run, into a CRC so the output can be compared with
// the host harness: `host/bjl_host testpage <dpi> out.prn`).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canon_bjl.h"
#include "esp_err.h"

typedef struct {
    uint32_t bytes;     // stream size
    uint32_t crc32;     // stream CRC-32 (dry run only)
    uint32_t ms;        // wall time
} print_stats_t;

// Resolution selected in menuconfig ("Printer server" menu)
bjl_mode_id_t print_job_default_mode(void);
esp_err_t print_job_test_page(bool dry_run, bjl_mode_id_t mode, print_stats_t *stats);
