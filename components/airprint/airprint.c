// AirPrint / IPP Everywhere server. One thread per HTTP connection; a single
// job prints at a time and is streamed straight from the socket to the
// printer (raster line -> Canon raster -> printer), so documents of any size
// fit in a few tens of KB.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     // strcasestr
#endif
#include "airprint.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bjl_render.h"
#include "canon_bjl.h"
#include "events.h"
#include "icon_png.h"
#include "ipp.h"
#include "raster.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define LOGI(...) ESP_LOGI("airprint", __VA_ARGS__)
#define LOGW(...) ESP_LOGW("airprint", __VA_ARGS__)
#else
#define LOGI(fmt, ...) fprintf(stderr, "I airprint: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "W airprint: " fmt "\n", ##__VA_ARGS__)
#endif

// iOS keeps several keep-alive connections open while it watches a job
#define MAX_CONNECTIONS 8
#ifdef ESP_PLATFORM
#define CONN_STACK_SIZE (12 * 1024)     // the print pipeline runs on this stack
#define LISTEN_STACK_SIZE (4 * 1024)
#else
#define CONN_STACK_SIZE (512 * 1024)    // host build runs with sanitizers
#define LISTEN_STACK_SIZE (256 * 1024)
#endif
#define IDLE_TIMEOUT_S 15
#define MAX_JOBS 10
#define MULTIPLE_OP_TIMEOUT_S 60

// borders in 1/100 mm, matching the printable area of canon_bjl
#define MARGIN_LEFT 353     // 10 pt
#define MARGIN_RIGHT 353
#define MARGIN_TOP 318      // 9 pt
#define MARGIN_BOTTOM 529   // 15 pt

enum {
    JOB_PENDING = 3,
    JOB_HELD = 4,
    JOB_PROCESSING = 5,
    JOB_CANCELED = 7,
    JOB_ABORTED = 8,
    JOB_COMPLETED = 9,
};

typedef struct {
    int id;
    int state;
    char name[64];
    char user[32];
    char format[64];
    const char *reason;         // job-state-reasons keyword
    time_t created, processing, completed;
    int impressions;
    int64_t bytes;
    volatile bool cancel;
} job_t;

static const airprint_config_t *s_cfg;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static job_t s_jobs[MAX_JOBS];      // ring of recent jobs
static int s_next_id = 1;
static job_t *s_active;             // pending or processing job
static time_t s_start;
static int s_connections;

// ===========================================================================
// HTTP connection

struct airprint_http {
    int fd;
    uint8_t buf[2048];
    size_t pos, len;
    // request
    char method[8];
    char path[128];
    char host[96];
    char if_none_match[48];
    bool keep_alive;
    bool http10;                // HTTP/1.0 client: no chunked responses
    bool chunked_out;           // streaming response uses chunked encoding
    // body
    bool chunked;
    int64_t remaining;          // Content-Length mode: bytes left; chunked: bytes left in chunk
    bool body_eof;
    bool in_chunks;             // a chunk was read: CRLF precedes the next size line
    bool detached;              // the socket was handed over (airprint_http_detach)
};
typedef struct airprint_http conn_t;

static int conn_fill(conn_t *c)
{
    if (c->pos < c->len)
        return 0;
    ssize_t n = recv(c->fd, c->buf, sizeof(c->buf), 0);
    if (n <= 0)
        return -1;
    c->pos = 0;
    c->len = (size_t)n;
    return 0;
}

static int read_line(conn_t *c, char *out, size_t out_len)
{
    size_t n = 0;
    for (;;) {
        if (conn_fill(c))
            return -1;
        char ch = (char)c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && n < out_len - 1)
            out[n++] = ch;
    }
    out[n] = 0;
    return (int)n;
}

// Sends in pieces through a stack buffer: the source may be in PSRAM, and
// one huge send() call has been seen to fail on lwIP.
static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t chunk[1460];
    while (len) {
        size_t piece = len < sizeof(chunk) ? len : sizeof(chunk);
        memcpy(chunk, p, piece);
        size_t off = 0;
        while (off < piece) {
            ssize_t n = send(fd, chunk + off, piece - off, 0);
            if (n <= 0) {
                LOGW("send failed after %u bytes (errno %d)", (unsigned)(p - (const uint8_t *)data + off), errno);
                return -1;
            }
            off += (size_t)n;
        }
        p += piece;
        len -= piece;
    }
    return 0;
}

// Reads up to n body bytes; 0 at end of body, < 0 on error.
static int body_read(conn_t *c, uint8_t *out, size_t n)
{
    if (c->body_eof)
        return 0;
    if (c->chunked && c->remaining == 0) {
        char line[64];
        if (c->in_chunks && read_line(c, line, sizeof(line)) < 0)   // CRLF after chunk data
            return -1;
        if (read_line(c, line, sizeof(line)) < 0)
            return -1;
        long size = strtol(line, NULL, 16);
        c->in_chunks = true;
        if (size <= 0) {
            while (read_line(c, line, sizeof(line)) > 0)   // trailers until empty line
                ;
            c->body_eof = true;
            return 0;
        }
        c->remaining = size;
    }
    if (c->remaining <= 0) {
        c->body_eof = true;
        return 0;
    }
    if (conn_fill(c))
        return -1;
    size_t take = c->len - c->pos;
    if (take > n)
        take = n;
    if ((int64_t)take > c->remaining)
        take = (size_t)c->remaining;
    memcpy(out, c->buf + c->pos, take);
    c->pos += take;
    c->remaining -= (int64_t)take;
    if (!c->chunked && c->remaining == 0)
        c->body_eof = true;
    return (int)take;
}

// exact read for the IPP parser
static int body_read_exact(void *ctx, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int k = body_read(ctx, buf + got, n - got);
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return (int)n;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct {
    conn_t *c;
    job_t *job;
    int64_t wait_us;            // time spent waiting for document data
} doc_reader_t;

static int doc_read(void *ctx, uint8_t *buf, size_t n)
{
    doc_reader_t *d = ctx;
    int64_t t0 = now_us();
    int k = body_read(d->c, buf, n);
    d->wait_us += now_us() - t0;
    if (k > 0)
        d->job->bytes += k;
    return k;
}

// Discards the rest of the request body. Clients only read the response once
// they have sent the whole body; closing with unread data would reset the
// connection and lose the response (libcups then retries the whole job).
static void drain_body(conn_t *c)
{
    uint8_t tmp[512];
    while (body_read(c, tmp, sizeof(tmp)) > 0)
        ;
    if (!c->body_eof)
        c->keep_alive = false;
}

static void send_http_ex(conn_t *c, const char *status, const char *type, const char *extra_headers,
                         const void *body, size_t len)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%sConnection: %s\r\n\r\n", status,
                     type, (unsigned)len, extra_headers ? extra_headers : "", c->keep_alive ? "keep-alive" : "close");
    if (n >= (int)sizeof(hdr)) {
        c->keep_alive = false;
        return;
    }
    if (send_all(c->fd, hdr, n) == 0 && len)
        send_all(c->fd, body, len);
}

static void send_http(conn_t *c, const char *status, const char *type, const void *body, size_t len)
{
    send_http_ex(c, status, type, NULL, body, len);
}

void airprint_http_send(airprint_http_t *c, const char *status, const char *type, const void *body, size_t len)
{
    send_http(c, status, type, body, len);
}

void airprint_http_send_ex(airprint_http_t *c, const char *status, const char *type, const char *extra_headers,
                           const void *body, size_t len)
{
    send_http_ex(c, status, type, extra_headers, body, len);
}

int airprint_http_read_body(airprint_http_t *c, void *buf, size_t max)
{
    size_t got = 0;
    while (got < max) {
        int k = body_read(c, (uint8_t *)buf + got, max - got);
        if (k < 0)
            return -1;
        if (k == 0)
            break;
        got += (size_t)k;
    }
    return (int)got;
}

const char *airprint_http_host(airprint_http_t *c)
{
    return c->host;
}

const char *airprint_http_if_none_match(airprint_http_t *c)
{
    return c->if_none_match;
}

int airprint_http_stream_begin(airprint_http_t *c, const char *type, const char *extra_headers)
{
    // a client that stops reading must not block the sender forever
    struct timeval tv = {.tv_sec = 30};
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    c->chunked_out = !c->http10;
    if (!c->chunked_out)
        c->keep_alive = false;      // the end of the body is the end of the connection
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n%s%sConnection: %s\r\n\r\n", type,
                     c->chunked_out ? "Transfer-Encoding: chunked\r\n" : "", extra_headers ? extra_headers : "",
                     c->keep_alive ? "keep-alive" : "close");
    if (n >= (int)sizeof(hdr) || send_all(c->fd, hdr, (size_t)n) < 0) {
        c->keep_alive = false;
        return -1;
    }
    return 0;
}

int airprint_http_stream_write(airprint_http_t *c, const void *data, size_t len)
{
    if (!len)
        return 0;
    if (c->chunked_out) {
        char size[16];
        int n = snprintf(size, sizeof(size), "%x\r\n", (unsigned)len);
        if (send_all(c->fd, size, (size_t)n) < 0 || send_all(c->fd, data, len) < 0 || send_all(c->fd, "\r\n", 2) < 0) {
            c->keep_alive = false;
            return -1;
        }
        return 0;
    }
    if (send_all(c->fd, data, len) < 0) {
        c->keep_alive = false;
        return -1;
    }
    return 0;
}

int airprint_http_detach(airprint_http_t *c)
{
    c->detached = true;
    c->keep_alive = false;
    return c->fd;
}

int airprint_http_stream_end(airprint_http_t *c)
{
    struct timeval tv = {0};
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (c->chunked_out && send_all(c->fd, "0\r\n\r\n", 5) < 0) {
        c->keep_alive = false;
        return -1;
    }
    return 0;
}

// ===========================================================================
// printer attributes

typedef struct {
    const ipp_request_t *req;
    int idx;                    // requested-attributes index, -1 = all
    bool all;
} filter_t;

static void filter_init(filter_t *f, const ipp_request_t *req)
{
    f->req = req;
    f->idx = ipp_find_index(req, IPP_TAG_OPERATION, "requested-attributes");
    f->all = f->idx < 0;
    for (int i = f->idx; i >= 0 && i < req->nattrs && (i == f->idx || req->attrs[i].name_len == 0); i++) {
        const ipp_attr_t *a = &req->attrs[i];
        if (ipp_attr_is(a, "all") || ipp_attr_is(a, "printer-description") || ipp_attr_is(a, "job-template"))
            f->all = true;
    }
}

static bool want(const filter_t *f, const char *name)
{
    bool explicit_only = strcmp(name, "media-col-database") == 0;
    if (f->all && !explicit_only)
        return true;
    for (int i = f->idx; i >= 0 && i < f->req->nattrs && (i == f->idx || f->req->attrs[i].name_len == 0); i++)
        if (ipp_attr_is(&f->req->attrs[i], name))
            return true;
    return false;
}

static void uri_for(const conn_t *c, char *out, size_t n, const char *scheme, const char *path)
{
    snprintf(out, n, "%s://%s%s", scheme, c->host[0] ? c->host : "localhost", path);
}

static void media_size(ipp_out_t *o, const char *member, int x, int y)
{
    if (member)
        ipp_out_member(o, member);
    ipp_out_coll_begin(o, NULL);
    ipp_out_member(o, "x-dimension");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, x);
    ipp_out_member(o, "y-dimension");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, y);
    ipp_out_coll_end(o);
}

// one media-col value: name == NULL adds another value to the attribute
static void media_col(ipp_out_t *o, const char *name, const char *key, int x, int y)
{
    ipp_out_coll_begin(o, name);
    ipp_out_member(o, "media-key");
    ipp_out_str(o, IPP_TAG_KEYWORD, NULL, key);
    media_size(o, "media-size", x, y);
    ipp_out_member(o, "media-size-name");
    ipp_out_str(o, IPP_TAG_KEYWORD, NULL, key);
    ipp_out_member(o, "media-left-margin");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, MARGIN_LEFT);
    ipp_out_member(o, "media-right-margin");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, MARGIN_RIGHT);
    ipp_out_member(o, "media-top-margin");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, MARGIN_TOP);
    ipp_out_member(o, "media-bottom-margin");
    ipp_out_int(o, IPP_TAG_INTEGER, NULL, MARGIN_BOTTOM);
    ipp_out_member(o, "media-source");
    ipp_out_str(o, IPP_TAG_KEYWORD, NULL, "auto");
    ipp_out_member(o, "media-type");
    ipp_out_str(o, IPP_TAG_KEYWORD, NULL, "stationery");
    ipp_out_coll_end(o);
}

static void get_status(airprint_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (s_cfg->get_status)
        s_cfg->get_status(s_cfg->ctx, st);
    else
        st->connected = true;
}

// printer-state from the job table and the printer
static int printer_state(const airprint_status_t *st)
{
    if (!st->connected)
        return 5;   // stopped
    return (s_active && s_active->state == JOB_PROCESSING) || st->busy ? 4 : 3;
}

static void add_printer_attrs(ipp_out_t *o, const conn_t *c, const filter_t *f)
{
    const airprint_config_t *cfg = s_cfg;
    airprint_status_t st;
    get_status(&st);
    time_t now = time(NULL);
    char uri[160];

#define W(name) want(f, name)
    if (W("charset-configured"))
        ipp_out_str(o, IPP_TAG_CHARSET, "charset-configured", "utf-8");
    if (W("charset-supported"))
        ipp_out_strs(o, IPP_TAG_CHARSET, "charset-supported", (const char *[]){"us-ascii", "utf-8", NULL});
    if (W("color-supported"))
        ipp_out_bool(o, "color-supported", true);
    if (W("compression-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "compression-supported", "none");
    if (W("copies-default"))
        ipp_out_int(o, IPP_TAG_INTEGER, "copies-default", 1);
    if (W("copies-supported"))
        ipp_out_range(o, "copies-supported", 1, 1);
    if (W("document-format-default"))
        ipp_out_str(o, IPP_TAG_MIMETYPE, "document-format-default", "application/octet-stream");
    if (W("document-format-supported"))
        ipp_out_strs(o, IPP_TAG_MIMETYPE, "document-format-supported",
                     (const char *[]){"application/octet-stream", "image/pwg-raster", "image/urf", NULL});
    if (W("finishings-default"))
        ipp_out_int(o, IPP_TAG_ENUM, "finishings-default", 3);
    if (W("finishings-supported"))
        ipp_out_int(o, IPP_TAG_ENUM, "finishings-supported", 3);
    if (W("generated-natural-language-supported"))
        ipp_out_str(o, IPP_TAG_LANGUAGE, "generated-natural-language-supported", "en");
    if (W("identify-actions-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "identify-actions-default", "flash");
    if (W("identify-actions-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "identify-actions-supported", "flash");
    if (W("ipp-features-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "ipp-features-supported",
                     (const char *[]){"airprint-1.4", "ipp-everywhere", NULL});
    if (W("ipp-versions-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "ipp-versions-supported", (const char *[]){"1.1", "2.0", NULL});
    if (W("job-creation-attributes-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "job-creation-attributes-supported",
                     (const char *[]){"copies", "finishings", "media", "media-col", "orientation-requested",
                                      "output-bin", "print-color-mode", "print-content-optimize", "print-quality",
                                      "print-rendering-intent", "printer-resolution", "sides", NULL});
    if (W("job-ids-supported"))
        ipp_out_bool(o, "job-ids-supported", true);
    if (W("media-bottom-margin-supported"))
        ipp_out_int(o, IPP_TAG_INTEGER, "media-bottom-margin-supported", MARGIN_BOTTOM);
    if (W("media-left-margin-supported"))
        ipp_out_int(o, IPP_TAG_INTEGER, "media-left-margin-supported", MARGIN_LEFT);
    if (W("media-right-margin-supported"))
        ipp_out_int(o, IPP_TAG_INTEGER, "media-right-margin-supported", MARGIN_RIGHT);
    if (W("media-top-margin-supported"))
        ipp_out_int(o, IPP_TAG_INTEGER, "media-top-margin-supported", MARGIN_TOP);
    if (W("media-col-database")) {
        media_col(o, "media-col-database", "iso_a4_210x297mm", 21000, 29700);
        media_col(o, NULL, "na_letter_8.5x11in", 21590, 27940);
    }
    if (W("media-col-default"))
        media_col(o, "media-col-default", "iso_a4_210x297mm", 21000, 29700);
    if (W("media-col-ready")) {
        media_col(o, "media-col-ready", "iso_a4_210x297mm", 21000, 29700);
        media_col(o, NULL, "na_letter_8.5x11in", 21590, 27940);
    }
    if (W("media-col-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "media-col-supported",
                     (const char *[]){"media-bottom-margin", "media-left-margin", "media-right-margin", "media-size",
                                      "media-size-name", "media-source", "media-top-margin", "media-type", NULL});
    if (W("media-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "media-default", "iso_a4_210x297mm");
    if (W("media-ready"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "media-ready",
                     (const char *[]){"iso_a4_210x297mm", "na_letter_8.5x11in", NULL});
    if (W("media-size-supported")) {
        ipp_out_coll_begin(o, "media-size-supported");
        ipp_out_member(o, "x-dimension");
        ipp_out_int(o, IPP_TAG_INTEGER, NULL, 21000);
        ipp_out_member(o, "y-dimension");
        ipp_out_int(o, IPP_TAG_INTEGER, NULL, 29700);
        ipp_out_coll_end(o);
        ipp_out_coll_begin(o, NULL);
        ipp_out_member(o, "x-dimension");
        ipp_out_int(o, IPP_TAG_INTEGER, NULL, 21590);
        ipp_out_member(o, "y-dimension");
        ipp_out_int(o, IPP_TAG_INTEGER, NULL, 27940);
        ipp_out_coll_end(o);
    }
    if (W("media-source-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "media-source-supported", "auto");
    if (W("media-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "media-supported",
                     (const char *[]){"iso_a4_210x297mm", "na_letter_8.5x11in", NULL});
    if (W("media-type-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "media-type-supported", "stationery");
    if (W("multiple-document-handling-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "multiple-document-handling-supported",
                    "separate-documents-uncollated-copies");
    if (W("multiple-document-jobs-supported"))
        ipp_out_bool(o, "multiple-document-jobs-supported", false);
    if (W("multiple-operation-time-out"))
        ipp_out_int(o, IPP_TAG_INTEGER, "multiple-operation-time-out", MULTIPLE_OP_TIMEOUT_S);
    if (W("multiple-operation-time-out-action"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "multiple-operation-time-out-action", "abort-job");
    if (W("natural-language-configured"))
        ipp_out_str(o, IPP_TAG_LANGUAGE, "natural-language-configured", "en");
    if (W("operations-supported")) {
        static const int ops[] = {IPP_OP_PRINT_JOB, IPP_OP_VALIDATE_JOB, IPP_OP_CREATE_JOB, IPP_OP_SEND_DOCUMENT,
                                  IPP_OP_CANCEL_JOB, IPP_OP_GET_JOB_ATTRIBUTES, IPP_OP_GET_JOBS,
                                  IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_OP_CANCEL_MY_JOBS, IPP_OP_CLOSE_JOB,
                                  IPP_OP_IDENTIFY_PRINTER};
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
            ipp_out_int(o, IPP_TAG_ENUM, i == 0 ? "operations-supported" : NULL, ops[i]);
    }
    if (W("orientation-requested-default"))
        ipp_out_int(o, IPP_TAG_ENUM, "orientation-requested-default", 3);
    if (W("orientation-requested-supported"))
        ipp_out_int(o, IPP_TAG_ENUM, "orientation-requested-supported", 3);
    if (W("output-bin-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "output-bin-default", "face-up");
    if (W("output-bin-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "output-bin-supported", "face-up");
    if (W("overrides-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "overrides-supported", (const char *[]){"document-number", "pages", NULL});
    if (W("page-ranges-supported"))
        ipp_out_bool(o, "page-ranges-supported", false);
    if (W("pages-per-minute"))
        ipp_out_int(o, IPP_TAG_INTEGER, "pages-per-minute", 8);
    if (W("pages-per-minute-color"))
        ipp_out_int(o, IPP_TAG_INTEGER, "pages-per-minute-color", 4);
    if (W("pdl-override-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "pdl-override-supported", "attempted");
    if (W("preferred-attributes-supported"))
        ipp_out_bool(o, "preferred-attributes-supported", false);
    if (W("print-color-mode-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "print-color-mode-default", "color");
    if (W("print-color-mode-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "print-color-mode-supported",
                     (const char *[]){"auto", "color", "monochrome", NULL});
    if (W("print-content-optimize-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "print-content-optimize-default", "auto");
    if (W("print-content-optimize-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "print-content-optimize-supported", "auto");
    if (W("print-quality-default"))
        ipp_out_int(o, IPP_TAG_ENUM, "print-quality-default", 4);
    if (W("print-quality-supported")) {
        ipp_out_int(o, IPP_TAG_ENUM, "print-quality-supported", 3);
        ipp_out_int(o, IPP_TAG_ENUM, NULL, 4);
        ipp_out_int(o, IPP_TAG_ENUM, NULL, 5);
    }
    if (W("print-rendering-intent-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "print-rendering-intent-default", "auto");
    if (W("print-rendering-intent-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "print-rendering-intent-supported", "auto");
    if (W("printer-config-change-date-time"))
        ipp_out_date(o, "printer-config-change-date-time", s_start);
    if (W("printer-config-change-time"))
        ipp_out_int(o, IPP_TAG_INTEGER, "printer-config-change-time", 0);
    if (W("printer-current-time"))
        ipp_out_date(o, "printer-current-time", now);
    if (W("printer-device-id"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-device-id", cfg->device_id);
    if (W("printer-geo-location"))
        ipp_out_raw(o, IPP_TAG_UNKNOWN, "printer-geo-location", NULL, 0);
    if (W("printer-get-attributes-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "printer-get-attributes-supported", "document-format");
    if (W("printer-icons")) {
        uri_for(c, uri, sizeof(uri), "http", "/icon.png");
        ipp_out_str(o, IPP_TAG_URI, "printer-icons", uri);
    }
    if (W("printer-info"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-info", cfg->name);
    if (W("printer-is-accepting-jobs"))
        ipp_out_bool(o, "printer-is-accepting-jobs", st.connected);
    if (W("printer-kind"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "printer-kind", "document");
    if (W("printer-location"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-location", "");
    if (W("printer-make-and-model"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-make-and-model", cfg->make_model);
    if (W("printer-more-info")) {
        uri_for(c, uri, sizeof(uri), "http", "/");
        ipp_out_str(o, IPP_TAG_URI, "printer-more-info", uri);
    }
    if (W("printer-name"))
        ipp_out_str(o, IPP_TAG_NAME, "printer-name", cfg->name);
    if (W("printer-organization"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-organization", "");
    if (W("printer-organizational-unit"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-organizational-unit", "");
    if (W("printer-resolution-default"))
        ipp_out_res(o, "printer-resolution-default", 600, 600);
    if (W("printer-resolution-supported")) {
        ipp_out_res(o, "printer-resolution-supported", 300, 300);
        ipp_out_res(o, NULL, 600, 600);
    }
    if (W("printer-state"))
        ipp_out_int(o, IPP_TAG_ENUM, "printer-state", printer_state(&st));
    if (W("printer-state-change-date-time"))
        ipp_out_date(o, "printer-state-change-date-time", s_start);
    if (W("printer-state-change-time"))
        ipp_out_int(o, IPP_TAG_INTEGER, "printer-state-change-time", 0);
    if (W("printer-state-message"))
        ipp_out_str(o, IPP_TAG_TEXT, "printer-state-message",
                    st.message[0] ? st.message : st.connected ? "Ready" : "Printer not connected");
    if (W("printer-state-reasons")) {
        if (!st.connected) {
            ipp_out_str(o, IPP_TAG_KEYWORD, "printer-state-reasons", "offline-report");
        } else if (st.nreasons == 0) {
            ipp_out_str(o, IPP_TAG_KEYWORD, "printer-state-reasons", "none");
        } else {
            for (int i = 0; i < st.nreasons; i++)
                ipp_out_str(o, IPP_TAG_KEYWORD, i == 0 ? "printer-state-reasons" : NULL, st.reasons[i]);
        }
    }
    // supplies: printer-supply (PWG 5100.13) and the older marker-* attributes
    if (st.nsupplies > 0) {
        if (W("printer-supply")) {
            for (int i = 0; i < st.nsupplies; i++) {
                char s[200];
                int n = snprintf(s, sizeof(s),
                                 "index=%d;class=supplyThatIsConsumed;type=ink;unit=percent;maxcapacity=100;level=%d;"
                                 "colorantname=%s;",
                                 i + 1, st.supply[i].level, i == 0 ? "black" : "multi-color");
                ipp_out_raw(o, IPP_TAG_STRING, i == 0 ? "printer-supply" : NULL, s, n);
            }
        }
        if (W("printer-supply-description"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_str(o, IPP_TAG_TEXT, i == 0 ? "printer-supply-description" : NULL, st.supply[i].name);
        if (W("marker-names"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_str(o, IPP_TAG_NAME, i == 0 ? "marker-names" : NULL, st.supply[i].name);
        if (W("marker-colors"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_str(o, IPP_TAG_NAME, i == 0 ? "marker-colors" : NULL, st.supply[i].color);
        if (W("marker-levels"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_int(o, IPP_TAG_INTEGER, i == 0 ? "marker-levels" : NULL, st.supply[i].level);
        if (W("marker-low-levels"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_int(o, IPP_TAG_INTEGER, i == 0 ? "marker-low-levels" : NULL, 10);
        if (W("marker-high-levels"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_int(o, IPP_TAG_INTEGER, i == 0 ? "marker-high-levels" : NULL, 100);
        if (W("marker-types"))
            for (int i = 0; i < st.nsupplies; i++)
                ipp_out_str(o, IPP_TAG_KEYWORD, i == 0 ? "marker-types" : NULL, "ink-cartridge");
    } else {
        // the attributes must exist, with matching counts
        if (W("printer-supply"))
            ipp_out_raw(o, IPP_TAG_NOVALUE, "printer-supply", NULL, 0);
        if (W("printer-supply-description"))
            ipp_out_raw(o, IPP_TAG_NOVALUE, "printer-supply-description", NULL, 0);
    }
    if (W("printer-supply-info-uri")) {
        uri_for(c, uri, sizeof(uri), "http", "/");
        ipp_out_str(o, IPP_TAG_URI, "printer-supply-info-uri", uri);
    }
    if (W("printer-up-time"))
        ipp_out_int(o, IPP_TAG_INTEGER, "printer-up-time", (int32_t)(now - s_start) + 1);
    if (W("printer-uri-supported")) {
        uri_for(c, uri, sizeof(uri), "ipp", "/ipp/print");
        ipp_out_str(o, IPP_TAG_URI, "printer-uri-supported", uri);
    }
    if (W("printer-uuid")) {
        snprintf(uri, sizeof(uri), "urn:uuid:%s", cfg->uuid);
        ipp_out_str(o, IPP_TAG_URI, "printer-uuid", uri);
    }
    if (W("pwg-raster-document-resolution-supported")) {
        ipp_out_res(o, "pwg-raster-document-resolution-supported", 300, 300);
        ipp_out_res(o, NULL, 600, 600);
    }
    if (W("pwg-raster-document-sheet-back"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", "normal");
    if (W("pwg-raster-document-type-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "pwg-raster-document-type-supported",
                     (const char *[]){"sgray_8", "srgb_8", NULL});
    if (W("queued-job-count"))
        ipp_out_int(o, IPP_TAG_INTEGER, "queued-job-count", s_active ? 1 : 0);
    if (W("sides-default"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "sides-default", "one-sided");
    if (W("sides-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "sides-supported", "one-sided");
    if (W("uri-authentication-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "uri-authentication-supported", "none");
    if (W("uri-security-supported"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "uri-security-supported", "none");
    if (W("urf-supported")) {
        // comma separated list -> 1setOf keyword
        char urf[] = AIRPRINT_URF;
        char *save = NULL;
        int i = 0;
        for (char *t = strtok_r(urf, ",", &save); t; t = strtok_r(NULL, ",", &save), i++)
            ipp_out_str(o, IPP_TAG_KEYWORD, i == 0 ? "urf-supported" : NULL, t);
    }
    if (W("which-jobs-supported"))
        ipp_out_strs(o, IPP_TAG_KEYWORD, "which-jobs-supported",
                     (const char *[]){"completed", "not-completed", "all", NULL});
#undef W
}

// ===========================================================================
// jobs (s_lock held by callers)

static const char *job_reason(const job_t *j)
{
    if (j->reason)
        return j->reason;
    switch (j->state) {
    case JOB_PENDING: return "job-incoming";
    case JOB_PROCESSING: return "job-printing";
    case JOB_CANCELED: return "job-canceled-by-user";
    case JOB_ABORTED: return "aborted-by-system";
    case JOB_COMPLETED: return "job-completed-successfully";
    default: return "none";
    }
}

static void add_job_attrs(ipp_out_t *o, const conn_t *c, const job_t *j, const filter_t *f)
{
    char uri[160], path[48];
#define W(name) (!f || want(f, name))
    if (W("job-id"))
        ipp_out_int(o, IPP_TAG_INTEGER, "job-id", j->id);
    if (W("job-uri")) {
        snprintf(path, sizeof(path), "/ipp/print/%d", j->id);
        uri_for(c, uri, sizeof(uri), "ipp", path);
        ipp_out_str(o, IPP_TAG_URI, "job-uri", uri);
    }
    if (W("job-printer-uri")) {
        uri_for(c, uri, sizeof(uri), "ipp", "/ipp/print");
        ipp_out_str(o, IPP_TAG_URI, "job-printer-uri", uri);
    }
    if (W("job-state"))
        ipp_out_int(o, IPP_TAG_ENUM, "job-state", j->state);
    if (W("job-state-reasons"))
        ipp_out_str(o, IPP_TAG_KEYWORD, "job-state-reasons", job_reason(j));
    if (f) {    // the short form (Print-Job/Create-Job responses) stops here
        if (W("job-name"))
            ipp_out_str(o, IPP_TAG_NAME, "job-name", j->name[0] ? j->name : "Untitled");
        if (W("job-originating-user-name"))
            ipp_out_str(o, IPP_TAG_NAME, "job-originating-user-name", j->user[0] ? j->user : "anonymous");
        if (W("job-impressions-completed"))
            ipp_out_int(o, IPP_TAG_INTEGER, "job-impressions-completed", j->impressions);
        if (W("job-k-octets-processed"))
            ipp_out_int(o, IPP_TAG_INTEGER, "job-k-octets-processed", (int32_t)(j->bytes / 1024));
        if (W("document-format") && j->format[0])
            ipp_out_str(o, IPP_TAG_MIMETYPE, "document-format", j->format);
        if (W("job-printer-up-time"))
            ipp_out_int(o, IPP_TAG_INTEGER, "job-printer-up-time", (int32_t)(time(NULL) - s_start) + 1);
        if (W("time-at-creation"))
            ipp_out_int(o, IPP_TAG_INTEGER, "time-at-creation", (int32_t)(j->created - s_start) + 1);
        if (W("time-at-processing")) {
            if (j->processing)
                ipp_out_int(o, IPP_TAG_INTEGER, "time-at-processing", (int32_t)(j->processing - s_start) + 1);
            else
                ipp_out_raw(o, IPP_TAG_NOVALUE, "time-at-processing", NULL, 0);
        }
        if (W("time-at-completed")) {
            if (j->completed)
                ipp_out_int(o, IPP_TAG_INTEGER, "time-at-completed", (int32_t)(j->completed - s_start) + 1);
            else
                ipp_out_raw(o, IPP_TAG_NOVALUE, "time-at-completed", NULL, 0);
        }
        if (W("date-time-at-creation"))
            ipp_out_date(o, "date-time-at-creation", j->created);
    }
#undef W
}

static job_t *job_find(int id)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (s_jobs[i].id == id && id > 0)
            return &s_jobs[i];
    return NULL;
}

static bool job_done(const job_t *j)
{
    return j->state >= JOB_CANCELED;
}

static void job_finish(job_t *j, int state)
{
    j->state = state;
    j->completed = time(NULL);
    if (s_active == j)
        s_active = NULL;
}

// Aborts a Create-Job job whose document never arrived.
static void expire_pending(void)
{
    if (s_active && s_active->state == JOB_PENDING &&
        time(NULL) - s_active->created > MULTIPLE_OP_TIMEOUT_S) {
        LOGW("job %d: no document within %d s, aborted", s_active->id, MULTIPLE_OP_TIMEOUT_S);
        s_active->reason = "aborted-by-system";
        job_finish(s_active, JOB_ABORTED);
    }
}

static job_t *job_create(const ipp_request_t *req)
{
    job_t *j = &s_jobs[(s_next_id - 1) % MAX_JOBS];
    memset(j, 0, sizeof(*j));
    j->id = s_next_id++;
    j->state = JOB_PENDING;
    j->created = time(NULL);
    ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "job-name"), j->name, sizeof(j->name));
    ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "requesting-user-name"), j->user, sizeof(j->user));
    s_active = j;
    return j;
}

// ===========================================================================
// print pipeline: raster -> bjl

typedef struct {
    job_t *job;
    int64_t out_us;             // time spent in the printer output hook
    int64_t render_us;          // time spent converting lines (render + dither + compress)
    bjl_job_t bjl;
    bjl_render_t render;
    bool started;
    int dpi;
    int err;                    // first failure, an IPP status
} pipe_t;

static int bjl_sink(void *user, const uint8_t *data, size_t len)
{
    pipe_t *p = user;
    int64_t t0 = now_us();
    int r = s_cfg->job_write(s_cfg->ctx, data, len) < 0 ? -1 : 0;
    p->out_us += now_us() - t0;
    return r;
}

static int pipe_page_begin(void *user, const raster_page_t *pg)
{
    pipe_t *p = user;
    if (p->job->cancel)
        return 1;
    if (pg->dpi != 300 && pg->dpi != 600) {
        LOGW("page %d: unsupported resolution %d dpi", pg->page, pg->dpi);
        p->err = IPP_DOCUMENT_FORMAT_ERROR;
        return 2;
    }
    if (!p->started) {
        // paper from the page size: Letter is 11 in tall, A4 11.69 in
        float h_in = (float)pg->height / pg->dpi;
        bjl_paper_id_t paper = (h_in < 11.35f) ? BJL_PAPER_LETTER : BJL_PAPER_A4;
        bjl_mode_id_t mode = pg->dpi == 300 ? BJL_MODE_300DPI : BJL_MODE_600DPI;
        if (bjl_job_init(&p->bjl, mode, paper, bjl_sink, p) || bjl_render_init(&p->render, &p->bjl)) {
            p->err = IPP_INTERNAL_ERROR;
            return 3;
        }
        p->started = true;
        p->dpi = pg->dpi;
        LOGI("job %d: %s %s, %s", p->job->id, bjl_get_paper(paper)->name, p->bjl.mode->name,
             pg->bytes_pp == 3 ? "color" : "gray");
        bjl_emit_start_job(&p->bjl);
        if (s_cfg->job_sync && s_cfg->job_sync(s_cfg->ctx, "StartJob"))
            p->bjl.err = -1;
        bjl_emit_mode_shift(&p->bjl);
        if (!p->bjl.err && s_cfg->job_sync && s_cfg->job_sync(s_cfg->ctx, "ModeShift"))
            p->bjl.err = -1;
    } else if (pg->dpi != p->dpi) {
        LOGW("page %d: resolution changed within the job", pg->page);
        p->err = IPP_DOCUMENT_FORMAT_ERROR;
        return 2;
    }
    bjl_page_begin(&p->bjl);
    bjl_render_page(&p->render, pg->width, pg->height, pg->bytes_pp);
    return p->bjl.err ? 4 : 0;
}

static int pipe_line(void *user, int y, const uint8_t *px)
{
    pipe_t *p = user;
    if (p->job->cancel)
        return 1;
#ifdef ESP_PLATFORM
    if ((y & 63) == 63)
        vTaskDelay(1);      // let the idle task run (task watchdog)
#endif
    int64_t t0 = now_us();
    int r = bjl_render_line(&p->render, y, px) ? 4 : 0;
    p->render_us += now_us() - t0;
    return r;
}

static int pipe_page_end(void *user, const raster_page_t *pg)
{
    pipe_t *p = user;
    if (bjl_page_end(&p->bjl))
        return 4;
    pthread_mutex_lock(&s_lock);
    p->job->impressions++;
    pthread_mutex_unlock(&s_lock);
    LOGI("job %d: page %d done (%u bytes so far)", p->job->id, pg->page, (unsigned)p->bjl.bytes_out);
    return 0;
}

// Prints the document in the request body. Returns an IPP status.
static int print_document(conn_t *c, job_t *j)
{
    if (s_cfg->job_open && s_cfg->job_open(s_cfg->ctx) != 0) {
        pthread_mutex_lock(&s_lock);
        j->reason = "printer-stopped";
        job_finish(j, JOB_ABORTED);
        pthread_mutex_unlock(&s_lock);
        return IPP_BUSY;
    }
    pthread_mutex_lock(&s_lock);
    j->state = JOB_PROCESSING;
    j->processing = time(NULL);
    pthread_mutex_unlock(&s_lock);
    if (s_cfg->job_state)
        s_cfg->job_state(s_cfg->ctx, true);
    LOGI("job %d: printing \"%s\" (%s) from %s", j->id, j->name, j->format, j->user);

    pipe_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (s_cfg->job_close)
            s_cfg->job_close(s_cfg->ctx, false);
        if (s_cfg->job_state)
            s_cfg->job_state(s_cfg->ctx, false);
        pthread_mutex_lock(&s_lock);
        job_finish(j, JOB_ABORTED);
        pthread_mutex_unlock(&s_lock);
        return IPP_INTERNAL_ERROR;
    }
    p->job = j;
    doc_reader_t rd = {c, j, 0};
    raster_cb_t cb = {pipe_page_begin, pipe_line, pipe_page_end};
    int64_t t_start = now_us();
    uint64_t prof[4];
    bjl_render_profile(prof, true);
    int r = raster_decode(doc_read, &rd, &cb, p);
    bjl_render_profile(prof, false);
#ifdef ESP_PLATFORM
    LOGI("job %d: render cycles: convert %.2f s, dither %.2f s, compress+emit %.2f s over %llu inked lines", j->id,
         prof[0] / 240e6, prof[1] / 240e6, prof[2] / 240e6, (unsigned long long)prof[3]);
#endif
    int64_t t_total = now_us() - t_start;
    LOGI("job %d: %.1f s: %.1f s waiting for data (%.0f KB/s), %.1f s printer output, %.1f s rendering, "
         "%.1f s decoding", j->id, t_total / 1e6, rd.wait_us / 1e6,
         rd.wait_us ? j->bytes / 1.024 / (rd.wait_us / 1000.0) : 0.0, p->out_us / 1e6,
         (p->render_us - p->out_us) / 1e6, (t_total - rd.wait_us - p->render_us) / 1e6);

    bool canceled = j->cancel;
    bool ok = r > 0 && !canceled;
    bool link_ok = true;
    if (p->started) {
        // finish cleanly whatever happened (cancel, bad data, client gone):
        // eject a partly printed sheet and close the Canon job
        if (p->bjl.in_page && !p->bjl.err)
            bjl_page_end(&p->bjl);
        if (!p->bjl.err)
            bjl_emit_end_job(&p->bjl);
        if (!p->bjl.err && s_cfg->job_sync)
            s_cfg->job_sync(s_cfg->ctx, "EndJob");
        link_ok = !p->bjl.err;
        if (!link_ok)
            ok = false;
        bjl_render_free(&p->render);
        bjl_job_free(&p->bjl);
    }
    // ok = false makes the platform reset the printer
    if (s_cfg->job_close)
        s_cfg->job_close(s_cfg->ctx, link_ok);
    if (s_cfg->job_state)
        s_cfg->job_state(s_cfg->ctx, false);

    int status = IPP_OK;
    pthread_mutex_lock(&s_lock);
    if (canceled) {
        job_finish(j, JOB_CANCELED);
    } else if (ok) {
        job_finish(j, JOB_COMPLETED);
    } else {
        if (r == 0) {
            j->reason = "document-format-error";
            status = IPP_DOCUMENT_FORMAT_ERROR;
        } else if (r < 0) {
            bool bad_data = r == RASTER_ERR_UNSUPPORTED || r == RASTER_ERR_FORMAT;
            j->reason = bad_data ? "document-unprintable-error" : "aborted-by-system";
            if (r == RASTER_ERR_IO)
                status = IPP_OK;    // client went away; nobody reads the status
            else if (bad_data && !p->started)
                status = IPP_DOCUMENT_FORMAT_NOT_SUPPORTED;  // not URF/PWG at all (autodetect failed)
            else
                status = IPP_DOCUMENT_FORMAT_ERROR;
        } else if (p->err) {
            j->reason = "document-unprintable-error";
            status = p->err;
        } else {
            j->reason = "printer-stopped";
            status = IPP_DEVICE_ERROR;
        }
        job_finish(j, JOB_ABORTED);
    }
    LOGI("job %d: %s, %d page(s), %lld bytes received%s%s", j->id,
         j->state == JOB_COMPLETED ? "completed" : j->state == JOB_CANCELED ? "canceled" : "aborted", j->impressions,
         (long long)j->bytes, r < 0 ? ": " : "", r < 0 ? raster_strerror(r) : "");
    pthread_mutex_unlock(&s_lock);
    free(p);
    return status;
}

// ===========================================================================
// IPP operations

static bool format_supported(const char *fmt)
{
    return !fmt[0] || !strcmp(fmt, "application/octet-stream") || !strcmp(fmt, "image/urf") ||
           !strcmp(fmt, "image/pwg-raster");
}

// Checks shared by Print-Job / Validate-Job / Create-Job. Returns an IPP status.
static int check_job_request(const ipp_request_t *req, ipp_out_t *unsup)
{
    char fmt[64];
    ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "document-format"), fmt, sizeof(fmt));
    if (!format_supported(fmt)) {
        ipp_out_str(unsup, IPP_TAG_MIMETYPE, "document-format", fmt);
        return IPP_DOCUMENT_FORMAT_NOT_SUPPORTED;
    }
    char comp[16];
    ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "compression"), comp, sizeof(comp));
    if (comp[0] && strcmp(comp, "none") != 0) {
        ipp_out_str(unsup, IPP_TAG_KEYWORD, "compression", comp);
        return IPP_COMPRESSION_NOT_SUPPORTED;
    }
    return IPP_OK;
}

static void handle_ipp(conn_t *c)
{
    ipp_request_t *req = calloc(1, sizeof(*req));
    ipp_out_t out, unsup;
    ipp_out_init(&out);
    ipp_out_init(&unsup);
    if (!req) {
        c->keep_alive = false;
        send_http(c, "500 Internal Server Error", "text/plain", "", 0);
        return;
    }
    int status = ipp_request_read(req, body_read_exact, c);
    uint8_t major = req->major >= 1 && req->major <= 2 ? req->major : 1;
    uint8_t minor = req->major == 2 ? 0 : 1;
    if (status == IPP_BAD_REQUEST || status == IPP_INTERNAL_ERROR) {
        c->keep_alive = false;
        ipp_request_free(req);
        free(req);
        send_http(c, "400 Bad Request", "text/plain", "", 0);
        return;
    }

    // RFC 8011 4.1.x: request-id != 0, charset and language first, printer-uri present
    if (status == 0) {
        const ipp_attr_t *a0 = req->nattrs > 0 ? &req->attrs[0] : NULL;
        const ipp_attr_t *a1 = req->nattrs > 1 ? &req->attrs[1] : NULL;
        if (req->request_id == 0)
            status = IPP_BAD_REQUEST;
        else if (!a0 || a0->group != IPP_TAG_OPERATION || a0->tag != IPP_TAG_CHARSET ||
                 a0->name_len != 18 || memcmp(a0->name, "attributes-charset", 18) != 0)
            status = IPP_BAD_REQUEST;
        else if (!a1 || a1->group != IPP_TAG_OPERATION || a1->tag != IPP_TAG_LANGUAGE ||
                 a1->name_len != 27 || memcmp(a1->name, "attributes-natural-language", 27) != 0)
            status = IPP_BAD_REQUEST;
        else if (!ipp_find(req, IPP_TAG_OPERATION, "printer-uri") && !ipp_find(req, IPP_TAG_OPERATION, "job-uri"))
            status = IPP_BAD_REQUEST;
    }

    char fmt[64];
    ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "document-format"), fmt, sizeof(fmt));
    ipp_out_t body;     // operation results (job / printer groups)
    ipp_out_init(&body);
    const char *message = NULL;

    if (status == 0) {
        airprint_status_t st;
        filter_t f;
        filter_init(&f, req);
        int job_id = ipp_attr_int(ipp_find(req, IPP_TAG_OPERATION, "job-id"), 0);
        if (!job_id) {      // job-uri form: ipp://host/ipp/print/<id>
            char juri[160];
            ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "job-uri"), juri, sizeof(juri));
            const char *slash = strrchr(juri, '/');
            if (slash)
                job_id = atoi(slash + 1);
        }
        job_t *j = NULL;

        switch (req->op) {
        case IPP_OP_GET_PRINTER_ATTRIBUTES:
            pthread_mutex_lock(&s_lock);
            ipp_out_group(&body, IPP_TAG_PRINTER);
            add_printer_attrs(&body, c, &f);
            pthread_mutex_unlock(&s_lock);
            break;

        case IPP_OP_VALIDATE_JOB:
            status = check_job_request(req, &unsup);
            break;

        case IPP_OP_PRINT_JOB:
        case IPP_OP_CREATE_JOB:
            status = check_job_request(req, &unsup);
            if (status)
                break;
            get_status(&st);
            if (!st.connected) {
                status = IPP_NOT_ACCEPTING_JOBS;
                message = "Printer not connected";
                break;
            }
            pthread_mutex_lock(&s_lock);
            expire_pending();
            if (s_active || st.busy) {
                pthread_mutex_unlock(&s_lock);
                status = IPP_BUSY;
                message = "Printer is busy";
                break;
            }
            j = job_create(req);
            snprintf(j->format, sizeof(j->format), "%s", fmt[0] ? fmt : "application/octet-stream");
            pthread_mutex_unlock(&s_lock);
            if (req->op == IPP_OP_PRINT_JOB)
                status = print_document(c, j);
            pthread_mutex_lock(&s_lock);
            ipp_out_group(&body, IPP_TAG_JOB);
            add_job_attrs(&body, c, j, NULL);
            pthread_mutex_unlock(&s_lock);
            break;

        case IPP_OP_SEND_DOCUMENT:
            pthread_mutex_lock(&s_lock);
            j = job_find(job_id);
            if (!ipp_find(req, IPP_TAG_OPERATION, "last-document")) {
                status = IPP_BAD_REQUEST;
                message = "Missing last-document";
                j = NULL;
            } else if (!j) {
                status = IPP_NOT_FOUND;
            } else if (j->state != JOB_PENDING) {
                status = IPP_NOT_POSSIBLE;
                message = "Job does not accept documents";
            } else if (!format_supported(fmt)) {
                status = IPP_DOCUMENT_FORMAT_NOT_SUPPORTED;
            } else if (fmt[0]) {
                snprintf(j->format, sizeof(j->format), "%s", fmt);
            }
            pthread_mutex_unlock(&s_lock);
            if (status == IPP_OK)
                status = print_document(c, j);
            if (j) {
                pthread_mutex_lock(&s_lock);
                ipp_out_group(&body, IPP_TAG_JOB);
                add_job_attrs(&body, c, j, NULL);
                pthread_mutex_unlock(&s_lock);
            }
            break;

        case IPP_OP_CLOSE_JOB:
            pthread_mutex_lock(&s_lock);
            j = job_find(job_id);
            if (!j)
                status = IPP_NOT_FOUND;
            else if (j->state == JOB_PENDING)
                job_finish(j, JOB_COMPLETED);   // no document: nothing to print
            pthread_mutex_unlock(&s_lock);
            break;

        case IPP_OP_CANCEL_JOB:
        case IPP_OP_CANCEL_MY_JOBS:
            pthread_mutex_lock(&s_lock);
            j = req->op == IPP_OP_CANCEL_JOB ? job_find(job_id) : s_active;
            if (req->op == IPP_OP_CANCEL_JOB && !j) {
                status = IPP_NOT_FOUND;
            } else if (j && job_done(j)) {
                status = req->op == IPP_OP_CANCEL_JOB ? IPP_NOT_POSSIBLE : IPP_OK;
            } else if (j) {
                j->cancel = true;
                if (j->state == JOB_PENDING)
                    job_finish(j, JOB_CANCELED);
                else if (s_cfg->job_cancel)
                    s_cfg->job_cancel(s_cfg->ctx);
                LOGI("job %d: cancel requested", j->id);
            }
            pthread_mutex_unlock(&s_lock);
            break;

        case IPP_OP_GET_JOB_ATTRIBUTES:
            pthread_mutex_lock(&s_lock);
            j = job_find(job_id);
            if (!j) {
                status = IPP_NOT_FOUND;
            } else {
                ipp_out_group(&body, IPP_TAG_JOB);
                add_job_attrs(&body, c, j, &f);
            }
            pthread_mutex_unlock(&s_lock);
            break;

        case IPP_OP_GET_JOBS: {
            char which[24];
            ipp_attr_str(ipp_find(req, IPP_TAG_OPERATION, "which-jobs"), which, sizeof(which));
            bool completed = !strcmp(which, "completed"), all = !strcmp(which, "all");
            bool minimal = f.idx < 0;   // RFC 8011: default is job-id + job-uri only
            pthread_mutex_lock(&s_lock);
            for (int id = s_next_id - 1; id > 0 && id >= s_next_id - MAX_JOBS; id--) {
                job_t *jj = job_find(id);
                if (!jj || (!all && job_done(jj) != completed))
                    continue;
                ipp_out_group(&body, IPP_TAG_JOB);
                if (minimal) {
                    char juri[160], path[48];
                    ipp_out_int(&body, IPP_TAG_INTEGER, "job-id", jj->id);
                    snprintf(path, sizeof(path), "/ipp/print/%d", jj->id);
                    uri_for(c, juri, sizeof(juri), "ipp", path);
                    ipp_out_str(&body, IPP_TAG_URI, "job-uri", juri);
                } else {
                    add_job_attrs(&body, c, jj, &f);
                }
            }
            pthread_mutex_unlock(&s_lock);
            break;
        }

        case IPP_OP_IDENTIFY_PRINTER:
            if (s_cfg->identify)
                s_cfg->identify(s_cfg->ctx);
            break;

        default:
            status = IPP_OPERATION_NOT_SUPPORTED;
            break;
        }
    }

    // response: operation group (+ unsupported group) + results
    ipp_out_header(&out, major, minor, (uint16_t)status, req->request_id);
    ipp_out_group(&out, IPP_TAG_OPERATION);
    ipp_out_str(&out, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    ipp_out_str(&out, IPP_TAG_LANGUAGE, "attributes-natural-language", "en");
    if (message)
        ipp_out_str(&out, IPP_TAG_TEXT, "status-message", message);
    if (unsup.len) {
        ipp_out_group(&out, IPP_TAG_UNSUPPORTED_GROUP);
        ipp_out_append(&out, &unsup);
    }
    ipp_out_append(&out, &body);
    ipp_out_end(&out);

    // document data the operation did not consume (e.g. a rejected Print-Job)
    drain_body(c);
    if (out.oom || body.oom)
        send_http(c, "500 Internal Server Error", "text/plain", "", 0);
    else
        send_http(c, "200 OK", "application/ipp", out.buf, out.len);
    if (req->op != IPP_OP_GET_PRINTER_ATTRIBUTES && req->op != IPP_OP_GET_JOB_ATTRIBUTES)
        LOGI("IPP op 0x%04x -> status 0x%04x", req->op, status);
    ipp_out_free(&out);
    ipp_out_free(&unsup);
    ipp_out_free(&body);
    ipp_request_free(req);
    free(req);
}

// ===========================================================================
// live state for the web UI (events.h)

static const char *job_state_name(int state)
{
    switch (state) {
    case JOB_PENDING: return "pending";
    case JOB_HELD: return "held";
    case JOB_PROCESSING: return "processing";
    case JOB_CANCELED: return "canceled";
    case JOB_ABORTED: return "aborted";
    case JOB_COMPLETED: return "completed";
    default: return "unknown";
    }
}

#define STATUS_REFRESH_S 2      // the printer status costs a USB control transfer

int airprint_status_json(void *ctx, char *buf, size_t n)
{
    static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
    static airprint_status_t cached;
    static time_t cached_at;
    airprint_status_t st;
    time_t now = time(NULL);
    pthread_mutex_lock(&cache_lock);
    if (!cached_at || now - cached_at >= STATUS_REFRESH_S || now < cached_at) {
        get_status(&cached);
        cached_at = now;
    }
    st = cached;
    pthread_mutex_unlock(&cache_lock);
    size_t p = 0;
#define PUT(...) (p += (size_t)snprintf(buf + p, p < n ? n - p : 0, __VA_ARGS__))
#define STR(s) (p = events_json_str(buf, p, n, (s)))
    PUT("{\"name\":");
    STR(s_cfg->name);
    PUT(",\"model\":");
    STR(s_cfg->make_model);
    pthread_mutex_lock(&s_lock);
    expire_pending();
    int state = printer_state(&st);
    PUT(",\"connected\":%s,\"state\":\"%s\",\"message\":", st.connected ? "true" : "false",
        state == 5 ? "stopped" : state == 4 ? "processing" : "idle");
    STR(st.message[0] ? st.message : st.connected ? "Ready" : "Printer not connected");
    PUT(",\"reasons\":[");
    for (int i = 0; i < st.nreasons; i++) {
        PUT(i ? "," : "");
        STR(st.reasons[i]);
    }
    PUT("],\"supplies\":[");
    for (int i = 0; i < st.nsupplies; i++) {
        PUT("%s{\"name\":", i ? "," : "");
        STR(st.supply[i].name);
        PUT(",\"color\":");
        STR(st.supply[i].color);
        PUT(",\"level\":%d}", st.supply[i].level);
    }
    PUT("],\"active\":%d,\"jobs\":[", s_active ? s_active->id : 0);
    int listed = 0;
    for (int id = s_next_id - 1; id > 0 && id >= s_next_id - MAX_JOBS && listed < 6 && p < n - 300; id--) {
        const job_t *j = job_find(id);
        if (!j)
            continue;
        PUT("%s{\"id\":%d,\"name\":", listed++ ? "," : "", j->id);
        STR(j->name[0] ? j->name : "Untitled");
        PUT(",\"user\":");
        STR(j->user);
        // a time, not an age: the snapshot only changes when something happens
        PUT(",\"state\":\"%s\",\"reason\":\"%s\",\"pages\":%d,\"bytes\":%lld,\"time\":%lld}",
            job_state_name(j->state), job_reason(j), j->impressions, (long long)j->bytes,
            (long long)(j->completed ? j->completed : j->created));
    }
    pthread_mutex_unlock(&s_lock);
    PUT("]}");
#undef PUT
#undef STR
    return p < n ? (int)p : -1;
}

// ===========================================================================
// web page

static void handle_get(conn_t *c)
{
    if (!strcmp(c->path, "/icon.png")) {
        send_http(c, "200 OK", "image/png", ICON_PNG, sizeof(ICON_PNG));
        return;
    }
    if (strcmp(c->path, "/") != 0) {
        send_http(c, "404 Not Found", "text/plain", "not found", 9);
        return;
    }
    airprint_status_t st;
    get_status(&st);
    char *html = malloc(4096);
    if (!html) {
        send_http(c, "500 Internal Server Error", "text/plain", "", 0);
        return;
    }
    int n = snprintf(html, 4096,
                     "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
                     "content='width=device-width'><title>%s</title><style>body{font-family:sans-serif;"
                     "max-width:40em;margin:2em auto;padding:0 1em}td{padding:.2em 1em .2em 0}</style></head>"
                     "<body><h1>%s</h1><p>%s &middot; %s</p>%s<table>",
                     s_cfg->name, s_cfg->name, s_cfg->make_model,
                     st.connected ? (st.message[0] ? st.message : "Ready") : "Printer not connected",
                     s_cfg->home_html ? s_cfg->home_html : "");
    for (int i = 0; i < st.nsupplies && n < 3500; i++)
        n += snprintf(html + n, 4096 - n, "<tr><td>%s ink</td><td>%s%d%%</td></tr>", st.supply[i].name,
                      st.supply[i].level < 0 ? "unknown " : "", st.supply[i].level < 0 ? 0 : st.supply[i].level);
    n += snprintf(html + n, 4096 - n, "</table><h2>Recent jobs</h2><table>");
    pthread_mutex_lock(&s_lock);
    for (int id = s_next_id - 1; id > 0 && id >= s_next_id - MAX_JOBS && n < 3800; id--) {
        job_t *j = job_find(id);
        if (j)
            n += snprintf(html + n, 4096 - n, "<tr><td>#%d</td><td>%.40s</td><td>%s</td><td>%d page(s)</td></tr>",
                          j->id, j->name[0] ? j->name : "Untitled", job_reason(j), j->impressions);
    }
    pthread_mutex_unlock(&s_lock);
    n += snprintf(html + n, 4096 - n, "</table></body></html>");
    send_http(c, "200 OK", "text/html; charset=utf-8", html, n);
    free(html);
}

// ===========================================================================
// connections

static void *conn_thread(void *arg)
{
    conn_t *c = arg;
    struct timeval tv = {.tv_sec = IDLE_TIMEOUT_S};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    for (;;) {
        char line[512];
        if (read_line(c, line, sizeof(line)) <= 0)
            break;
        char version[16] = "";
        if (sscanf(line, "%7s %127s %15s", c->method, c->path, version) < 2)
            break;
        c->http10 = strcmp(version, "HTTP/1.0") == 0;
        c->keep_alive = !c->http10;
        c->chunked = false;
        c->remaining = 0;
        c->body_eof = false;
        c->in_chunks = false;
        c->host[0] = 0;
        c->if_none_match[0] = 0;
        bool expect_continue = false, has_length = false;
        int hl;
        while ((hl = read_line(c, line, sizeof(line))) > 0) {
            char *v = strchr(line, ':');
            if (!v)
                continue;
            *v++ = 0;
            while (*v == ' ')
                v++;
            if (!strcasecmp(line, "Content-Length")) {
                c->remaining = strtoll(v, NULL, 10);
                has_length = true;
            } else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(v, "chunked")) {
                c->chunked = true;
            } else if (!strcasecmp(line, "Expect") && strcasestr(v, "100-continue")) {
                expect_continue = true;
            } else if (!strcasecmp(line, "Connection")) {
                if (strcasestr(v, "close"))
                    c->keep_alive = false;
                else if (strcasestr(v, "keep-alive"))
                    c->keep_alive = true;
            } else if (!strcasecmp(line, "Host")) {
                snprintf(c->host, sizeof(c->host), "%s", v);
            } else if (!strcasecmp(line, "If-None-Match")) {
                snprintf(c->if_none_match, sizeof(c->if_none_match), "%s", v);
            }
        }
        if (hl < 0)
            break;
        if (!c->chunked && !has_length)
            c->body_eof = true;
        if (expect_continue && !c->body_eof)
            send_all(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

        bool ipp_path = !strcmp(c->path, "/") || !strncmp(c->path, "/ipp", 4);
        if (!strcmp(c->method, "POST") && (ipp_path || !s_cfg->http_handler)) {
            handle_ipp(c);
        } else if (!strcmp(c->method, "POST") || !strcmp(c->method, "DELETE")) {
            // the handler may read the body (airprint_http_read_body); the rest is dropped after
            if (!s_cfg->http_handler || !s_cfg->http_handler(s_cfg->ctx, c, c->method, c->path)) {
                drain_body(c);
                send_http(c, "404 Not Found", "text/plain", "not found", 9);
            }
            drain_body(c);
        } else if (!strcmp(c->method, "GET") || !strcmp(c->method, "HEAD")) {
            drain_body(c);
            if (!s_cfg->http_handler || !s_cfg->http_handler(s_cfg->ctx, c, c->method, c->path))
                handle_get(c);
        } else {
            drain_body(c);
            c->keep_alive = false;
            send_http(c, "501 Not Implemented", "text/plain", "", 0);
        }
        if (!c->keep_alive)
            break;
    }
    if (!c->detached)
        close(c->fd);
    free(c);
    pthread_mutex_lock(&s_lock);
    s_connections--;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

static void *listen_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 4)) {
        LOGW("cannot listen on port %d (errno %d)", port, errno);
        return NULL;
    }
    LOGI("%s server listening on port %d", port == s_cfg->port ? "IPP" : "web", port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&s_lock);
        bool full = s_connections >= MAX_CONNECTIONS;
        if (!full)
            s_connections++;
        pthread_mutex_unlock(&s_lock);
        if (full)
            LOGW("too many connections (%d), refusing one", MAX_CONNECTIONS);
        conn_t *c = full ? NULL : calloc(1, sizeof(*c));
        if (!c) {
            if (!full) {
                pthread_mutex_lock(&s_lock);
                s_connections--;
                pthread_mutex_unlock(&s_lock);
            }
            close(fd);
            continue;
        }
        c->fd = fd;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, CONN_STACK_SIZE);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t th;
        if (pthread_create(&th, &attr, conn_thread, c) != 0) {
            close(fd);
            free(c);
            pthread_mutex_lock(&s_lock);
            s_connections--;
            pthread_mutex_unlock(&s_lock);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

int airprint_start(const airprint_config_t *cfg)
{
    s_cfg = cfg;
    s_start = time(NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, LISTEN_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int r = pthread_create(&th, &attr, listen_thread, (void *)(intptr_t)cfg->port);
    if (r == 0 && cfg->web_port)
        r = pthread_create(&th, &attr, listen_thread, (void *)(intptr_t)cfg->web_port);
    pthread_attr_destroy(&attr);
    return r;
}
