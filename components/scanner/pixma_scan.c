// Canon PIXMA "generation 5" scan protocol for the MG2500 series. Command
// codes, payload layouts, timeouts and the order of operations follow SANE's
// pixma backend (backend/pixma/pixma_mp150.c and pixma_common.c).
//
// Command:  u16be code, 10 x 0, u32be payload length (+ expected reply data
//           length), payload whose last byte makes the payload sum 0 (mod 256)
// Reply:    u16be status (0x0606 ok, 0x1414 busy, 0x1515 failed), 6 x 0,
//           then data whose last byte makes the data sum 0
// Image:    reply header (8) + flags (u8: 0x08 end of scan, 0x20 no more
//           data), 3 x 0, u32be block length, then raw data without checksum
#include "pixma_scan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define LOGI(...) ESP_LOGI("pixma", __VA_ARGS__)
#define LOGW(...) ESP_LOGW("pixma", __VA_ARGS__)
#else
#define LOGI(fmt, ...) fprintf(stderr, "I pixma: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "W pixma: " fmt "\n", ##__VA_ARGS__)
#endif

enum {
    CMD_START_SESSION = 0xdb20,
    CMD_GAMMA = 0xee20,
    CMD_ABORT_SESSION = 0xef20,
    CMD_READ_IMAGE = 0xd420,
    CMD_ERROR_INFO = 0xff20,
    CMD_SCAN_PARAM_3 = 0xd820,
    CMD_SCAN_START_3 = 0xd920,
    CMD_STATUS_3 = 0xda20,
};

#define STATUS_OK 0x0606
#define STATUS_BUSY 0x1414
#define STATUS_FAILED 0x1515

#define CMD_HDR 16
#define RES_HDR 8
#define CMDBUF_SIZE (4096 + 24)
#define IMAGE_BLOCK_SIZE (512 * 1024)   // block size SANE asks for; read in pieces
#define IO_TIMEOUT_MS 1000              // PIXMA_BULKIN/BULKOUT_TIMEOUT
#define RESPONSE_TRIES 8                // rec_tmo: 8 x 1 s for a reply
#define CALIBRATION_TRIES 120           // wait_until_ready: 120 x 1 s
#define MAX_ZLP 4

#define XML_START_1                                                                                 \
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"                                                   \
    "<cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\">"                              \
    "<ivec:contents><ivec:operation>StartJob</ivec:operation>"                                      \
    "<ivec:param_set servicetype=\"scan\"><ivec:jobID>00000001</ivec:jobID>"                        \
    "<ivec:bidi>1</ivec:bidi></ivec:param_set></ivec:contents></cmd>"

#define XML_START_2                                                                                 \
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"                                                   \
    "<cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\" "                              \
    "xmlns:vcn=\"http://www.canon.com/ns/cmd/2008/07/canon/\">"                                     \
    "<ivec:contents><ivec:operation>VendorCmd</ivec:operation>"                                     \
    "<ivec:param_set servicetype=\"scan\"><ivec:jobID>00000001</ivec:jobID>"                        \
    "<vcn:ijoperation>ModeShift</vcn:ijoperation><vcn:ijmode>1</vcn:ijmode>"                        \
    "</ivec:param_set></ivec:contents></cmd>"

#define XML_END                                                                                     \
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"                                                   \
    "<cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\">"                              \
    "<ivec:contents><ivec:operation>EndJob</ivec:operation>"                                        \
    "<ivec:param_set servicetype=\"scan\"><ivec:jobID>00000001</ivec:jobID>"                        \
    "</ivec:param_set></ivec:contents></cmd>"

#define XML_OK "<ivec:response>OK</ivec:response>"

typedef struct {
    const pixma_io_t *io;
    const pixma_params_t *p;
    volatile bool *cancel;
    pixma_line_fn line_cb;
    void *user;
    uint8_t *cmd;               // command and reply buffer, CMDBUF_SIZE
    size_t cmd_len, res_len;    // pending command, expected reply length
    uint8_t *chunk;             // image reads, PIXMA_READ_CHUNK
    uint8_t *line;              // raw line being assembled
    size_t raw_line, line_fill;
    int xs, wx, h;
    int lines;                  // lines delivered
    bool stop;                  // the line callback asked to stop
} scan_t;

static void put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static unsigned get_be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t sum_bytes(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    while (n--)
        s += *p++;
    return s;
}

static int map_status(unsigned st)
{
    return st == STATUS_OK ? PIXMA_OK : st == STATUS_BUSY ? PIXMA_EBUSY : st == STATUS_FAILED ? PIXMA_ECANCELED
                                                                                             : PIXMA_EPROTO;
}

static void sleep_ms(scan_t *s, int ms)
{
    if (s->io->sleep_ms)
        s->io->sleep_ms(s->io->ctx, ms);
}

static bool canceled(const scan_t *s)
{
    return s->cancel && *s->cancel;
}

// ---------------------------------------------------------------------------
// transport

static int write_all(scan_t *s, const uint8_t *data, size_t len)
{
    int r = s->io->write(s->io->ctx, data, len, IO_TIMEOUT_MS);
    if (r < 0)
        return r;
    return (size_t)r == len ? PIXMA_OK : PIXMA_EIO;
}

// Reads a reply, waiting up to RESPONSE_TRIES timeouts for it. Zero-length
// packets left over from a previous transfer are skipped.
static int read_reply(scan_t *s, uint8_t *buf, size_t len)
{
    int tries = RESPONSE_TRIES, zlp = 0;
    for (;;) {
        int r = s->io->read(s->io->ctx, buf, len, IO_TIMEOUT_MS);
        if (r == PIXMA_ETIMEDOUT && --tries > 0)
            continue;
        if (r == 0 && ++zlp <= MAX_ZLP)
            continue;
        return r;
    }
}

// Starts a command; returns the payload to fill in (zeroed).
static uint8_t *new_cmd(scan_t *s, unsigned code, size_t dataout, size_t datain)
{
    memset(s->cmd, 0, CMD_HDR + dataout);
    put_be16(s->cmd, code);
    put_be32(s->cmd + 12, (uint32_t)(dataout + datain));
    s->cmd_len = CMD_HDR + dataout;
    s->res_len = RES_HDR + datain;
    return s->cmd + CMD_HDR;
}

// Sends the pending command and checks the reply (pixma_exec +
// pixma_check_result). Reply data is at s->cmd + RES_HDR.
static int exec(scan_t *s)
{
    if (s->cmd_len > CMD_HDR) {
        uint8_t *payload = s->cmd + CMD_HDR;
        size_t n = s->cmd_len - CMD_HDR;
        payload[n - 1] = (uint8_t)-sum_bytes(payload, n - 1);
    }
    unsigned code = get_be16(s->cmd);
    int r = write_all(s, s->cmd, s->cmd_len);
    if (r < 0)
        return r;
    r = read_reply(s, s->cmd, s->res_len);
    if (r < 0) {
        LOGW("no reply to command %04x: %s", code, pixma_strerror(r));
        return r;
    }
    if (r < RES_HDR)
        return PIXMA_EPROTO;
    int err = map_status(get_be16(s->cmd));
    if ((size_t)r == s->res_len) {
        if (sum_bytes(s->cmd + RES_HDR, (size_t)r - RES_HDR) != 0)
            err = PIXMA_EPROTO;
    } else if (r != RES_HDR) {
        err = PIXMA_EPROTO;     // only a bare header (a failed command) may be shorter
    }
    if (err == PIXMA_EPROTO)
        LOGW("bad reply to command %04x: %d bytes, status %04x", code, r, get_be16(s->cmd));
    return err;
}

static int exec_short(scan_t *s, unsigned code)
{
    new_cmd(s, code, 0, 0);
    return exec(s);
}

static int xml_dialog(scan_t *s, const char *xml, const char *what)
{
    int r = write_all(s, (const uint8_t *)xml, strlen(xml));
    if (r >= 0)
        r = read_reply(s, s->cmd, 1024);
    if (r < 0) {
        LOGW("%s: %s", what, pixma_strerror(r));
        return r;
    }
    s->cmd[r] = 0;
    if (!strstr((const char *)s->cmd, XML_OK)) {
        const char *d = strstr((const char *)s->cmd, "<ivec:response_detail>");
        LOGW("%s refused: %.60s", what, d ? d + 22 : (const char *)s->cmd);
        return PIXMA_EPROTO;
    }
    return PIXMA_OK;
}

// Reads and discards whatever the scanner still sends.
static void drain(scan_t *s)
{
    for (int i = 0; i < 2048; i++)
        if (s->io->read(s->io->ctx, s->chunk, PIXMA_READ_CHUNK, IO_TIMEOUT_MS) < 0)
            break;
}

// ---------------------------------------------------------------------------
// commands

static int send_gamma(scan_t *s)
{
    // 1024 x u16le, gamma 2.2 (SANE's default), header 0x10 / 0x0804
    uint8_t *d = new_cmd(s, CMD_GAMMA, 1024 * 2 + 8, 0);
    d[0] = 0x10;
    put_be16(d + 2, 0x0804);
    for (int i = 0; i < 1024; i++) {
        unsigned v = (unsigned)(65535.0 * pow(i / 1023.0, 1.0 / 2.2) + 0.5);
        d[4 + 2 * i] = (uint8_t)v;
        d[5 + 2 * i] = (uint8_t)(v >> 8);
    }
    return exec(s);
}

static int send_scan_param(scan_t *s)
{
    const pixma_params_t *p = s->p;
    uint8_t *d = new_cmd(s, CMD_SCAN_PARAM_3, 0x38, 0);
    d[0x00] = 0x01;             // flatbed
    d[0x01] = 0x01;
    d[0x02] = 0x01;
    d[0x05] = p->calibrate ? 0x01 : 0x00;
    put_be16(d + 0x08, (unsigned)p->dpi | 0x8000);
    put_be16(d + 0x0a, (unsigned)p->dpi | 0x8000);
    put_be32(d + 0x0c, (uint32_t)(p->x - s->xs));
    put_be32(d + 0x10, (uint32_t)p->y);
    put_be32(d + 0x14, (uint32_t)s->wx);
    put_be32(d + 0x18, (uint32_t)s->h);
    d[0x1c] = 0x08;             // colour
    d[0x1d] = 24;               // bits per pixel
    d[0x1f] = 0x01;
    d[0x20] = 0xff;
    d[0x21] = 0x81;
    d[0x23] = 0x02;
    d[0x24] = 0x01;
    d[0x30] = 0x01;
    return exec(s);
}

// status byte 0 bit 0 or 1: calibrated (is_calibrated, generation >= 3)
static int wait_until_ready(scan_t *s)
{
    for (int tries = CALIBRATION_TRIES;; tries--) {
        new_cmd(s, CMD_STATUS_3, 0, 8);
        int err = exec(s);
        if (err < 0)
            return err;
        if (s->cmd[RES_HDR] & 0x03)
            return PIXMA_OK;
        if (tries == 0) {
            LOGW("scanner not ready after %d s", CALIBRATION_TRIES);
            return PIXMA_ETIMEDOUT;
        }
        if (canceled(s))
            return PIXMA_ECANCELED;
        sleep_ms(s, 1000);
    }
}

static int start_session(scan_t *s)
{
    int err = exec_short(s, CMD_START_SESSION);
    for (int tries = 10; err == PIXMA_EBUSY && tries > 0; tries--) {
        if (canceled(s))
            return PIXMA_ECANCELED;
        LOGI("scanner busy, retrying");
        sleep_ms(s, 1000);
        err = exec_short(s, CMD_START_SESSION);
    }
    if (err == PIXMA_EBUSY || err == PIXMA_ETIMEDOUT) {
        // maybe hung: empty its output, abort, try once more
        LOGW("scanner hangs? sending abort session");
        drain(s);
        exec_short(s, CMD_ABORT_SESSION);
        sleep_ms(s, 500);
        err = exec_short(s, CMD_START_SESSION);
    }
    return err;
}

// ---------------------------------------------------------------------------
// image data

static void feed(scan_t *s, const uint8_t *data, size_t n)
{
    while (n) {
        size_t k = s->raw_line - s->line_fill;
        if (k > n)
            k = n;
        memcpy(s->line + s->line_fill, data, k);
        s->line_fill += k;
        data += k;
        n -= k;
        if (s->line_fill == s->raw_line) {
            s->line_fill = 0;
            if (s->lines < s->h && !s->stop) {
                if (s->line_cb(s->user, s->lines, s->line + 3 * s->xs))
                    s->stop = true;
                s->lines++;
            }
        }
    }
}

// Reads one image block (read_image_block). Returns its flags or < 0.
static int read_block(scan_t *s, uint8_t last_flags)
{
    uint8_t cmd[CMD_HDR] = {0};
    put_be16(cmd, CMD_READ_IMAGE);
    put_be32(cmd + 12, (last_flags & 0x20) ? 32 + 8 : IMAGE_BLOCK_SIZE + 8);
    int r = write_all(s, cmd, sizeof(cmd));
    if (r < 0)
        return r;
    int got = read_reply(s, s->chunk, PIXMA_READ_CHUNK);
    if (got < 0)
        return got;
    if (got < RES_HDR)
        return PIXMA_EPROTO;
    int err = map_status(get_be16(s->chunk));
    if (err < 0)
        return err;
    if (got < 16)
        return PIXMA_EPROTO;
    uint8_t flags = s->chunk[8];
    uint32_t len = get_be32(s->chunk + 12);
    if (flags & ~0x38)
        LOGW("unexpected image block flags %02x", flags);
    uint32_t have = (uint32_t)got - 16;
    if (have > len)
        return PIXMA_EPROTO;
    feed(s, s->chunk + 16, have);
    uint32_t left = len - have;
    if (left && got < PIXMA_READ_CHUNK) {
        LOGW("image block ended early: %u of %u bytes", (unsigned)have, (unsigned)len);
        return PIXMA_EPROTO;
    }
    int tries = RESPONSE_TRIES;
    while (left) {
        size_t n = left < PIXMA_READ_CHUNK ? left : PIXMA_READ_CHUNK;
        r = s->io->read(s->io->ctx, s->chunk, n, IO_TIMEOUT_MS);
        if (r == PIXMA_ETIMEDOUT && --tries > 0)
            continue;
        if (r < 0)
            return r;
        if (r == 0)
            continue;   // stray zero-length packet
        feed(s, s->chunk, (size_t)r);
        left -= (uint32_t)r;
        if ((size_t)r < n && left) {
            LOGW("image block ended early: %u bytes missing", (unsigned)left);
            return PIXMA_EPROTO;
        }
    }
    if (len == 0)
        sleep_ms(s, 10);    // no data yet
    return flags & 0x38;
}

static int read_image(scan_t *s)
{
    uint8_t flags = 0;
    while ((flags & 0x28) != 0x28) {    // end of image
        if (canceled(s) || s->stop)
            return PIXMA_ECANCELED;
        int r = read_block(s, flags);
        if (r < 0) {
            if (r == PIXMA_ECANCELED) {
                // seen in traffic logs after a failed block (meaning unknown)
                new_cmd(s, CMD_ERROR_INFO, 0, 16);
                exec(s);
            }
            return r;
        }
        flags = (uint8_t)r;
    }
    return PIXMA_OK;
}

// ---------------------------------------------------------------------------

int pixma_scan(const pixma_io_t *io, const pixma_params_t *p, pixma_line_fn line, void *user,
               volatile bool *cancel)
{
    int max_w = PIXMA_MAX_WIDTH_75 * p->dpi / 75, max_h = PIXMA_MAX_HEIGHT_75 * p->dpi / 75;
    if ((p->dpi != 75 && p->dpi != 150 && p->dpi != 300 && p->dpi != 600) || p->x < 0 || p->y < 0 || p->w <= 0 ||
        p->h <= 0 || p->x + p->w > max_w || p->y >= max_h)
        return PIXMA_EINVAL;

    scan_t s = {.io = io, .p = p, .cancel = cancel, .line_cb = line, .user = user};
    s.xs = p->x % 32;
    s.wx = (p->w + s.xs + 31) & ~31;
    s.h = p->h < max_h - p->y ? p->h : max_h - p->y;
    s.raw_line = (size_t)s.wx * 3;
    s.cmd = malloc(CMDBUF_SIZE + 1);
    s.chunk = malloc(PIXMA_READ_CHUNK);
    s.line = malloc(s.raw_line);
    int err = PIXMA_ENOMEM;
    if (!s.cmd || !s.chunk || !s.line)
        goto out;

    LOGI("scan %d dpi, %dx%d at (%d,%d), raw line %d px", p->dpi, p->w, s.h, p->x, p->y, s.wx);
    err = xml_dialog(&s, XML_START_1, "StartJob");
    if (err < 0)
        goto out;
    err = xml_dialog(&s, XML_START_2, "ModeShift");
    if (err < 0)
        goto end_job;

    err = start_session(&s);
    // from here on the session is aborted at the end, whatever happened
    for (int i = 0; i < 3 && err >= 0; i++)    // SANE sends it three times
        err = send_gamma(&s);
    if (err >= 0)
        err = send_scan_param(&s);
    if (err >= 0)
        err = exec_short(&s, CMD_SCAN_START_3);
    if (err >= 0)
        err = wait_until_ready(&s);
    if (err >= 0) {
        sleep_ms(&s, 1000);     // the Windows driver waits 1.5 s, SANE 1 s
        err = read_image(&s);
        if (err < 0 && err != PIXMA_ECANCELED)
            drain(&s);          // a transfer may have been cut off
    }
    if (err >= 0 && s.lines < s.h)
        LOGW("short image: %d of %d lines", s.lines, s.h);
    int e2 = exec_short(&s, CMD_ABORT_SESSION);
    if (e2 < 0)
        LOGW("abort session failed: %s", pixma_strerror(e2));
end_job:
    xml_dialog(&s, XML_END, "EndJob");
out:
    if (err < 0)
        LOGW("scan ended: %s after %d lines", pixma_strerror(err), s.lines);
    free(s.cmd);
    free(s.chunk);
    free(s.line);
    return err < 0 ? err : PIXMA_OK;
}

const char *pixma_strerror(int err)
{
    switch (err) {
    case PIXMA_OK: return "ok";
    case PIXMA_EIO: return "I/O error";
    case PIXMA_ETIMEDOUT: return "timeout";
    case PIXMA_EPROTO: return "protocol error";
    case PIXMA_EBUSY: return "scanner busy";
    case PIXMA_ECANCELED: return "canceled";
    case PIXMA_ENOMEM: return "out of memory";
    case PIXMA_EINVAL: return "invalid parameters";
    default: return "error";
    }
}
