// One large buffer (most of the PSRAM on the ESP32) shared by jobs that never
// run at the same time: scans, and the print capture of dev mode. A user
// takes it for writing, which drops the previous content; what it leaves
// behind stays readable (e.g. the last scan) until the next user takes it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SBUF_NONE = 0,
    SBUF_SCAN,
    SBUF_PRINT,
} sbuf_user_t;

void sbuf_init(uint8_t *data, size_t size);
size_t sbuf_size(void);

// Exclusive write access. NULL while someone else writes or reads it.
uint8_t *sbuf_acquire(sbuf_user_t user, size_t *size);
// Ends the write access; content_len bytes stay readable as `user`'s content.
void sbuf_release(sbuf_user_t user, size_t content_len);

// Read access to `user`'s content (blocks writers until sbuf_read_end).
// NULL if the buffer holds nothing from this user.
const uint8_t *sbuf_read_begin(sbuf_user_t user, size_t *len);
void sbuf_read_end(void);
// Length of `user`'s content, 0 if none.
size_t sbuf_content(sbuf_user_t user);

#ifdef __cplusplus
}
#endif
