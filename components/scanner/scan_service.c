// Scan jobs: a thread runs the scanner and the JPEG encoder; the encoder
// writes into the shared buffer, as a ring drained by the HTTP connection
// (stream mode) or as a whole file kept for later (last mode). A kept scan
// can be read while it is scanned (it only grows), so every browser can show
// it, whoever started it.
#include "scan_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "events.h"
#include "jpeg_enc.h"
#include "shared_buf.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define LOGI(...) ESP_LOGI("scan", __VA_ARGS__)
#define LOGW(...) ESP_LOGW("scan", __VA_ARGS__)
#define THREAD_STACK (8 * 1024)
#else
#define LOGI(fmt, ...) fprintf(stderr, "I scan: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "W scan: " fmt "\n", ##__VA_ARGS__)
#define THREAD_STACK (512 * 1024)    // host build runs with sanitizers
#endif

#define QUALITY 85
#define LAST_DPI 150            // kept scans without a dpi
#define BUTTON_DPI 150          // the device's button: an A4 page at 150 dpi
#define RECENT_S 600            // a scan this recent is "recent" in the status
#define STALL_TIMEOUT_S 60      // stream client not reading: cancel the scan
#define SEND_PIECE 16384

typedef enum { MODE_STREAM, MODE_LAST } scan_mode_t;
typedef enum { ST_IDLE, ST_SCANNING, ST_DONE, ST_FAILED, ST_CANCELED } scan_state_t;

static const char *const STATE_NAMES[] = {"idle", "scanning", "done", "failed", "canceled"};

// paper sizes in 1/600 inch (as canon_bjl); "card": the corner of the glass
// where an ID card goes (85.6 x 54 mm, with room to spare), a quick scan
static const struct {
    const char *name;
    int w, h;
} PAPERS[] = {{"a4", 4961, 7016}, {"letter", 5100, 6600}, {"card", 2400, 1700}};

// a scan area: dpi, and offset and size in pixels at that dpi
typedef struct {
    int dpi, x, y, w, h;
    int paper;                  // index in PAPERS, -1 for any other area (eSCL)
} scan_area_t;

static const char *paper_name(int i)
{
    return i < 0 ? "custom" : PAPERS[i].name;
}

static const scan_config_t *s_cfg;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;   // any change below

static struct {
    scan_state_t state;
    bool running;               // thread alive: holds the buffer (and the device until closed)
    bool producing;             // the encoder may still write
    scan_mode_t mode;
    int dpi, paper, x, y, width, height;
    volatile int lines;
    volatile bool cancel;
    char error[64];
    uint8_t *buf;
    size_t size;
    uint64_t wpos, rpos;        // bytes written by the encoder / sent to the client
    bool consumer;              // stream client attached
    bool discard;               // nobody reads the rest
    bool overflow;
    int64_t t_start, t_end, enc_us;
    int last_paper;             // paper of the kept scan
    int last_dpi;
    char id[24];                // this scan: "<boot>-<n>", unique across boots
    char last_id[24];           // the kept scan
    const char *source;         // who started it: "web", "stream", "button", "escl"
    int readers;                // connections reading the kept scan while it is written
    bool released;              // the buffer was given back (kept or not)
    unsigned seq;
    uint32_t boot;
} S;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// waits on s_cond (s_lock held) for at most ms
static void wait_ms(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&s_cond, &s_lock, &ts);
}

// ---------------------------------------------------------------------------
// scan thread

// encoder output
static int sink(void *user, const uint8_t *data, size_t len)
{
    int r = 0;
    pthread_mutex_lock(&s_lock);
    if (S.mode == MODE_LAST) {
        if (S.wpos + len > S.size) {
            S.overflow = true;
            r = -1;
        } else {
            memcpy(S.buf + S.wpos, data, len);
            S.wpos += len;
        }
    } else {
        int64_t stalled = 0;
        while (len && !S.discard) {
            size_t space = S.size - (size_t)(S.wpos - S.rpos);
            if (!space) {
                // ring full: the client is behind; the scanner pauses meanwhile
                if (!stalled) {
                    stalled = now_us();
                } else if (now_us() - stalled > STALL_TIMEOUT_S * 1000000LL) {
                    LOGW("client stopped reading for %d s, canceling", STALL_TIMEOUT_S);
                    S.discard = true;
                    S.cancel = true;
                    break;
                }
                wait_ms(1000);
                continue;
            }
            stalled = 0;
            size_t off = (size_t)(S.wpos % S.size), k = len;
            if (k > space)
                k = space;
            if (k > S.size - off)
                k = S.size - off;
            memcpy(S.buf + off, data, k);
            S.wpos += k;
            data += k;
            len -= k;
            pthread_cond_broadcast(&s_cond);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

static int on_line(void *user, int y, const uint8_t *rgb)
{
    int64_t t0 = now_us();
    int r = jpeg_enc_add_line(user, rgb);
    S.enc_us += now_us() - t0;
    S.lines = y + 1;
#ifdef ESP_PLATFORM
    if ((y & 63) == 63)
        vTaskDelay(1);      // let the idle task run (task watchdog)
#endif
    return r < 0 || S.cancel;
}

static void *scan_thread(void *arg)
{
    if (s_cfg->state)
        s_cfg->state(s_cfg->ctx, true);
    LOGI("%s scan: %s, %d dpi, %dx%d at (%d,%d)", S.mode == MODE_LAST ? "kept" : "streamed", paper_name(S.paper),
         S.dpi, S.width, S.height, S.x, S.y);
    int err = PIXMA_ENOMEM;
    jpeg_enc_t *e = jpeg_enc_create(S.width, S.height, QUALITY, S.dpi, sink, NULL);
    if (e) {
        pixma_params_t p = {.dpi = S.dpi, .x = S.x, .y = S.y, .w = S.width, .h = S.height, .calibrate = true};
        err = pixma_scan(&s_cfg->io, &p, on_line, e, &S.cancel);
    }
    s_cfg->close(s_cfg->ctx);       // printing may go on while the stream drains
    if (s_cfg->state)
        s_cfg->state(s_cfg->ctx, false);

    // a stream always ends with a complete JPEG (missing lines white), unless nobody reads it
    bool ok = err == PIXMA_OK && !S.overflow;
    if (e && (ok || (S.mode == MODE_STREAM && !S.discard)) && jpeg_enc_finish(e) < 0)
        ok = false;
    int lines = e ? jpeg_enc_lines(e) : 0;
    jpeg_enc_free(e);

    pthread_mutex_lock(&s_lock);
    S.producing = false;
    S.t_end = now_us();
    S.state = ok ? ST_DONE : S.cancel ? ST_CANCELED : ST_FAILED;
    snprintf(S.error, sizeof(S.error), "%s",
             ok ? "" : S.overflow ? "the image does not fit in memory" : pixma_strerror(err));
    pthread_cond_broadcast(&s_cond);
    while (S.consumer || S.readers)     // clients still read from the buffer
        pthread_cond_wait(&s_cond, &s_lock);
    size_t keep = S.mode == MODE_LAST && ok ? (size_t)S.wpos : 0;
    if (keep) {
        S.last_paper = S.paper;
        S.last_dpi = S.dpi;
        memcpy(S.last_id, S.id, sizeof(S.last_id));
    }
    uint64_t bytes = S.wpos;
    sbuf_release(SBUF_SCAN, keep);
    S.released = true;          // from now on readers go through sbuf_read_begin
    pthread_mutex_unlock(&s_lock);
    events_poke();

    double secs = (S.t_end - S.t_start) / 1e6;
    LOGI("scan %s: %d lines, %llu bytes of JPEG, %.1f s (%.1f s encoding)%s%s", STATE_NAMES[S.state], lines,
         (unsigned long long)bytes, secs, S.enc_us / 1e6, ok ? "" : ": ", S.error);
    pthread_mutex_lock(&s_lock);
    S.running = false;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

// Starts a scan. Returns 0, or fills `why` and returns an HTTP status.
static int start_scan(scan_mode_t mode, const scan_area_t *a, const char *source, const char **why)
{
    pthread_mutex_lock(&s_lock);
    int code = 503;
    uint8_t *buf = NULL;
    size_t size = 0;
    if (S.running) {
        *why = "A scan is already running";
        code = 409;
    } else if (s_cfg->available && !s_cfg->available(s_cfg->ctx)) {
        *why = "No scanner connected";
    } else if (!(buf = sbuf_acquire(SBUF_SCAN, &size))) {
        *why = "The memory is in use (a print job or a download)";
    } else if (s_cfg->open(s_cfg->ctx) != 0) {
        sbuf_release(SBUF_SCAN, 0);
        *why = "The printer is busy";
    } else {
        code = 0;
    }
    if (code) {
        pthread_mutex_unlock(&s_lock);
        return code;
    }
    S.last_id[0] = 0;           // taking the buffer dropped the kept scan
    S.state = ST_SCANNING;
    S.mode = mode;
    S.source = source;
    snprintf(S.id, sizeof(S.id), "%08lx-%u", (unsigned long)S.boot, ++S.seq);
    S.dpi = a->dpi;
    S.paper = a->paper;
    S.x = a->x;
    S.y = a->y;
    S.width = a->w;
    S.height = a->h;
    S.lines = 0;
    S.cancel = false;
    S.error[0] = 0;
    S.buf = buf;
    S.size = size;
    S.wpos = S.rpos = 0;
    S.released = false;
    S.consumer = mode == MODE_STREAM;
    S.discard = false;
    S.overflow = false;
    S.t_start = now_us();
    S.t_end = 0;
    S.enc_us = 0;
    S.running = S.producing = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int r = pthread_create(&th, &attr, scan_thread, NULL);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        s_cfg->close(s_cfg->ctx);
        sbuf_release(SBUF_SCAN, 0);
        S.running = S.producing = S.consumer = false;
        S.state = ST_FAILED;
        snprintf(S.error, sizeof(S.error), "cannot start the scan thread");
        *why = "Cannot start the scan";
        pthread_mutex_unlock(&s_lock);
        return 500;
    }
    pthread_mutex_unlock(&s_lock);
    events_poke();
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP

static const char *status_line(int code)
{
    return code == 400 ? "400 Bad Request" : code == 404 ? "404 Not Found" : code == 405 ? "405 Method Not Allowed"
         : code == 409 ? "409 Conflict" : code == 500 ? "500 Internal Server Error" : "503 Service Unavailable";
}

static void send_text(airprint_http_t *h, int code, const char *text)
{
    airprint_http_send(h, status_line(code), "text/plain; charset=utf-8", text, strlen(text));
}

// value of key in a query string ("a=1&b=2"), "" if absent
static const char *query_get(const char *query, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = query; p && *p;) {
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = len - kl - 1 < n - 1 ? len - kl - 1 : n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            break;
        }
        p = end ? end + 1 : NULL;
    }
    return out;
}

static int parse_paper(const char *query)
{
    char v[16];
    query_get(query, "paper", v, sizeof(v));
    for (int i = 0; i < (int)(sizeof(PAPERS) / sizeof(PAPERS[0])); i++)
        if (!strcmp(v, PAPERS[i].name))
            return i;
    return v[0] ? -1 : 0;
}

static scan_area_t paper_area(int paper, int dpi)
{
    return (scan_area_t){.dpi = dpi, .w = PAPERS[paper].w * dpi / 600, .h = PAPERS[paper].h * dpi / 600,
                         .paper = paper};
}

// Sends the running stream scan to the client (the connection is the ring's
// consumer). Returns the scan's final state.
static scan_state_t send_stream(airprint_http_t *h, const char *type)
{
    bool client_ok = airprint_http_stream_begin(h, type, "Cache-Control: no-store\r\n") == 0;
    pthread_mutex_lock(&s_lock);
    while (client_ok) {
        while (S.rpos == S.wpos && S.producing)
            wait_ms(1000);
        if (S.rpos == S.wpos)
            break;      // all sent
        size_t off = (size_t)(S.rpos % S.size), n = (size_t)(S.wpos - S.rpos);
        if (n > S.size - off)
            n = S.size - off;
        if (n > SEND_PIECE)
            n = SEND_PIECE;
        const uint8_t *p = S.buf + off;
        pthread_mutex_unlock(&s_lock);
        client_ok = airprint_http_stream_write(h, p, n) == 0;
        pthread_mutex_lock(&s_lock);
        if (client_ok)
            S.rpos += n;
        pthread_cond_broadcast(&s_cond);
    }
    if (!client_ok) {
        if (S.producing)
            LOGW("stream client gone, canceling the scan");
        S.cancel = true;
        S.discard = true;
    }
    S.consumer = false;
    pthread_cond_broadcast(&s_cond);
    while (S.producing)     // the outcome, also after a cancel
        pthread_cond_wait(&s_cond, &s_lock);
    scan_state_t st = client_ok ? S.state : ST_CANCELED;
    pthread_mutex_unlock(&s_lock);
    if (client_ok)
        airprint_http_stream_end(h);
    return st;
}

static void handle_stream(airprint_http_t *h, const char *query)
{
    char v[16];
    int dpi = atoi(query_get(query, "dpi", v, sizeof(v)));
    if (!v[0])
        dpi = 300;
    int paper = parse_paper(query);
    if ((dpi != 150 && dpi != 300 && dpi != 600) || paper < 0) {
        send_text(h, 400, "dpi must be 150, 300 or 600, paper a4, letter or card");
        return;
    }
    const char *why;
    scan_area_t a = paper_area(paper, dpi);
    int code = start_scan(MODE_STREAM, &a, "stream", &why);
    if (code) {
        send_text(h, code, why);
        return;
    }
    send_stream(h, "image/jpeg");
}

static int start_kept(int paper, int dpi, const char *source, const char **why)
{
    scan_area_t a = paper_area(paper, dpi);
    return start_scan(MODE_LAST, &a, source, why);
}

static void handle_last(airprint_http_t *h, const char *query)
{
    char v[16];
    int dpi = atoi(query_get(query, "dpi", v, sizeof(v)));
    if (!v[0])
        dpi = LAST_DPI;
    int paper = parse_paper(query);
    if ((dpi != 75 && dpi != 150 && dpi != 300 && dpi != 600) || paper < 0) {
        send_text(h, 400, "dpi must be 75, 150, 300 or 600, paper a4, letter or card");
        return;
    }
    const char *why;
    int code = start_kept(paper, dpi, "web", &why);
    if (code) {
        send_text(h, code, why);
        return;
    }
    pthread_mutex_lock(&s_lock);
    char js[64];
    int n = snprintf(js, sizeof(js), "{\"ok\":true,\"id\":\"%s\"}", S.id);
    pthread_mutex_unlock(&s_lock);
    airprint_http_send(h, "202 Accepted", "application/json", js, (size_t)n);
}

// GET /scan/last.jpg[?id=<scan id>][&from=<offset>]: the kept scan from
// `from` on. While it is scanned: what was encoded so far (possibly nothing),
// X-Scan-State tells whether more follows. id: 404 unless that scan is the
// one kept (or being kept).
static void handle_last_jpg(airprint_http_t *h, const char *query)
{
    char id[24], v[16];
    query_get(query, "id", id, sizeof(id));
    long long from = atoll(query_get(query, "from", v, sizeof(v)));
    if (from < 0)
        from = 0;
    char hdr[96];
    pthread_mutex_lock(&s_lock);
    if (S.running && !S.released && S.mode == MODE_LAST && (!id[0] || !strcmp(id, S.id))) {
        // the scan thread keeps the buffer until the last reader is done
        S.readers++;
        uint64_t end = S.wpos;
        bool more = S.producing;
        snprintf(hdr, sizeof(hdr), "Cache-Control: no-store\r\nX-Scan-Id: %s\r\nX-Scan-State: %s\r\n", S.id,
                 more ? "scanning" : STATE_NAMES[S.state]);
        const uint8_t *p = S.buf;
        pthread_mutex_unlock(&s_lock);
        size_t start = (uint64_t)from < end ? (size_t)from : (size_t)end;
        airprint_http_send_ex(h, "200 OK", "image/jpeg", hdr, p + start, (size_t)(end - start));
        pthread_mutex_lock(&s_lock);
        S.readers--;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
        return;
    }
    bool match = !id[0] || !strcmp(id, S.last_id);
    snprintf(hdr, sizeof(hdr), "Cache-Control: no-store\r\nX-Scan-Id: %s\r\nX-Scan-State: done\r\n", S.last_id);
    pthread_mutex_unlock(&s_lock);
    size_t len;
    const uint8_t *p = match ? sbuf_read_begin(SBUF_SCAN, &len) : NULL;
    if (!p) {
        send_text(h, 404, "No scan is kept on the printer");
        return;
    }
    size_t start = (size_t)from < len ? (size_t)from : len;
    airprint_http_send_ex(h, "200 OK", "image/jpeg", hdr, p + start, len - start);
    sbuf_read_end();
}

int scan_service_status_json(void *ctx, char *js, size_t size)
{
    bool scanner = !s_cfg || !s_cfg->available || s_cfg->available(s_cfg->ctx);
    size_t last = sbuf_content(SBUF_SCAN);
    pthread_mutex_lock(&s_lock);
    int64_t now = now_us(), end = S.t_end ? S.t_end : now;
    int n = snprintf(js, size,
                     "{\"scanner\":%s,\"state\":\"%s\",\"mode\":\"%s\",\"id\":\"%s\",\"source\":\"%s\",\"dpi\":%d,"
                     "\"paper\":\"%s\",\"width\":%d,\"height\":%d,\"lines\":%d,\"bytes\":%llu,\"elapsed\":%.1f,"
                     "\"recent\":%s,\"error\":\"%s\",\"last\":%u,\"last_id\":\"%s\",\"last_paper\":\"%s\","
                     "\"last_dpi\":%d,\"button\":{\"dpi\":%d,\"paper\":\"%s\"}}",
                     scanner ? "true" : "false", STATE_NAMES[S.state], S.mode == MODE_LAST ? "last" : "stream", S.id,
                     S.source ? S.source : "", S.dpi, paper_name(S.paper), S.width, S.height, S.lines,
                     (unsigned long long)S.wpos, S.t_start ? (end - S.t_start) / 1e6 : 0.0,
                     S.t_end && !S.producing && now - S.t_end < RECENT_S * 1000000LL ? "true" : "false", S.error,
                     (unsigned)(S.last_id[0] ? last : 0), S.last_id, paper_name(S.last_paper), S.last_dpi,
                     BUTTON_DPI, paper_name(0));
    pthread_mutex_unlock(&s_lock);
    return n < (int)size ? n : -1;
}

static void handle_status(airprint_http_t *h)
{
    char js[640];
    int n = scan_service_status_json(NULL, js, sizeof(js));
    airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", js, n > 0 ? (size_t)n : 0);
}

int scan_service_start_button(const char **why)
{
    if (!s_cfg) {
        *why = "scanning is not set up (no WiFi?)";
        return 503;
    }
    int dpi = BUTTON_DPI, paper = 0;    // A4
    *why = "";
    int code = start_kept(paper, dpi, "button", why);
    if (!code)
        LOGI("button: kept scan, %s, %d dpi", paper_name(paper), dpi);
    return code;
}

// ---------------------------------------------------------------------------
// eSCL

#define ESCL_JOBS 4
#define ESCL_NS "xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" " \
                "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\""
// scan area in eSCL units (1/300 inch)
#define ESCL_MAX_W (PIXMA_MAX_WIDTH_75 * 4)
#define ESCL_MAX_H (PIXMA_MAX_HEIGHT_75 * 4)
#define ESCL_MIN_SIZE 16

typedef enum { JOB_PENDING, JOB_PROCESSING, JOB_COMPLETED, JOB_CANCELED, JOB_ABORTED } escl_state_t;
static const char *const JOB_STATES[] = {"Pending", "Processing", "Completed", "Canceled", "Aborted"};
static const char *const JOB_REASONS[] = {"JobQueued", "JobScanning", "JobCompletedSuccessfully",
                                          "JobCanceledByUser", "AbortedBySystem"};

typedef struct {
    bool used;
    char id[40];
    escl_state_t state;
    scan_area_t area;
    time_t created;
} escl_job_t;

static escl_job_t s_jobs[ESCL_JOBS];    // under s_lock
static unsigned s_job_seq;

static escl_job_t *job_find(const char *id)
{
    for (int i = 0; i < ESCL_JOBS; i++)
        if (s_jobs[i].used && !strcmp(s_jobs[i].id, id))
            return &s_jobs[i];
    return NULL;
}

// Text of the first <name> or <prefix:name> element, "" if none.
static const char *xml_get(const char *xml, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = strstr(xml, name); p; p = strstr(p + 1, name)) {
        if (p[nl] != '>' || p == xml)
            continue;
        const char *q = p - 1;
        if (*q == ':') {        // namespace prefix
            while (q > xml && q[-1] != '<' && q[-1] != '/' && q[-1] != '>' && q[-1] != ' ')
                q--;
            q--;
        }
        if (q < xml || *q != '<')
            continue;
        const char *v = p + nl + 1, *e = strchr(v, '<');
        size_t vl = e ? (size_t)(e - v) : strlen(v);
        if (vl >= n)
            vl = n - 1;
        memcpy(out, v, vl);
        out[vl] = 0;
        return out;
    }
    return out;
}

static int xml_int(const char *xml, const char *name, int def)
{
    char v[24];
    xml_get(xml, name, v, sizeof(v));
    return v[0] ? atoi(v) : def;
}

// "http://<Host header>"
static void base_url(airprint_http_t *h, char *out, size_t n)
{
    const char *host = airprint_http_host(h);
    snprintf(out, n, "http://%s", host[0] ? host : "localhost");
}

static void send_xml(airprint_http_t *h, char *xml, int len)
{
    airprint_http_send(h, "200 OK", "text/xml", xml, (size_t)len);
}

static void escl_capabilities(airprint_http_t *h)
{
    char base[128];
    base_url(h, base, sizeof(base));
    char *x = malloc(4096);
    if (!x) {
        send_text(h, 500, "no memory");
        return;
    }
    int n = snprintf(x, 4096,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<scan:ScannerCapabilities " ESCL_NS ">\r\n"
        "<pwg:Version>2.0</pwg:Version>\r\n"
        "<pwg:MakeAndModel>%s</pwg:MakeAndModel>\r\n"
        "<pwg:SerialNumber>%s</pwg:SerialNumber>\r\n"
        "<scan:UUID>%s</scan:UUID>\r\n"
        "<scan:AdminURI>%s/</scan:AdminURI>\r\n"
        "<scan:IconURI>%s/icon.png</scan:IconURI>\r\n"
        "<scan:Platen><scan:PlatenInputCaps>\r\n"
        "<scan:MinWidth>%d</scan:MinWidth><scan:MaxWidth>%d</scan:MaxWidth>\r\n"
        "<scan:MinHeight>%d</scan:MinHeight><scan:MaxHeight>%d</scan:MaxHeight>\r\n"
        "<scan:MaxPhysicalWidth>%d</scan:MaxPhysicalWidth><scan:MaxPhysicalHeight>%d</scan:MaxPhysicalHeight>\r\n"
        "<scan:MaxScanRegions>1</scan:MaxScanRegions>\r\n"
        "<scan:SettingProfiles><scan:SettingProfile name=\"0\">\r\n"
        "<scan:ColorModes><scan:ColorMode>RGB24</scan:ColorMode></scan:ColorModes>\r\n"
        "<scan:ColorSpaces><scan:ColorSpace>RGB</scan:ColorSpace></scan:ColorSpaces>\r\n"
        "<scan:SupportedResolutions><scan:DiscreteResolutions>\r\n",
        s_cfg->make_model ? s_cfg->make_model : "Scanner", s_cfg->uuid ? s_cfg->uuid : "1",
        s_cfg->uuid ? s_cfg->uuid : "", base, base, ESCL_MIN_SIZE, ESCL_MAX_W, ESCL_MIN_SIZE, ESCL_MAX_H, ESCL_MAX_W,
        ESCL_MAX_H);
    for (int dpi = 75; dpi <= PIXMA_MAX_DPI; dpi *= 2)
        n += snprintf(x + n, 4096 - n,
                      "<scan:DiscreteResolution><scan:XResolution>%d</scan:XResolution>"
                      "<scan:YResolution>%d</scan:YResolution></scan:DiscreteResolution>\r\n", dpi, dpi);
    n += snprintf(x + n, 4096 - n,
        "</scan:DiscreteResolutions></scan:SupportedResolutions>\r\n"
        "<scan:DocumentFormats><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>"
        "<scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt></scan:DocumentFormats>\r\n"
        "</scan:SettingProfile></scan:SettingProfiles>\r\n"
        "<scan:SupportedIntents><scan:SupportedIntent>Document</scan:SupportedIntent>"
        "<scan:SupportedIntent>TextAndGraphic</scan:SupportedIntent><scan:SupportedIntent>Photo</scan:SupportedIntent>"
        "<scan:SupportedIntent>Preview</scan:SupportedIntent></scan:SupportedIntents>\r\n"
        "<scan:MaxOpticalXResolution>%d</scan:MaxOpticalXResolution>"
        "<scan:MaxOpticalYResolution>%d</scan:MaxOpticalYResolution>\r\n"
        "</scan:PlatenInputCaps></scan:Platen>\r\n"
        "</scan:ScannerCapabilities>\r\n", PIXMA_MAX_DPI, PIXMA_MAX_DPI);
    send_xml(h, x, n);
    free(x);
}

static void escl_status(airprint_http_t *h)
{
    bool scanner = !s_cfg->available || s_cfg->available(s_cfg->ctx);
    char *x = malloc(3072);
    if (!x) {
        send_text(h, 500, "no memory");
        return;
    }
    pthread_mutex_lock(&s_lock);
    int n = snprintf(x, 3072,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                     "<scan:ScannerStatus " ESCL_NS ">\r\n"
                     "<pwg:Version>2.0</pwg:Version>\r\n"
                     "<pwg:State>%s</pwg:State>\r\n<scan:Jobs>\r\n",
                     !scanner ? "Stopped" : S.running ? "Processing" : "Idle");
    time_t now = time(NULL);
    for (unsigned k = 0; k < ESCL_JOBS; k++) {     // newest first
        const escl_job_t *j = &s_jobs[(s_job_seq + ESCL_JOBS - 1 - k) % ESCL_JOBS];
        if (!j->used)
            continue;
        n += snprintf(x + n, 3072 - n,
                      "<scan:JobInfo><pwg:JobUri>/eSCL/ScanJobs/%s</pwg:JobUri><pwg:JobUuid>%s</pwg:JobUuid>"
                      "<scan:Age>%ld</scan:Age><pwg:ImagesCompleted>%d</pwg:ImagesCompleted>"
                      "<pwg:ImagesToTransfer>%d</pwg:ImagesToTransfer><pwg:JobState>%s</pwg:JobState>"
                      "<pwg:JobStateReasons><pwg:JobStateReason>%s</pwg:JobStateReason></pwg:JobStateReasons>"
                      "</scan:JobInfo>\r\n",
                      j->id, j->id, (long)(now - j->created), j->state == JOB_COMPLETED ? 1 : 0,
                      j->state <= JOB_PROCESSING ? 1 : 0, JOB_STATES[j->state], JOB_REASONS[j->state]);
    }
    pthread_mutex_unlock(&s_lock);
    n += snprintf(x + n, 3072 - n, "</scan:Jobs>\r\n</scan:ScannerStatus>\r\n");
    send_xml(h, x, n);
    free(x);
}

// scan settings XML -> area, or an HTTP status
static int parse_settings(const char *xml, scan_area_t *a)
{
    char v[48];
    xml_get(xml, "InputSource", v, sizeof(v));
    if (v[0] && strcmp(v, "Platen") != 0)
        return 409;     // no document feeder
    xml_get(xml, "ContentRegionUnits", v, sizeof(v));
    if (v[0] && !strstr(v, "ThreeHundredthsOfInches"))
        return 409;
    int res = xml_int(xml, "XResolution", 300);
    a->dpi = PIXMA_MAX_DPI;     // the lowest native resolution that is at least the requested one
    for (int d = 75; d <= PIXMA_MAX_DPI; d *= 2) {
        if (d >= res) {
            a->dpi = d;
            break;
        }
    }
    int x = xml_int(xml, "XOffset", 0), y = xml_int(xml, "YOffset", 0);
    int w = xml_int(xml, "Width", ESCL_MAX_W - x), h = xml_int(xml, "Height", ESCL_MAX_H - y);
    if (x < 0 || y < 0 || x >= ESCL_MAX_W || y >= ESCL_MAX_H || w < 1 || h < 1)
        return 409;
    if (x + w > ESCL_MAX_W)
        w = ESCL_MAX_W - x;
    if (y + h > ESCL_MAX_H)
        h = ESCL_MAX_H - y;
    // 1/300 inch -> pixels; the scanner's limits are in whole pixels at the dpi
    int max_w = PIXMA_MAX_WIDTH_75 * a->dpi / 75, max_h = PIXMA_MAX_HEIGHT_75 * a->dpi / 75;
    a->x = x * a->dpi / 300;
    a->y = y * a->dpi / 300;
    a->w = (w * a->dpi + 150) / 300;
    a->h = (h * a->dpi + 150) / 300;
    if (a->w < 1)
        a->w = 1;
    if (a->h < 1)
        a->h = 1;
    if (a->x + a->w > max_w)
        a->w = max_w - a->x;
    if (a->y + a->h > max_h)
        a->h = max_h - a->y;
    a->paper = -1;
    return 0;
}

static void escl_new_job(airprint_http_t *h)
{
    char xml[2048];
    int n = airprint_http_read_body(h, xml, sizeof(xml) - 1);
    if (n <= 0) {
        send_text(h, 400, "scan settings missing");
        return;
    }
    xml[n] = 0;
    scan_area_t a;
    int code = parse_settings(xml, &a);
    if (code) {
        send_text(h, code, "unsupported scan settings");
        return;
    }
    if (s_cfg->available && !s_cfg->available(s_cfg->ctx)) {
        send_text(h, 503, "No scanner connected");
        return;
    }
    pthread_mutex_lock(&s_lock);
    if (S.running) {
        pthread_mutex_unlock(&s_lock);
        send_text(h, 503, "A scan is already running");
        return;
    }
    escl_job_t *j = &s_jobs[s_job_seq % ESCL_JOBS];
    unsigned seq = ++s_job_seq;
    memset(j, 0, sizeof(*j));
    j->used = true;
    j->state = JOB_PENDING;
    j->area = a;
    j->created = time(NULL);
    // UUID-shaped, unique per boot (sequence) and across boots (time)
    snprintf(j->id, sizeof(j->id), "%08lx-%04x-4000-8000-%012x", (unsigned long)j->created, seq & 0xffff,
             (unsigned)(now_us() & 0xffffffff));
    char loc[96];
    snprintf(loc, sizeof(loc), "Location: /eSCL/ScanJobs/%s\r\n", j->id);
    pthread_mutex_unlock(&s_lock);
    LOGI("eSCL job %s: %d dpi, %dx%d at (%d,%d)", j->id, a.dpi, a.w, a.h, a.x, a.y);
    airprint_http_send_ex(h, "201 Created", "text/plain", loc, "", 0);
}

static void escl_next_document(airprint_http_t *h, const char *id)
{
    pthread_mutex_lock(&s_lock);
    escl_job_t *j = job_find(id);
    bool pending = j && j->state == JOB_PENDING;
    scan_area_t a = j ? j->area : (scan_area_t){0};
    pthread_mutex_unlock(&s_lock);
    if (!pending) {
        send_text(h, 404, j ? "no more documents" : "no such job");     // one page per platen job
        return;
    }
    const char *why;
    int code = start_scan(MODE_STREAM, &a, "escl", &why);
    if (code) {
        send_text(h, 503, why);
        return;
    }
    pthread_mutex_lock(&s_lock);
    if ((j = job_find(id)))
        j->state = JOB_PROCESSING;
    pthread_mutex_unlock(&s_lock);
    scan_state_t st = send_stream(h, "image/jpeg");
    pthread_mutex_lock(&s_lock);
    if ((j = job_find(id)))
        j->state = st == ST_DONE ? JOB_COMPLETED : st == ST_CANCELED ? JOB_CANCELED : JOB_ABORTED;
    pthread_mutex_unlock(&s_lock);
}

static void escl_image_info(airprint_http_t *h, const char *id)
{
    pthread_mutex_lock(&s_lock);
    escl_job_t *j = job_find(id);
    scan_area_t a = j ? j->area : (scan_area_t){0};
    pthread_mutex_unlock(&s_lock);
    if (!j) {
        send_text(h, 404, "no such job");
        return;
    }
    char x[640];
    int n = snprintf(x, sizeof(x),
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                     "<scan:ScanImageInfo " ESCL_NS ">\r\n"
                     "<pwg:JobUri>/eSCL/ScanJobs/%s</pwg:JobUri><pwg:JobUuid>%s</pwg:JobUuid>\r\n"
                     "<scan:ActualWidth>%d</scan:ActualWidth><scan:ActualHeight>%d</scan:ActualHeight>\r\n"
                     "<scan:ActualBytesPerLine>%d</scan:ActualBytesPerLine>\r\n"
                     "</scan:ScanImageInfo>\r\n",
                     id, id, a.w, a.h, a.w * 3);
    send_xml(h, x, n);
}

static void escl_cancel_job(airprint_http_t *h, const char *id)
{
    pthread_mutex_lock(&s_lock);
    escl_job_t *j = job_find(id);
    bool running = j && j->state == JOB_PROCESSING;
    if (j && j->state == JOB_PENDING)
        j->state = JOB_CANCELED;
    pthread_mutex_unlock(&s_lock);
    if (!j) {
        send_text(h, 404, "no such job");
        return;
    }
    if (running)
        scan_service_cancel();      // the NextDocument request ends and marks it canceled
    airprint_http_send(h, "200 OK", "text/plain", "", 0);
}

static bool escl_http(airprint_http_t *h, const char *method, const char *route)
{
    bool get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    const char *r = route + 5;      // after "/eSCL"
    if (!strcmp(r, "/ScannerCapabilities") && get) {
        escl_capabilities(h);
    } else if (!strcmp(r, "/ScannerStatus") && get) {
        escl_status(h);
    } else if (!strcmp(r, "/ScanJobs") && !strcmp(method, "POST")) {
        escl_new_job(h);
    } else if (!strncmp(r, "/ScanJobs/", 10)) {
        char id[40];
        const char *p = r + 10, *slash = strchr(p, '/');
        size_t il = slash ? (size_t)(slash - p) : strlen(p);
        if (il == 0 || il >= sizeof(id))
            return false;
        memcpy(id, p, il);
        id[il] = 0;
        if (!slash && !strcmp(method, "DELETE"))
            escl_cancel_job(h, id);
        else if (slash && !strcmp(slash, "/NextDocument") && !strcmp(method, "GET"))
            escl_next_document(h, id);
        else if (slash && !strcmp(slash, "/ScanImageInfo") && get)
            escl_image_info(h, id);
        else
            return false;
    } else {
        return false;
    }
    return true;
}

bool scan_service_http(airprint_http_t *h, const char *method, const char *path)
{
    bool escl = !strncmp(path, "/eSCL/", 6);
    if (!s_cfg || (!escl && (strncmp(path, "/scan", 5) != 0 || (path[5] && path[5] != '/' && path[5] != '?'))))
        return false;
    char route[96];
    const char *q = strchr(path, '?');
    size_t rl = q ? (size_t)(q - path) : strlen(path);
    if (rl >= sizeof(route))
        return false;
    memcpy(route, path, rl);
    route[rl] = 0;
    const char *query = q ? q + 1 : "";
    bool get = !strcmp(method, "GET") || !strcmp(method, "HEAD"), post = !strcmp(method, "POST");
    if (escl)
        return escl_http(h, method, route);

    if (!strcmp(route, "/scan") || !strcmp(route, "/scan/")) {
        // the scanner page of the web UI
        if (get)
            airprint_http_send_ex(h, "302 Found", "text/plain", "Location: /#/scanner\r\n", "", 0);
        else
            send_text(h, 405, "use GET");
    } else if (!strcmp(route, "/scan/status")) {
        get ? handle_status(h) : send_text(h, 405, "use GET");
    } else if (!strcmp(route, "/scan/stream.jpg")) {
        !strcmp(method, "GET") ? handle_stream(h, query) : send_text(h, 405, "use GET");    // HEAD would scan
    } else if (!strcmp(route, "/scan/last")) {
        post ? handle_last(h, query) : send_text(h, 405, "use POST");
    } else if (!strcmp(route, "/scan/last.jpg")) {
        get ? handle_last_jpg(h, query) : send_text(h, 405, "use GET");
    } else if (!strcmp(route, "/scan/cancel")) {
        if (post) {
            scan_service_cancel();
            airprint_http_send(h, "200 OK", "application/json", "{\"ok\":true}", 11);
        } else {
            send_text(h, 405, "use POST");
        }
    } else {
        return false;
    }
    return true;
}

void scan_service_init(const scan_config_t *cfg)
{
    s_cfg = cfg;
#ifdef ESP_PLATFORM
    S.boot = esp_random();
#else
    S.boot = (uint32_t)time(NULL) ^ (uint32_t)now_us() ^ ((uint32_t)getpid() << 16);
#endif
}

bool scan_service_active(void)
{
    return S.running;
}

void scan_service_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    if (S.producing) {
        LOGI("cancel requested");
        S.cancel = true;
    }
    pthread_mutex_unlock(&s_lock);
}
