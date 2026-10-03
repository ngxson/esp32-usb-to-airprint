// Static files of the web UI. Everything is stored gzipped and sent as is
// (every browser accepts gzip). Vite names the files under /assets/ after
// their content, so they are cached for good; index.html is revalidated with
// its ETag on every load.
#include "webui.h"

#include <stdio.h>
#include <string.h>

static const webui_asset_t *find(const char *path, size_t len)
{
    for (const webui_asset_t *a = WEBUI_ASSETS; a->path; a++)
        if (strlen(a->path) == len && !memcmp(a->path, path, len))
            return a;
    return NULL;
}

bool webui_http(airprint_http_t *h, const char *method, const char *path)
{
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return false;
    size_t len = strcspn(path, "?#");
    if (len == 1 && path[0] == '/') {
        path = "/index.html";
        len = strlen(path);
    }
    const webui_asset_t *a = find(path, len);
    if (!a)
        return false;
    bool immutable = !strncmp(a->path, "/assets/", 8);
    char hdr[192];
    snprintf(hdr, sizeof(hdr), "Content-Encoding: gzip\r\nETag: %s\r\nCache-Control: %s\r\nVary: Accept-Encoding\r\n",
             a->etag, immutable ? "public, max-age=31536000, immutable" : "no-cache");
    if (!strcmp(airprint_http_if_none_match(h), a->etag))
        airprint_http_send_ex(h, "304 Not Modified", a->type, hdr, "", 0);
    else
        airprint_http_send_ex(h, "200 OK", a->type, hdr, a->data, a->len);
    return true;
}
