// Canon imageCLASS "generation 2" scan protocol (MF220 Series). Command codes,
// layouts and the order of operations follow SANE's pixma backend
// (backend/pixma/pixma_imageclass.c and pixma_common.c); see iclass_scan.h.
//
// Command:  u16be code, 5 x 0, u16be data length (out + expected in) at 7,
//           0, then the payload whose last byte makes its sum 0 (mod 256)
// Reply:    u16be status (0x0606 ok, 0x1414 busy, 0x1515 failed), then data
//           whose last byte makes its sum 0
// Image:    11-byte request (u16be 0xd460, flags at 8, 0x06 at 10); reply of
//           at most 512 bytes: status, info (0x28 = last block) at 2, u32be
//           block length at 4, then the block's first bytes; the rest of the
//           block follows as raw bulk data
#include "iclass_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define LOGI(...) ESP_LOGI("iclass", __VA_ARGS__)
#define LOGW(...) ESP_LOGW("iclass", __VA_ARGS__)
#else
#define LOGI(fmt, ...) fprintf(stderr, "I iclass: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, "W iclass: " fmt "\n", ##__VA_ARGS__)
#endif

enum {
    CMD_START_SESSION = 0xdb20,
    CMD_SELECT_SOURCE = 0xdd20,
    CMD_SCAN_PARAM = 0xde20,
    CMD_STATUS = 0xf320,
    CMD_ABORT_SESSION = 0xef20,
    CMD_READ_IMAGE2 = 0xd460,
    CMD_ERROR_INFO = 0xff20,
};

#define STATUS_OK 0x0606
#define STATUS_BUSY 0x1414
#define STATUS_FAILED 0x1515

#define CMD_HDR 10
#define RES_HDR 2
#define CMDBUF_SIZE 512
#define BLOCK_REPLY 512                 // the first reply to an image request, as SANE reads it
#define BLOCK_HDR 8
#define IO_TIMEOUT_MS 1000              // PIXMA_BULKIN/BULKOUT_TIMEOUT
#define RESPONSE_TRIES 8                // rec_tmo: 8 x 1 s for a reply
#define NOT_READY_MS 100                // SANE waits 100 ms on the interrupt endpoint
#define NOT_READY_TIMEOUT_S 120
#define MAX_ZLP 4

typedef struct {
    const pixma_io_t *io;
    const pixma_params_t *p;
    volatile bool *cancel;
    pixma_line_fn line_cb;
    void *user;
    uint8_t *cmd;               // command and reply buffer, CMDBUF_SIZE
    size_t cmd_len, res_len;    // pending command, expected reply length
    bool command_rejected;    // the last command received an explicit BUSY/FAILED status reply
    uint8_t *chunk;             // image reads, PIXMA_READ_CHUNK
    uint8_t *line;              // raw line being assembled, then the delivered one
    size_t raw_line, line_fill;
    int w, wx, h;               // scanned width (at most the glass), raw width, lines
    int lines;
    bool stop;
    uint8_t status[12];         // last status reply
    uint8_t last_flags;
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

static int read_reply(scan_t *s, uint8_t *buf, size_t len, int tries)
{
    int zlp = 0;
    for (;;) {
        int r = s->io->read(s->io->ctx, buf, len, IO_TIMEOUT_MS);
        if (r == PIXMA_ETIMEDOUT && --tries > 0)
            continue;
        if (r == 0 && ++zlp <= MAX_ZLP)
            continue;
        return r;
    }
}

// pixma_newcmd with a 10-byte header and the length at 7
static uint8_t *new_cmd(scan_t *s, unsigned code, size_t dataout, size_t datain)
{
    memset(s->cmd, 0, CMD_HDR + dataout);
    put_be16(s->cmd, code);
    put_be16(s->cmd + 7, (unsigned)(dataout + datain));
    s->cmd_len = CMD_HDR + dataout;
    s->res_len = RES_HDR + datain;
    return dataout ? s->cmd + CMD_HDR : s->cmd + RES_HDR;
}

// pixma_exec + pixma_check_result. Reply data is at s->cmd + RES_HDR.
static int exec_tries(scan_t *s, int tries)
{
    if (s->cmd_len > CMD_HDR) {
        uint8_t *payload = s->cmd + CMD_HDR;
        size_t n = s->cmd_len - CMD_HDR;
        payload[n - 1] = (uint8_t)-sum_bytes(payload, n - 1);
    }
    unsigned code = get_be16(s->cmd);
    s->command_rejected = false;
    int r = write_all(s, s->cmd, s->cmd_len);
    if (r < 0)
        return r;
    r = read_reply(s, s->cmd, s->res_len, tries);
    if (r < 0) {
        LOGW("no reply to command %04x: %s", code, pixma_strerror(r));
        return r;
    }
    if (r < RES_HDR)
        return PIXMA_EPROTO;
    unsigned status = get_be16(s->cmd);
    s->command_rejected = status == STATUS_BUSY || status == STATUS_FAILED;
    int err = map_status(status);
    if (err == PIXMA_OK && (size_t)r != s->res_len)
        err = PIXMA_ECANCELED;      // a bare "ok" header: the command was cut short (SANE)
    else if (err == PIXMA_OK && s->res_len > RES_HDR && sum_bytes(s->cmd + RES_HDR, (size_t)r - RES_HDR) != 0)
        err = PIXMA_EPROTO;
    if (err == PIXMA_EPROTO)
        LOGW("bad reply to command %04x: %d bytes, status %04x", code, r, get_be16(s->cmd));
    return err;
}

static int exec(scan_t *s)
{
    return exec_tries(s, RESPONSE_TRIES);
}

static int exec_short(scan_t *s, unsigned code)
{
    new_cmd(s, code, 0, 0);
    return exec(s);
}

static int query_status(scan_t *s, int tries)
{
    new_cmd(s, CMD_STATUS, 0, 12);
    int err = exec_tries(s, tries);
    if (err >= 0)
        memcpy(s->status, s->cmd + RES_HDR, sizeof(s->status));
    return err;
}

// "paper" in the status as SANE's has_paper (the ADF; a flatbed scan ends
// the session either way)
static bool has_paper(const scan_t *s)
{
    return (s->status[1] & 0x0f) == 0 || s->status[1] == 0x51;
}

// Reads and discards whatever the scanner still sends.
static bool drain(scan_t *s)
{
    int zlp = 0;
    for (int i = 0; i < 2048; i++) {
        int r = s->io->read(s->io->ctx, s->chunk, PIXMA_READ_CHUNK, IO_TIMEOUT_MS);
        if (r < 0)
            return r == PIXMA_ETIMEDOUT;
        if (r == 0 && ++zlp > MAX_ZLP)
            return false;
        if (r > 0)
            zlp = 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// commands

static int select_source(scan_t *s)
{
    uint8_t *d = new_cmd(s, CMD_SELECT_SOURCE, 10, 0);
    d[0] = 1;                   // flatbed
    return exec(s);
}

static int send_scan_param(scan_t *s)
{
    const pixma_params_t *p = s->p;
    uint8_t *d = new_cmd(s, CMD_SCAN_PARAM, 0x2e, 0);
    put_be16(d + 0x04, (unsigned)p->dpi | 0x1000);
    put_be16(d + 0x06, (unsigned)p->dpi | 0x1000);
    put_be32(d + 0x08, (uint32_t)p->x);
    put_be32(d + 0x0c, (uint32_t)p->y);
    put_be32(d + 0x10, (uint32_t)s->wx);
    put_be32(d + 0x14, (uint32_t)s->h);
    d[0x18] = 0x08;             // colour
    d[0x19] = 24;               // bits per pixel
    d[0x1f] = 0x7f;
    d[0x20] = 0xff;
    d[0x23] = 0x81;
    return exec(s);
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
            if (s->lines < s->h && !s->stop && !canceled(s)) {
                // columns past the glass: white
                memset(s->line + 3 * s->w, 0xff, 3 * (size_t)(s->p->w - s->w));
                if (s->line_cb(s->user, s->lines, s->line))
                    s->stop = true;
                s->lines++;
            }
        }
    }
}

// One image request (request_image_block + read_image_block). flag: 0 for the
// first request after the parameters, 4 afterwards. Returns the block length
// read (0: not ready yet), or < 0. s->last_flags gets the end flags.
static int read_block(scan_t *s, uint8_t flag)
{
    uint8_t cmd[11] = {0};
    put_be16(cmd, CMD_READ_IMAGE2);
    cmd[8] = flag;
    cmd[10] = 0x06;
    int r = write_all(s, cmd, sizeof(cmd));
    if (r < 0)
        return r;
    int got = read_reply(s, s->chunk, BLOCK_REPLY, RESPONSE_TRIES);
    if (got < 0)
        return got;
    if (got < BLOCK_HDR)
        return PIXMA_EPROTO;
    int err = map_status(get_be16(s->chunk));
    if (err < 0)
        return err;
    uint8_t info = s->chunk[2];
    uint32_t len = get_be32(s->chunk + 4);
    if (info & ~0x38)
        LOGW("unexpected image block info %02x", info);
    uint32_t have = (uint32_t)got - BLOCK_HDR;
    if (have > len)
        return PIXMA_EPROTO;
    if (flag == 0 && len) {
        LOGW("image data before the first block request");
        return PIXMA_EPROTO;
    }
    s->last_flags = info & 0x38;
    feed(s, s->chunk + BLOCK_HDR, have);
    uint32_t left = len - have;
    int tries = RESPONSE_TRIES, zlp = 0;
    while (left) {
        if (canceled(s) || s->stop)
            return PIXMA_ECANCELED;
        // SANE reads 16 KB pieces (generation 2), multiples of 512
        size_t n = left < PIXMA_READ_CHUNK ? left : PIXMA_READ_CHUNK;
        r = s->io->read(s->io->ctx, s->chunk, n, IO_TIMEOUT_MS);
        if (r == PIXMA_ETIMEDOUT && --tries > 0)
            continue;
        if (r < 0)
            return r;
        if (r == 0) {
            if (++zlp > MAX_ZLP)
                return PIXMA_ETIMEDOUT;
            continue;           // stray zero-length packet
        }
        if ((size_t)r > n)
            return PIXMA_EPROTO;
        tries = RESPONSE_TRIES;
        zlp = 0;
        feed(s, s->chunk, (size_t)r);
        left -= (uint32_t)r;     // short transfers can continue the same block
    }
    return (int)len;
}

static int read_image(scan_t *s)
{
    int idle_ms = 0;
    s->last_flags = 0;
    while (s->last_flags != 0x28 && s->last_flags != 0x38) {   // last block (0x38: and the ADF is empty)
        if (canceled(s) || s->stop)
            return PIXMA_ECANCELED;
        int r = read_block(s, 4);
        if (r < 0) {
            bool drained = drain(s); // consume remaining raw data before another command
            if (drained && !canceled(s) && !s->stop) {
                // seen in traffic logs after a failed block (SANE: meaning unknown)
                new_cmd(s, CMD_ERROR_INFO, 0, 16);
                exec(s);
            }
            return r;
        }
        if (r == 0 && !s->last_flags) {
            if ((idle_ms += NOT_READY_MS) > NOT_READY_TIMEOUT_S * 1000)
                return PIXMA_ETIMEDOUT;
            sleep_ms(s, NOT_READY_MS);  // no data at this moment (warm-up)
        } else {
            idle_ms = 0;
        }
    }
    return canceled(s) || s->stop ? PIXMA_ECANCELED : PIXMA_OK;
}

// iclass_finish_scan: abort, read the status twice, abort again. SANE sends
// the second abort only when the ADF has nothing left to feed (else it waits
// for the next sheet); a flatbed scan has no next sheet, so it always goes.
static int finish(scan_t *s)
{
    int e1 = exec_short(s, CMD_ABORT_SESSION);
    query_status(s, RESPONSE_TRIES);
    int e2 = query_status(s, RESPONSE_TRIES);
    int e3 = exec_short(s, CMD_ABORT_SESSION);
    LOGI("session ended: abort %s, status %s (%02x %02x %02x %02x %02x %02x %02x %02x, ADF paper %d), abort %s",
         pixma_strerror(e1), pixma_strerror(e2), s->status[0], s->status[1], s->status[2], s->status[3], s->status[4],
         s->status[5], s->status[6], s->status[7], has_paper(s), pixma_strerror(e3));
    // Either acknowledged abort releases the flatbed session. If both fail,
    // a device rejection means busy, rather than a user-requested cancellation.
    if (e1 >= 0 || e3 >= 0)
        return PIXMA_OK;
    if (e1 == PIXMA_ECANCELED && e3 == PIXMA_ECANCELED)
        return PIXMA_EBUSY;
    return e3 != PIXMA_ECANCELED ? e3 : e1;
}

// ---------------------------------------------------------------------------

int iclass_scan(const pixma_io_t *io, const pixma_params_t *p, pixma_line_fn line, void *user,
                volatile bool *cancel)
{
    if (p->dpi != 75 && p->dpi != 150 && p->dpi != 300 && p->dpi != 600)
        return PIXMA_EINVAL;
    int max_w = ICLASS_MAX_WIDTH_75 * p->dpi / 75, max_h = ICLASS_MAX_HEIGHT_75 * p->dpi / 75;
    if (p->x < 0 || p->y < 0 || p->w <= 0 || p->h <= 0 || p->x >= max_w || p->y >= max_h)
        return PIXMA_EINVAL;

    scan_t s = {.io = io, .p = p, .cancel = cancel, .line_cb = line, .user = user};
    s.w = p->w < max_w - p->x ? p->w : max_w - p->x;
    s.wx = (s.w + 31) & ~31;
    s.h = p->h < max_h - p->y ? p->h : max_h - p->y;
    s.raw_line = (size_t)s.wx * 3;
    s.cmd = malloc(CMDBUF_SIZE);
    s.chunk = malloc(PIXMA_READ_CHUNK);
    // the raw line, or the delivered one when the request is wider than the glass
    s.line = malloc(s.raw_line > (size_t)p->w * 3 ? s.raw_line : (size_t)p->w * 3);
    int err = PIXMA_ENOMEM;
    if (!s.cmd || !s.chunk || !s.line)
        goto out;

    LOGI("scan %d dpi, %dx%d at (%d,%d), raw line %d px", p->dpi, s.w, s.h, p->x, p->y, s.wx);
    // the first command: a short timeout, then once more (SANE's step1)
    err = query_status(&s, 2);
    if (err < 0)
        err = query_status(&s, RESPONSE_TRIES);
    if (err < 0)
        goto out;
    err = exec_short(&s, CMD_START_SESSION);
    if (err < 0) {
        // A transport error cannot establish whether START_SESSION reached
        // the device. Only an explicit status rejection rules out our session.
        if (!s.command_rejected) {
            drain(&s);
            finish(&s);
        }
        goto out;
    }
    // from here on the session is ended, whatever happens
    err = select_source(&s);
    if (err >= 0)
        err = send_scan_param(&s);
    if (err >= 0) {
        err = read_block(&s, 0);
        if (err < 0)
            drain(&s);
    }
    if (err >= 0)
        err = read_image(&s);
    if (err >= 0 && s.lines < s.h) {
        LOGW("short image: %d of %d lines", s.lines, s.h);
        err = PIXMA_EPROTO;
    }
    int cleanup = finish(&s);
    if (err >= 0)
        err = cleanup;
out:
    if (err < 0)
        LOGW("scan ended: %s after %d lines", pixma_strerror(err), s.lines);
    free(s.cmd);
    free(s.chunk);
    free(s.line);
    return err < 0 ? err : PIXMA_OK;
}
