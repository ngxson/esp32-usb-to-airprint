// Server-Sent Events hub: see events.h.
#include "events.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "canon_bjl.h"      // BJL_MALLOC_BULK

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define LOGI(...) ESP_LOGI("events", __VA_ARGS__)
#define LOGW(...) ESP_LOGW("events", __VA_ARGS__)
#define HUB_STACK (4 * 1024)
#else
#define LOGI(fmt, ...) fprintf(stderr, "I events: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "W events: " fmt "\n", ##__VA_ARGS__)
#define HUB_STACK (256 * 1024)
#endif

#define MAX_SOURCES 4
#define SNAPSHOT_SIZE 3072
#define TICK_MS 1000
#define WAKE_MS 250             // how often a poke is noticed
#define KEEPALIVE_S 15
// the snapshot buffers live in PSRAM on the board: internal RAM is scarce
#define ALLOC(n) BJL_MALLOC_BULK(n)

static struct {
    const char *key;
    events_source_fn fn;
    void *ctx;
} s_src[MAX_SOURCES];
static int s_nsrc;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fd[EVENTS_MAX_LISTENERS];      // -1 = free
static unsigned s_order[EVENTS_MAX_LISTENERS];  // age, for dropping the oldest
static unsigned s_seq;
static volatile bool s_poke;
static bool s_started;

void events_add_source(const char *key, events_source_fn fn, void *ctx)
{
    if (s_nsrc < MAX_SOURCES) {
        s_src[s_nsrc].key = key;
        s_src[s_nsrc].fn = fn;
        s_src[s_nsrc].ctx = ctx;
        s_nsrc++;
    }
}

void events_poke(void)
{
    s_poke = true;
}

size_t events_json_str(char *buf, size_t pos, size_t n, const char *s)
{
    if (pos + 1 >= n)
        return pos;
    buf[pos++] = '"';
    for (; s && *s && pos + 8 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = (char)ch;
        } else if (ch < 0x20) {
            pos += (size_t)snprintf(buf + pos, n - pos, "\\u%04x", ch);
        } else {
            buf[pos++] = (char)ch;
        }
    }
    if (pos + 1 < n)
        buf[pos++] = '"';
    buf[pos] = 0;
    return pos;
}

// {"key":value,...}; returns the length
static int build_snapshot(char *buf, size_t n)
{
    size_t pos = 0;
    buf[pos++] = '{';
    char *part = ALLOC(SNAPSHOT_SIZE);
    for (int i = 0; part && i < s_nsrc; i++) {
        int k = s_src[i].fn(s_src[i].ctx, part, SNAPSHOT_SIZE);
        if (k < 0 || k >= SNAPSHOT_SIZE)
            continue;
        size_t need = strlen(s_src[i].key) + (size_t)k + 5;
        if (pos + need >= n - 2)
            break;
        pos += (size_t)snprintf(buf + pos, n - pos, "%s\"%s\":%s", pos > 1 ? "," : "", s_src[i].key, part);
    }
    free(part);
    buf[pos++] = '}';
    buf[pos] = 0;
    return (int)pos;
}

static void drop(int i)
{
    close(s_fd[i]);
    s_fd[i] = -1;
}

// Sends to every listener without blocking; a listener that cannot take the
// whole message is dropped (a partial event would corrupt its stream).
static void broadcast(const char *msg, size_t len)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < EVENTS_MAX_LISTENERS; i++) {
        if (s_fd[i] < 0)
            continue;
        ssize_t k = send(s_fd[i], msg, len, MSG_DONTWAIT);
        if (k != (ssize_t)len) {
            LOGI("listener dropped (%s)", k < 0 ? strerror(errno) : "send buffer full");
            drop(i);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *hub_thread(void *arg)
{
    char *snap = ALLOC(SNAPSHOT_SIZE), *last = ALLOC(SNAPSHOT_SIZE), *msg = ALLOC(SNAPSHOT_SIZE + 32);
    if (!snap || !last || !msg) {
        LOGW("no memory for the event hub");
        return NULL;
    }
    last[0] = 0;
    int64_t t_built = 0, t_sent = now_ms();
    for (;;) {
        // wait for a tick, noticing listeners that hang up
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1, count = 0;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < EVENTS_MAX_LISTENERS; i++) {
            if (s_fd[i] >= 0) {
                FD_SET(s_fd[i], &rd);
                if (s_fd[i] > maxfd)
                    maxfd = s_fd[i];
                count++;
            }
        }
        pthread_mutex_unlock(&s_lock);
        if (!count) {
            last[0] = 0;        // a new listener gets its own snapshot anyway
            usleep(WAKE_MS * 1000);
            continue;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = WAKE_MS * 1000};
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r > 0) {
            pthread_mutex_lock(&s_lock);
            for (int i = 0; i < EVENTS_MAX_LISTENERS; i++) {
                if (s_fd[i] < 0 || !FD_ISSET(s_fd[i], &rd))
                    continue;
                char tmp[64];
                ssize_t k = recv(s_fd[i], tmp, sizeof(tmp), MSG_DONTWAIT);
                if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                    drop(i);    // closed by the browser
            }
            pthread_mutex_unlock(&s_lock);
        }
        int64_t now = now_ms();
        if (!s_poke && now - t_built < TICK_MS)
            continue;
        s_poke = false;
        t_built = now;
        int len = build_snapshot(snap, SNAPSHOT_SIZE);
        if (strcmp(snap, last) != 0) {
            memcpy(last, snap, (size_t)len + 1);
            int m = snprintf(msg, SNAPSHOT_SIZE + 32, "event: state\ndata: %s\n\n", snap);
            broadcast(msg, (size_t)m);
            t_sent = now;
        } else if (now - t_sent >= KEEPALIVE_S * 1000) {
            broadcast(": keep-alive\n\n", 14);
            t_sent = now;
        }
    }
    return NULL;
}

int events_start(void)
{
    for (int i = 0; i < EVENTS_MAX_LISTENERS; i++)
        s_fd[i] = -1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HUB_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int r = pthread_create(&th, &attr, hub_thread, NULL);
    pthread_attr_destroy(&attr);
    s_started = r == 0;
    return r;
}

static void subscribe(airprint_http_t *h)
{
    char *snap = ALLOC(SNAPSHOT_SIZE);
    if (!snap) {
        airprint_http_send(h, "503 Service Unavailable", "text/plain", "no memory", 9);
        return;
    }
    build_snapshot(snap, SNAPSHOT_SIZE);
    int fd = airprint_http_detach(h);
    // the first event goes out from here (blocking is fine on this thread);
    // retry: tells EventSource how soon to reconnect after a drop
    static const char hdr[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                              "Connection: close\r\n\r\nretry: 2000\n\n";
    char pre[32];
    int pl = snprintf(pre, sizeof(pre), "event: state\ndata: ");
    bool ok = send(fd, hdr, sizeof(hdr) - 1, 0) == (ssize_t)(sizeof(hdr) - 1) &&
              send(fd, pre, (size_t)pl, 0) == pl && send(fd, snap, strlen(snap), 0) == (ssize_t)strlen(snap) &&
              send(fd, "\n\n", 2, 0) == 2;
    free(snap);
    if (!ok) {
        close(fd);
        return;
    }
    struct timeval tv = {0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_mutex_lock(&s_lock);
    int slot = -1;
    for (int i = 0; i < EVENTS_MAX_LISTENERS; i++) {
        if (s_fd[i] < 0) {
            slot = i;
            break;
        }
        if (slot < 0 || s_order[i] < s_order[slot])
            slot = i;   // all taken: the oldest
    }
    if (s_fd[slot] >= 0) {
        LOGI("%d listeners: dropping the oldest", EVENTS_MAX_LISTENERS);
        drop(slot);
    }
    s_fd[slot] = fd;
    s_order[slot] = ++s_seq;
    pthread_mutex_unlock(&s_lock);
}

bool events_http(airprint_http_t *h, const char *method, const char *path)
{
    bool get = !strcmp(method, "GET");
    if (!strcmp(path, "/events")) {
        if (!get)
            airprint_http_send(h, "405 Method Not Allowed", "text/plain", "use GET", 7);
        else if (!s_started)
            airprint_http_send(h, "503 Service Unavailable", "text/plain", "events not running", 18);
        else
            subscribe(h);
        return true;
    }
    if (!strcmp(path, "/api/state") && (get || !strcmp(method, "HEAD"))) {
        char *snap = ALLOC(SNAPSHOT_SIZE);
        if (!snap) {
            airprint_http_send(h, "503 Service Unavailable", "text/plain", "no memory", 9);
            return true;
        }
        int n = build_snapshot(snap, SNAPSHOT_SIZE);
        airprint_http_send_ex(h, "200 OK", "application/json", "Cache-Control: no-store\r\n", snap, (size_t)n);
        free(snap);
        return true;
    }
    return false;
}
