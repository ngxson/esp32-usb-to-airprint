// Live state for the web UI: one JSON snapshot of the device, pushed to every
// browser that listens on GET /events (Server-Sent Events) whenever it
// changes, and readable once at GET /api/state.
//
// The snapshot is {"<key>": <value>, ...}, one member per source (printer,
// scan, ...). A single thread serves all listeners: it rebuilds the snapshot
// every TICK and sends it when it differs from the last one, or a keep-alive
// comment every 15 s. Listeners hand their socket over to that thread, so they
// take no connection slot or thread of the HTTP server. A listener that
// cannot take an event at once (full send buffer) is dropped; EventSource
// reconnects by itself.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "airprint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENTS_MAX_LISTENERS 4      // the oldest is dropped for a new one

// Writes one JSON value into buf (at most n bytes, NUL terminated); returns
// its length, or < 0 to leave the member out.
typedef int (*events_source_fn)(void *ctx, char *buf, size_t n);

// Adds a snapshot member; before events_start. key must stay valid.
void events_add_source(const char *key, events_source_fn fn, void *ctx);
int events_start(void);
// Rebuild the snapshot now rather than at the next tick (e.g. a scan ended).
void events_poke(void);
// GET /events and GET /api/state; false for other paths.
bool events_http(airprint_http_t *h, const char *method, const char *path);

// JSON string helper for sources: appends "s" (quoted, escaped) at buf + pos.
// Returns the new position (never beyond n - 1).
size_t events_json_str(char *buf, size_t pos, size_t n, const char *s);

#ifdef __cplusplus
}
#endif
