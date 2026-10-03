// The web UI (web/, a Svelte app) served from flash: the files of web/dist,
// gzipped by tools/gen_web.py into web_assets.c. Portable.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airprint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;           // "/index.html", "/assets/index-1a2b3c4d.js"
    const char *type;           // Content-Type
    const char *etag;           // "\"<hash>\""
    const uint8_t *data;        // gzip
    size_t len;
} webui_asset_t;

// generated: the assets, terminated by an entry with path == NULL
extern const webui_asset_t WEBUI_ASSETS[];

// GET/HEAD of "/" and the files of the app; false for other paths (and for
// "/" when the app was not built, so the server's own status page shows).
bool webui_http(airprint_http_t *h, const char *method, const char *path);

#ifdef __cplusplus
}
#endif
