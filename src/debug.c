#include "debug.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "local_config.h"

static const char *TAG = "debug";

#define LINE_MAX_LEN 256        // longer log lines are cut in the ring (not on the UART)
#define OTA_BUF_SIZE 4096
#define SEND_PIECE 1024
#define INFO_SIZE 6144

// ---------------------------------------------------------------------------
// log ring

static char *s_ring;
static size_t s_ring_size;
static uint64_t s_wpos;                 // bytes ever written; the ring holds the last s_ring_size
static SemaphoreHandle_t s_log_lock;
static SemaphoreHandle_t s_log_new;     // given on every new line (wakes the stream)
static vprintf_like_t s_uart_vprintf;
static char s_line[LINE_MAX_LEN];       // under s_log_lock

// The end of the log in RTC memory: survives panics, watchdog and software
// resets (not power loss). Shown at the top of the next boot's log.
#define TAIL_SIZE 3072
#define TAIL_MAGIC 0x54676f6cu
typedef struct {
    uint32_t magic;
    uint32_t pos, len;
    char data[TAIL_SIZE];
} tail_t;
static RTC_NOINIT_ATTR tail_t s_tail;

static void ring_put(const char *p, size_t n)
{
    while (n) {
        size_t off = (size_t)(s_wpos % s_ring_size), k = s_ring_size - off;
        if (k > n)
            k = n;
        memcpy(s_ring + off, p, k);
        s_wpos += k;
        p += k;
        n -= k;
    }
}

static void tail_put(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_tail.data[s_tail.pos] = p[i];
        s_tail.pos = (s_tail.pos + 1) % TAIL_SIZE;
    }
    s_tail.len = s_tail.len + n > TAIL_SIZE ? TAIL_SIZE : s_tail.len + (uint32_t)n;
}

// copies up to n bytes of the log from position *pos; returns the count
// (*pos moves to the oldest byte still held if it fell behind)
static size_t ring_get(uint64_t *pos, char *out, size_t n, uint64_t *lost)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    if (s_wpos - *pos > s_ring_size) {
        if (lost)
            *lost += s_wpos - s_ring_size - *pos;
        *pos = s_wpos - s_ring_size;
    }
    size_t avail = (size_t)(s_wpos - *pos), got = 0;
    if (n > avail)
        n = avail;
    while (got < n) {
        size_t off = (size_t)((*pos + got) % s_ring_size), k = s_ring_size - off;
        if (k > n - got)
            k = n - got;
        memcpy(out + got, s_ring + off, k);
        got += k;
    }
    *pos += got;
    xSemaphoreGive(s_log_lock);
    return got;
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_uart_vprintf(fmt, ap);
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        if (n >= (int)sizeof(s_line)) {
            n = sizeof(s_line) - 1;
            s_line[n - 1] = '\n';
        }
        if (n > 0) {
            ring_put(s_line, (size_t)n);
            tail_put(s_line, (size_t)n);
        }
        xSemaphoreGive(s_log_lock);
        xSemaphoreGive(s_log_new);
    }
    va_end(copy);
    return r;
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software (esp_restart)";
    case ESP_RST_PANIC: return "panic (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
    }
}

static const char *ota_state_name(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK)
        return "flashed";       // written over serial: no OTA state
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static void log_coredump_summary(void)
{
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return;
    esp_core_dump_summary_t *sm = malloc(sizeof(*sm));
    if (sm && esp_core_dump_get_summary(sm) == ESP_OK) {
        char bt[16 * 11 + 1] = "";
        int n = 0;
        for (uint32_t i = 0; i < sm->exc_bt_info.depth && i < 16; i++)
            n += snprintf(bt + n, sizeof(bt) - n, " 0x%08lx", (unsigned long)sm->exc_bt_info.bt[i]);
        ESP_LOGE(TAG, "a previous run crashed: task %s, PC 0x%08lx, backtrace%s%s", sm->exc_task,
                 (unsigned long)sm->exc_pc, bt, sm->exc_bt_info.corrupted ? " (corrupted)" : "");
    }
    ESP_LOGE(TAG, "core dump of %u bytes kept: tools/remote.py crash", (unsigned)size);
    free(sm);
}

void debug_log_start(size_t ring_size)
{
    s_ring = heap_caps_malloc(ring_size, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ring_size = 8 * 1024;
        s_ring = malloc(ring_size);
    }
    s_log_lock = xSemaphoreCreateMutex();
    s_log_new = xSemaphoreCreateBinary();
    if (!s_ring || !s_log_lock || !s_log_new) {
        ESP_LOGE(TAG, "no memory for the log ring");
        return;
    }
    s_ring_size = ring_size;

    // the end of the previous boot's log, then this boot
    char hdr[96];
    if (s_tail.magic == TAIL_MAGIC && s_tail.len <= TAIL_SIZE && s_tail.pos < TAIL_SIZE && s_tail.len) {
        int n = snprintf(hdr, sizeof(hdr), "==== end of the previous boot's log ====\n");
        ring_put(hdr, (size_t)n);
        uint32_t start = s_tail.len < TAIL_SIZE ? 0 : s_tail.pos;
        for (uint32_t i = 0; i < s_tail.len; i++)
            ring_put(&s_tail.data[(start + i) % TAIL_SIZE], 1);
    }
    int n = snprintf(hdr, sizeof(hdr), "==== boot, reset reason: %s ====\n", reset_reason());
    ring_put(hdr, (size_t)n);
    s_tail.magic = TAIL_MAGIC;
    s_tail.pos = s_tail.len = 0;
    s_uart_vprintf = esp_log_set_vprintf(log_vprintf);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "firmware %s, built %s %s, partition %s (%s), reset: %s", app->version, app->date, app->time,
             run ? run->label : "?", ota_state_name(run), reset_reason());
    log_coredump_summary();
}

// ---------------------------------------------------------------------------
// live log stream: nc <host> 2323

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        int r = send(fd, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void log_stream_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DEBUG_LOG_PORT)};
    if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 1)) {
        ESP_LOGE(TAG, "log stream: cannot listen on port %d", DEBUG_LOG_PORT);
        vTaskDelete(NULL);
    }
    static char buf[512];
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval tv = {.tv_sec = 10};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        xSemaphoreTake(s_log_lock, portMAX_DELAY);
        uint64_t pos = s_wpos > s_ring_size ? s_wpos - s_ring_size : 0, lost = 0;   // the oldest line held
        xSemaphoreGive(s_log_lock);
        for (;;) {
            uint64_t lost_before = lost;
            size_t n = ring_get(&pos, buf, sizeof(buf), &lost);
            if (lost != lost_before) {
                char msg[64];
                int k = snprintf(msg, sizeof(msg), "\n==== %llu bytes of log lost ====\n",
                                 (unsigned long long)(lost - lost_before));
                if (send_all(fd, msg, (size_t)k) < 0)
                    break;
            }
            if (n) {
                if (send_all(fd, buf, n) < 0)
                    break;
                continue;
            }
            xSemaphoreTake(s_log_new, pdMS_TO_TICKS(1000));
            char c;
            if (recv(fd, &c, 1, MSG_DONTWAIT) == 0)
                break;      // the client closed the connection
        }
        close(fd);
    }
}

// ---------------------------------------------------------------------------
// HTTP

static bool (*s_busy)(void);
static uint32_t s_boot_id;              // random per boot: tells a client the board restarted

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void restart_soon(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1000 * 1000);   // after the response is out
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_text(req, "200 OK",
                     "Debug server (MODE=debug)\n"
                     "  GET  /info            firmware, memory, tasks, OTA state\n"
                     "  GET  /log             log ring (live: nc <host> 2323)\n"
                     "  POST /ota             firmware image (.pio/build/esp32s3/firmware.bin)\n"
                     "  POST /reboot\n"
                     "  GET  /coredump        last crash's core dump (ELF)\n"
                     "  POST /coredump/erase\n"
                     "  POST /panic           crash on purpose (to try the core dump tools)\n"
                     "  POST /loglevel?tag=<tag|*>&level=<none|error|warn|info>\n"
                     "tools/remote.py does all of this.\n");
}

static esp_err_t info_get(httpd_req_t *req)
{
    char *out = malloc(INFO_SIZE);
    if (!out)
        return send_text(req, "500 Internal Server Error", "no memory\n");
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition(), *next = esp_ota_get_next_update_partition(NULL);
    char elf[17];       // first 8 bytes, as tools/remote.py reads them from the image
    for (int i = 0; i < 8; i++)
        snprintf(elf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    int64_t up = esp_timer_get_time() / 1000000;
    int n = snprintf(out, INFO_SIZE,
                     "firmware   %s (%s), built %s %s, ESP-IDF %s\n"
                     "elf_sha256 %s\n"
                     "mode       %s\n"
                     "partition  %s\n"
                     "ota_state  %s\n"
                     "next_ota   %s\n"
                     "uptime     %lldd %02lld:%02lld:%02lld\n"
                     "boot_id    %08lx\n"
                     "reset      %s\n"
                     "internal   %u KB free, %u KB lowest, %u KB largest block\n"
                     "psram      %u KB free, %u KB lowest\n",
                     app->version, app->project_name, app->date, app->time, app->idf_ver, elf, MODE,
                     run ? run->label : "?", ota_state_name(run), next ? next->label : "none", up / 86400,
                     up / 3600 % 24, up / 60 % 60, up % 60, (unsigned long)s_boot_id, reset_reason(),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        n += snprintf(out + n, INFO_SIZE - n, "wifi_rssi  %d dBm, channel %d\n", ap.rssi, ap.primary);
    size_t cd_addr, cd_size;
    if (esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK)
        n += snprintf(out + n, INFO_SIZE - n, "coredump   %u bytes (GET /coredump)\n", (unsigned)cd_size);
    else
        n += snprintf(out + n, INFO_SIZE - n, "coredump   none\n");
    n += snprintf(out + n, INFO_SIZE - n, "\ntask             state prio stack_free  num\n");
    if (INFO_SIZE - n > 3000)     // vTaskList has no size limit: about 45 bytes per task
        vTaskList(out + n);     // name, state, priority, stack high water mark (bytes), number
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return r;
}

static esp_err_t log_get(httpd_req_t *req)
{
    if (!s_ring)
        return send_text(req, "404 Not Found", "log capture is off\n");
    char *buf = malloc(SEND_PIECE);
    if (!buf)
        return send_text(req, "500 Internal Server Error", "no memory\n");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    uint64_t pos = 0, end = s_wpos;
    esp_err_t r = ESP_OK;
    while (pos < end && r == ESP_OK) {
        size_t n = ring_get(&pos, buf, end - pos < SEND_PIECE ? (size_t)(end - pos) : SEND_PIECE, NULL);
        if (!n)
            break;
        r = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    free(buf);
    return r == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : r;
}

// Reads exactly n bytes of the request body (retrying on timeouts).
static int recv_exact(httpd_req_t *req, uint8_t *buf, size_t n)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < n) {
        int r = httpd_req_recv(req, (char *)buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

// Checks the start of an uploaded image: ESP32-S3 app of this project.
static const char *check_image(const uint8_t *p, size_t n)
{
    const size_t desc_ofs = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < desc_ofs + sizeof(esp_app_desc_t))
        return "image too small";
    const esp_image_header_t *ih = (const esp_image_header_t *)p;
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC)
        return "not an ESP firmware image (use firmware.bin, not the .elf)";
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3)
        return "image is not for the ESP32-S3";
    const esp_app_desc_t *d = (const esp_app_desc_t *)(p + desc_ofs);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "image has no app description";
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0)
        return "image is from another project";
    return NULL;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (s_busy && s_busy())
        return send_text(req, "409 Conflict", "busy printing or scanning, try again later\n");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    size_t total = req->content_len;
    if (!part)
        return send_text(req, "500 Internal Server Error", "no OTA partition\n");
    if (total < 4096 || total > part->size)
        return send_text(req, "400 Bad Request", "bad image size\n");
    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf)
        return send_text(req, "500 Internal Server Error", "no memory\n");

    const char *err = NULL;
    esp_ota_handle_t h = 0;
    size_t done = 0;
    int64_t t0 = esp_timer_get_time();
    int last_pct = -1;
    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", (unsigned)total, part->label);
    while (done < total && !err) {
        size_t n = total - done < OTA_BUF_SIZE ? total - done : OTA_BUF_SIZE;
        if (recv_exact(req, buf, n) < 0) {
            err = "upload interrupted";
            break;
        }
        if (done == 0) {
            err = check_image(buf, n);
            if (!err && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
                err = "cannot start the update";
            if (err)
                break;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            err = "flash write failed";
            break;
        }
        done += n;
        int pct = (int)(done * 10 / total) * 10;
        if (pct != last_pct) {
            last_pct = pct;
            ESP_LOGI(TAG, "OTA: %d%%", pct);
        }
    }
    free(buf);
    if (!err) {
        esp_err_t e = esp_ota_end(h);
        h = 0;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED)
            err = "image is corrupted (validation failed)";
        else if (e != ESP_OK)
            err = "update failed";
        else if (esp_ota_set_boot_partition(part) != ESP_OK)
            err = "cannot select the new image";
    }
    if (h)
        esp_ota_abort(h);
    if (err) {
        ESP_LOGW(TAG, "OTA failed: %s", err);
        char msg[128];
        snprintf(msg, sizeof(msg), "OTA failed: %s\n", err);
        return send_text(req, "400 Bad Request", msg);
    }
    ESP_LOGW(TAG, "OTA: %u bytes written to %s in %.1f s, rebooting", (unsigned)total, part->label,
             (esp_timer_get_time() - t0) / 1e6);
    send_text(req, "200 OK", "OK, rebooting into the new firmware\n");
    restart_soon();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "reboot requested");
    send_text(req, "200 OK", "rebooting\n");
    restart_soon();
    return ESP_OK;
}

// a deliberate crash, to try the core dump tools
static void panic_cb(void *arg)
{
    ESP_LOGE(TAG, "crashing on request");
    volatile int *p = NULL;
    *p = 42;
}

static esp_err_t panic_post(httpd_req_t *req)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = panic_cb, .name = "panic"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK)
        return send_text(req, "500 Internal Server Error", "no timer\n");
    send_text(req, "200 OK", "crashing in 1 s\n");
    esp_timer_start_once(t, 1000 * 1000);
    return ESP_OK;
}

static esp_err_t coredump_get(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return send_text(req, "404 Not Found", "no core dump\n");
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    char *buf = malloc(SEND_PIECE);
    if (!p || !buf) {
        free(buf);
        return send_text(req, "500 Internal Server Error", "cannot read the core dump\n");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t r = ESP_OK;
    for (size_t off = 0; off < size && r == ESP_OK; off += SEND_PIECE) {
        size_t n = size - off < SEND_PIECE ? size - off : SEND_PIECE;
        r = esp_partition_read(p, addr - p->address + off, buf, n);
        if (r == ESP_OK)
            r = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    free(buf);
    return r == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : r;
}

static esp_err_t coredump_erase_post(httpd_req_t *req)
{
    esp_err_t r = esp_core_dump_image_erase();
    return send_text(req, r == ESP_OK ? "200 OK" : "500 Internal Server Error",
                     r == ESP_OK ? "core dump erased\n" : "erase failed\n");
}

static esp_err_t loglevel_post(httpd_req_t *req)
{
    char q[64], tag[32], level[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "tag", tag, sizeof(tag)) != ESP_OK ||
        httpd_query_key_value(q, "level", level, sizeof(level)) != ESP_OK)
        return send_text(req, "400 Bad Request", "use ?tag=<tag|*>&level=<none|error|warn|info>\n");
    static const char *const names[] = {"none", "error", "warn", "info", "debug", "verbose"};
    for (int i = 0; i < 6; i++) {
        if (!strcmp(level, names[i])) {
            esp_log_level_set(tag, (esp_log_level_t)i);
            ESP_LOGI(TAG, "log level of %s set to %s", tag, level);
            return send_text(req, "200 OK", "ok (levels above info are not compiled in)\n");
        }
    }
    return send_text(req, "400 Bad Request", "unknown level\n");
}

esp_err_t debug_server_start(bool (*busy)(void))
{
    s_busy = busy;
    s_boot_id = esp_random();
    if (s_ring)
        xTaskCreate(log_stream_task, "log_stream", 4096, NULL, 3, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = DEBUG_HTTP_PORT;
    cfg.ctrl_port = DEBUG_HTTP_PORT + 1;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 3;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t h;
    esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "debug server: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/coredump", .method = HTTP_GET, .handler = coredump_get},
        {.uri = "/coredump/erase", .method = HTTP_POST, .handler = coredump_erase_post},
        {.uri = "/loglevel", .method = HTTP_POST, .handler = loglevel_post},
        {.uri = "/panic", .method = HTTP_POST, .handler = panic_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        if (httpd_register_uri_handler(h, &uris[i]) != ESP_OK)
            ESP_LOGE(TAG, "cannot register %s", uris[i].uri);
    ESP_LOGW(TAG, "debug server on port %d (log stream: port %d); anyone on this network can update the firmware",
             DEBUG_HTTP_PORT, DEBUG_LOG_PORT);
    return ESP_OK;
}

// ---------------------------------------------------------------------------

void debug_ota_confirm(bool (*healthy)(void), int settle_s, int timeout_s)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY)
        return;
    ESP_LOGW(TAG, "new firmware on %s: confirming it once it has been healthy for %d s", run->label, settle_s);
    int ok_for = 0;
    for (int t = 0; t < timeout_s; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ok_for = healthy() ? ok_for + 1 : 0;
        if (ok_for >= settle_s) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "new firmware confirmed");
            return;
        }
    }
    ESP_LOGE(TAG, "new firmware not healthy after %d s: going back to the previous one", timeout_s);
    vTaskDelay(pdMS_TO_TICKS(500));     // let the log out
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
