// Emulated imageCLASS generation-2 scanner (MF220 Series) for the host tests:
// the device side of iclass_scan's protocol through a pixma_io_t. Checks every
// command (order, header, lengths, checksums, parameters, unread replies) and
// sends the fake_scanner test page in irregular image blocks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixma_scan.h"

typedef struct {
    // --- settings
    int not_ready;              // answer this many block requests with "no data yet"
    int fail_at_block;          // > 0: this image block fails (status 0x1515)
    bool adf_paper;             // the status reports paper in the ADF
    unsigned fail_command;      // this command code is answered "failed" (0x1515, bare)
    unsigned cut_command;       // this command code is answered with a bare "ok" (cut short)
    int mute_status;            // the first this many status commands get no reply
    int corrupt_status;         // > 0: the checksums of status replies from this one (1 = first) on are wrong
    bool mute_start;           // accept the next start, but lose its reply
    bool fail_start_write;     // reject the next start before it reaches the device
    int accepted_start_write_error; // accept next start, but report this transport write error
    int start_reply_error;     // transport error on reading next start reply (not a scanner status)
    bool first_block_data;     // malformed first response declares unexpected raw image data
    unsigned fail_aborts;       // bitmask of abort attempts (bit 0 = first) answered failed
    size_t raw_chunk;           // > 0: maximum bytes in a continuation transfer
    int zero_reads;             // continuation transfers returning zero before data
    int raw_reads;              // continuation read calls (including empty transfers)
    volatile bool *cancel;     // optional cancellation raised at cancel_at_read
    int cancel_at_read;
    int short_lines;            // > 0: omit this many complete lines from the image
    // --- results
    int errors;                 // protocol violations
    char error[160];            // the first one
    int scans;                  // whole images sent, then the session aborted
    bool session_open;
    int aborts, status_queries, error_infos;
    char order[4096];             // commands in order: S status, B begin, R source, P params, 0 first read,
                                //   I image read, A abort, E error info
    int dpi, x, y, wx, h;       // last parameters
    uint64_t image_bytes, image_total;
    // --- internal
    int stage, blocks;
    int pending_read_error;
    uint32_t rng;
    uint8_t resp[600];
    size_t resp_len, resp_pos;
    uint64_t raw_left;          // block data still to send after the reply
} fake_iclass_t;

void fake_iclass_init(fake_iclass_t *f);
pixma_io_t fake_iclass_io(fake_iclass_t *f);
// byte i of the raw image (wx RGB pixels per line) of the last parameters
uint8_t fake_iclass_byte(const fake_iclass_t *f, uint64_t i);
