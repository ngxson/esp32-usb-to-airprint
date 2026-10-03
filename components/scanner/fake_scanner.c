// Device side of the pixma protocol (see pixma_scan.c), strict about what
// the host sends, with USB-like reads: a reply can be read in pieces of a
// multiple of 64 bytes, a reply whose length is a multiple of 64 is followed
// by a zero-length packet, and image blocks have irregular sizes that do not
// line up with the image lines.
#include "fake_scanner.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ST_IDLE, ST_JOB, ST_MODE, ST_SESSION, ST_PARAMS, ST_SCANNING };

#define MAX_BLOCK 20000             // image block sizes 1..MAX_BLOCK
#define RESP_CAP (MAX_BLOCK + 64)   // also holds the XML replies
#define CALIBRATION_POLLS 2

static void violation(fake_scanner_t *f, const char *fmt, ...)
{
    char msg[sizeof(f->error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "E fake_scanner: %s\n", msg);
    if (!f->errors++)
        snprintf(f->error, sizeof(f->error), "%s", msg);
}

static unsigned be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
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

static uint32_t rnd(fake_scanner_t *f)
{
    f->rng = f->rng * 1103515245u + 12345u;
    return f->rng >> 8;
}

// ---------------------------------------------------------------------------
// test page, in 1/600 inch: frame, colour bars, grey and hue ramps, "text",
// a shaded disc and a slanted line

void fake_scanner_pixel(int dpi, int x, int y, uint8_t rgb[3])
{
    static const uint8_t bars[8][3] = {
        {220, 30, 30}, {30, 170, 60}, {30, 60, 200}, {0, 170, 220},
        {210, 0, 140}, {250, 220, 0}, {25, 25, 25},  {128, 128, 128},
    };
    int s = 600 / dpi;
    int u = x * s + s / 2, v = y * s + s / 2;
    int r = 250, g = 250, b = 245;      // paper

    bool frame_u = u >= 300 && u < 4800, frame_v = v >= 300 && v < 6716;
    if ((frame_v && ((u >= 300 && u < 312) || (u >= 4788 && u < 4800))) ||
        (frame_u && ((v >= 300 && v < 312) || (v >= 6704 && v < 6716)))) {
        r = g = b = 20;
    } else if (u >= 600 && u < 4504 && v >= 600 && v < 1200) {
        const uint8_t *c = bars[(u - 600) / 488];
        r = c[0], g = c[1], b = c[2];
    } else if (u >= 600 && u < 4504 && v >= 1300 && v < 1600) {
        r = g = b = (u - 600) * 255 / 3903;
    } else if (u >= 600 && u < 4504 && v >= 1700 && v < 2000) {
        int t = (u - 600) * 1536 / 3904, k = t & 255;     // hue: 6 segments of 256
        int up = k, down = 255 - k;
        switch (t >> 8) {
        case 0: r = 255, g = up, b = 0; break;
        case 1: r = down, g = 255, b = 0; break;
        case 2: r = 0, g = 255, b = up; break;
        case 3: r = 0, g = down, b = 255; break;
        case 4: r = up, g = 0, b = 255; break;
        default: r = 255, g = 0, b = down; break;
        }
    } else if (u >= 600 && u < 3000 && v >= 2300 && v < 6200 && (v - 2300) % 150 < 60) {
        uint32_t line = (uint32_t)(v - 2300) / 150, h = line * 2654435761u;
        int pos = 600;
        while (pos < 3000) {        // words of 80..480 units, 50 apart
            h = h * 1103515245u + 12345u;
            int w = 80 + (int)((h >> 16) % 400);
            if (u < pos + w) {
                if (u >= pos)
                    r = g = b = 35;
                break;
            }
            pos += w + 50;
        }
    } else {
        int du = u - 3900, dv = v - 4200;
        int d2 = du * du + dv * dv;
        if (d2 < 700 * 700) {
            r = 40 + (u - 3200) * 200 / 1400;
            g = 60;
            b = 240 - (v - 3500) * 200 / 1400;
        }
    }
    // slanted line from (600, 6300) to (4500, 6600), 8 units thick
    if (u >= 600 && u < 4500) {
        int lv = 6300 + (u - 600) * 300 / 3900;
        if (v >= lv && v < lv + 8)
            r = g = b = 0;
    }
    rgb[0] = (uint8_t)r;
    rgb[1] = (uint8_t)g;
    rgb[2] = (uint8_t)b;
}

// ---------------------------------------------------------------------------
// replies

static uint8_t *reply(fake_scanner_t *f, unsigned status, size_t datain)
{
    memset(f->resp, 0, 8 + datain);
    f->resp[0] = (uint8_t)(status >> 8);
    f->resp[1] = (uint8_t)status;
    if (status == 0x1414)
        f->resp[3] = 0x21;
    f->resp_len = 8 + datain;
    f->resp_pos = 0;
    return f->resp + 8;
}

static void reply_ok(fake_scanner_t *f)
{
    reply(f, 0x0606, 0);
}

static void reply_failed(fake_scanner_t *f)
{
    reply(f, 0x1515, 0);
}

static void seal(fake_scanner_t *f)
{
    f->resp[f->resp_len - 1] = (uint8_t)-sum_bytes(f->resp + 8, f->resp_len - 9);
}

static void xml_reply(fake_scanner_t *f, const char *op, bool ok, const char *detail)
{
    int n = snprintf((char *)f->resp, RESP_CAP,
                     "<?xml version=\"1.0\" encoding=\"utf-8\" ?><cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/"
                     "2008/07/common/\"><ivec:contents><ivec:operation>%sResponse</ivec:operation>"
                     "<ivec:param_set servicetype=\"scan\"><ivec:response>%s</ivec:response>"
                     "<ivec:response_detail>%s</ivec:response_detail></ivec:param_set></ivec:contents></cmd>",
                     op, ok ? "OK" : "NG", detail);
    f->resp_len = (size_t)n;
    f->resp_pos = 0;
}

static void handle_xml(fake_scanner_t *f, const uint8_t *data, size_t len)
{
    char x[1024];
    if (len >= sizeof(x)) {
        violation(f, "XML command of %u bytes", (unsigned)len);
        len = sizeof(x) - 1;
    }
    memcpy(x, data, len);
    x[len] = 0;
    if (!strstr(x, "servicetype=\"scan\""))
        violation(f, "XML command without servicetype scan");
    if (strstr(x, "<ivec:operation>StartJob</ivec:operation>")) {
        if (f->job_open) {
            violation(f, "StartJob while the previous job is open");
            xml_reply(f, "StartJob", false, "PCScanning");
            return;
        }
        f->job_open = true;
        f->stage = ST_JOB;
        xml_reply(f, "StartJob", true, "");
    } else if (strstr(x, "<vcn:ijoperation>ModeShift</vcn:ijoperation>")) {
        if (f->stage != ST_JOB)
            violation(f, "ModeShift outside a fresh job");
        f->stage = ST_MODE;
        xml_reply(f, "VendorCmd", true, "");
    } else if (strstr(x, "<ivec:operation>EndJob</ivec:operation>")) {
        if (!f->job_open)
            violation(f, "EndJob without a job");
        if (f->session_open)
            violation(f, "EndJob while the scan session is open");
        if (f->total && f->image_bytes == f->total && f->job_open && !f->session_open)
            f->scans++;
        f->job_open = false;
        f->total = 0;
        f->stage = ST_IDLE;
        xml_reply(f, "EndJob", true, "");
    } else {
        violation(f, "unknown XML command");
        xml_reply(f, "Unknown", false, "");
    }
}

static void gen_image(fake_scanner_t *f, uint8_t *out, size_t n)
{
    size_t line_bytes = (size_t)f->wx * 3;
    int max_w = PIXMA_MAX_WIDTH_75 * f->dpi / 75;
    while (n) {
        if (f->raw_pos == line_bytes) {
            f->raw_y++;
            f->raw_pos = 0;
            for (int px = 0; px < f->wx; px++) {
                uint8_t *p = f->raw + 3 * px;
                if (f->x + px < max_w)
                    fake_scanner_pixel(f->dpi, f->x + px, f->y + f->raw_y, p);
                else
                    p[0] = p[1] = p[2] = 0;     // beyond the glass
            }
        }
        size_t k = line_bytes - f->raw_pos;
        if (k > n)
            k = n;
        memcpy(out, f->raw + f->raw_pos, k);
        f->raw_pos += k;
        out += k;
        n -= k;
    }
}

static void read_image(fake_scanner_t *f, uint32_t max)
{
    if (f->stage != ST_SCANNING || f->status_polls < CALIBRATION_POLLS) {
        violation(f, "image read before the scanner was ready");
        reply_failed(f);
        return;
    }
    if (++f->block_no == f->fail_at_block) {
        reply_failed(f);
        return;
    }
    if (f->image_bytes == f->total)
        violation(f, "image read after the end of the image");
    uint64_t left = f->total - f->image_bytes;
    uint32_t n = f->block_no % 5 == 0 ? 0 : 1 + rnd(f) % MAX_BLOCK;    // now and then: no data yet
    if (n > max)
        n = max;
    if (n > left)
        n = (uint32_t)left;
    uint8_t *h = reply(f, 0x0606, 8);
    gen_image(f, h + 8, n);
    f->image_bytes += n;
    h[0] = f->image_bytes == f->total ? 0x28 : 0x00;
    h[4] = (uint8_t)(n >> 24);
    h[5] = (uint8_t)(n >> 16);
    h[6] = (uint8_t)(n >> 8);
    h[7] = (uint8_t)n;
    f->resp_len = 16 + n;
}

static void scan_param(fake_scanner_t *f, const uint8_t *d)
{
    unsigned xdpi = be16(d + 0x08), ydpi = be16(d + 0x0a);
    int dpi = (int)(xdpi & 0x7fff);
    int x = (int)be32(d + 0x0c), y = (int)be32(d + 0x10), wx = (int)be32(d + 0x14), h = (int)be32(d + 0x18);
    int max_w = PIXMA_MAX_WIDTH_75 * dpi / 75, max_h = PIXMA_MAX_HEIGHT_75 * dpi / 75;
    if (d[0] != 0x01 || !(xdpi & 0x8000) || xdpi != ydpi || (dpi != 75 && dpi != 150 && dpi != 300 && dpi != 600) ||
        d[0x1c] != 0x08 || d[0x1d] != 24 || x % 32 || wx % 32 || wx <= 0 || h <= 0 || x + wx > max_w + 31 ||
        y + h > max_h) {
        violation(f, "bad scan parameters: dpi %04x/%04x x %d y %d wx %d h %d mode %02x/%u", xdpi, ydpi, x, y, wx, h,
                  d[0x1c], d[0x1d]);
        reply_failed(f);
        return;
    }
    uint8_t *raw = realloc(f->raw, (size_t)wx * 3);
    if (!raw) {
        reply_failed(f);
        return;
    }
    f->raw = raw;
    f->dpi = dpi;
    f->x = x;
    f->y = y;
    f->wx = wx;
    f->h = h;
    f->total = (uint64_t)wx * 3 * h;
    f->image_bytes = 0;
    f->raw_y = -1;
    f->raw_pos = (size_t)wx * 3;
    f->stage = ST_PARAMS;
    reply_ok(f);
}

static void handle_cmd(fake_scanner_t *f, const uint8_t *d, size_t len)
{
    if (len < 16) {
        violation(f, "short command (%u bytes)", (unsigned)len);
        reply_failed(f);
        return;
    }
    unsigned code = be16(d);
    for (int i = 2; i < 12; i++) {
        if (d[i]) {
            violation(f, "command %04x: header byte %d is %02x", code, i, d[i]);
            break;
        }
    }
    uint32_t plen = be32(d + 12);
    size_t out = len - 16;
    size_t want_out = 0, want_in = 0;
    switch (code) {
    case 0xee20: want_out = 2048 + 8; break;
    case 0xd820: want_out = 0x38; break;
    case 0xda20: want_in = 8; break;
    case 0xff20: want_in = 16; break;
    case 0xdb20: case 0xef20: case 0xd920: case 0xd420: break;
    default:
        violation(f, "unknown command %04x", code);
        reply_failed(f);
        return;
    }
    if (out != want_out || (code != 0xd420 && plen != want_out + want_in)) {
        violation(f, "command %04x: %u payload bytes, length field %u", code, (unsigned)out, (unsigned)plen);
        reply_failed(f);
        return;
    }
    if (out && sum_bytes(d + 16, out) != 0) {
        violation(f, "command %04x: bad checksum", code);
        reply_failed(f);
        return;
    }
    const uint8_t *p = d + 16;
    switch (code) {
    case 0xdb20:    // start session
        if (f->stage != ST_MODE || f->session_open)
            violation(f, "start session in the wrong state");
        if (f->busy_starts > 0) {
            f->busy_starts--;
            reply(f, 0x1414, 0);
            return;
        }
        f->session_open = true;
        f->stage = ST_SESSION;
        f->gamma_count = 0;
        reply_ok(f);
        break;
    case 0xee20: {  // gamma table
        bool mono = true;
        for (int i = 1; i < 1024; i++)
            if (p[4 + 2 * i] + 256 * p[5 + 2 * i] < p[2 + 2 * i] + 256 * p[3 + 2 * i])
                mono = false;
        if (f->stage != ST_SESSION || p[0] != 0x10 || be16(p + 2) != 0x0804 || !mono)
            violation(f, "bad gamma table command");
        f->gamma_count++;
        reply_ok(f);
        break;
    }
    case 0xd820:    // scan parameters
        if (f->stage != ST_SESSION || f->gamma_count != 3) {
            violation(f, "scan parameters in the wrong state (%d gamma tables)", f->gamma_count);
            reply_failed(f);
            return;
        }
        scan_param(f, p);
        break;
    case 0xd920:    // start scan
        if (f->stage != ST_PARAMS)
            violation(f, "start scan without parameters");
        f->stage = ST_SCANNING;
        f->status_polls = 0;
        f->block_no = 0;
        reply_ok(f);
        break;
    case 0xda20: {  // status: calibrated after a few polls
        uint8_t *s = reply(f, 0x0606, 8);
        if (f->stage == ST_SCANNING && ++f->status_polls >= CALIBRATION_POLLS)
            s[0] = 0x01;
        seal(f);
        break;
    }
    case 0xd420:    // read image
        read_image(f, plen >= 8 ? plen - 8 : 0);
        break;
    case 0xef20:    // abort session
        f->session_open = false;
        f->stage = f->job_open ? ST_MODE : ST_IDLE;
        reply_ok(f);
        break;
    case 0xff20:    // error info
        reply(f, 0x0606, 16);
        seal(f);
        break;
    }
}

// ---------------------------------------------------------------------------
// I/O

static int fs_write(void *ctx, const uint8_t *data, size_t len, int timeout_ms)
{
    fake_scanner_t *f = ctx;
    if (f->resp_pos < f->resp_len)
        violation(f, "command sent with %u reply bytes unread", (unsigned)(f->resp_len - f->resp_pos));
    f->resp_len = f->resp_pos = 0;
    if (len >= 5 && memcmp(data, "<?xml", 5) == 0)
        handle_xml(f, data, len);
    else
        handle_cmd(f, data, len);
    return (int)len;
}

static int fs_read(void *ctx, uint8_t *buf, size_t len, int timeout_ms)
{
    fake_scanner_t *f = ctx;
    if (f->zlp_pending) {
        f->zlp_pending = false;
        return 0;
    }
    size_t left = f->resp_len - f->resp_pos;
    if (!left) {
        if (f->bytes_per_s)
            usleep((useconds_t)timeout_ms * 1000);
        return PIXMA_ETIMEDOUT;
    }
    if (len > PIXMA_READ_CHUNK)
        violation(f, "read of %u bytes, more than one transfer", (unsigned)len);
    if (len < left && len % 64)
        violation(f, "read of %u bytes in the middle of a reply (not a multiple of 64)", (unsigned)len);
    size_t n = len < left ? len : left;
    memcpy(buf, f->resp + f->resp_pos, n);
    f->resp_pos += n;
    if (f->resp_pos == f->resp_len && f->resp_len % 64 == 0)
        f->zlp_pending = true;
    if (f->bytes_per_s)
        usleep((useconds_t)((uint64_t)n * 1000000 / f->bytes_per_s));
    return (int)n;
}

static void fs_sleep(void *ctx, int ms)
{
    fake_scanner_t *f = ctx;
    if (f->bytes_per_s)
        usleep((useconds_t)ms * 1000);
}

int fake_scanner_init(fake_scanner_t *f, unsigned bytes_per_s)
{
    memset(f, 0, sizeof(*f));
    f->bytes_per_s = bytes_per_s;
    f->rng = 1;
    f->resp = malloc(RESP_CAP);
    return f->resp ? 0 : -1;
}

void fake_scanner_free(fake_scanner_t *f)
{
    free(f->resp);
    free(f->raw);
    f->resp = f->raw = NULL;
}

pixma_io_t fake_scanner_io(fake_scanner_t *f)
{
    return (pixma_io_t){.write = fs_write, .read = fs_read, .sleep_ms = fs_sleep, .ctx = f};
}
