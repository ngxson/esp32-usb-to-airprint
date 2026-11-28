// Emulated imageCLASS generation-2 scanner, written from SANE's description of
// the protocol (pixma_imageclass.c), independently of iclass_scan.c.
#include "fake_iclass.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fake_scanner.h"

static void violation(fake_iclass_t *f, const char *fmt, ...)
{
    if (!f->errors++) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(f->error, sizeof(f->error), fmt, ap);
        va_end(ap);
    }
}

static void note(fake_iclass_t *f, char c)
{
    size_t n = strlen(f->order);
    if (n + 1 < sizeof(f->order)) {
        f->order[n] = c;
        f->order[n + 1] = 0;
    }
}

void fake_iclass_init(fake_iclass_t *f)
{
    memset(f, 0, sizeof(*f));
    f->rng = 1;
}

uint8_t fake_iclass_byte(const fake_iclass_t *f, uint64_t i)
{
    uint64_t line = (uint64_t)f->wx * 3;
    int y = (int)(i / line), px = (int)(i % line) / 3, c = (int)(i % 3);
    uint8_t rgb[3];
    fake_scanner_pixel(f->dpi, f->x + px, f->y + y, rgb);
    return rgb[c];
}

static unsigned be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

// a reply: status, then data closed by its checksum byte
static void reply(fake_iclass_t *f, unsigned status, size_t datain)
{
    memset(f->resp, 0, 2 + datain);
    f->resp[0] = status >> 8;
    f->resp[1] = status & 0xff;
    if (datain) {
        uint8_t s = 0;
        for (size_t i = 2; i < 1 + datain; i++) {
            f->resp[i] = (uint8_t)(i * 7 + f->status_queries);
            s += f->resp[i];
        }
        f->resp[1 + datain] = (uint8_t)-s;
    }
    f->resp_len = 2 + datain;
    f->resp_pos = 0;
}

static uint32_t rnd(fake_iclass_t *f)
{
    f->rng = f->rng * 1103515245 + 12345;
    return f->rng >> 8;
}

static void image_request(fake_iclass_t *f, const uint8_t *d)
{
    for (int i = 2; i < 10; i++) {
        if (i != 8 && d[i]) {
            violation(f, "image request reserved byte %d is %02x", i, d[i]);
            return;
        }
    }
    if ((d[8] != 0 && d[8] != 4) || d[10] != 0x06) {
        violation(f, "image request flags %02x %02x", d[8], d[10]);
        return;
    }
    if (d[8] == 0) {
        note(f, '0');
        if (f->stage != 3)
            violation(f, "first image request out of order (stage %d)", f->stage);
        f->stage = 4;
        reply(f, 0x0606, 6);    // 8-byte header, no data
        memset(f->resp + 2, 0, 6);
        if (f->first_block_data) {
            f->resp[6] = 3;
            f->resp[7] = 0xe8;  // 1000 bytes, not allowed on the initial request
            f->raw_left = 1000;
        }
        return;
    }
    note(f, 'I');
    if (f->stage != 4)
        violation(f, "image request out of order (stage %d)", f->stage);
    memset(f->resp, 0, 8);
    f->resp[0] = f->resp[1] = 0x06;
    f->resp_len = 8;
    f->resp_pos = 0;
    if (f->not_ready > 0) {
        f->not_ready--;
        return;
    }
    if (++f->blocks == f->fail_at_block) {
        f->resp[0] = f->resp[1] = 0x15;
        return;
    }
    uint64_t left = f->image_total - f->image_bytes;
    // irregular blocks: tiny ones (header only), around 504 bytes, large ones
    uint32_t sizes[] = {300, 504, 505, 4096, 20000, 65536 + 17, 200000};
    uint64_t len = sizes[rnd(f) % 7];
    if (len > left)
        len = left;
    bool last = len == left;
    f->resp[2] = last ? 0x28 : 0x00;
    f->resp[4] = (uint8_t)(len >> 24);
    f->resp[5] = (uint8_t)(len >> 16);
    f->resp[6] = (uint8_t)(len >> 8);
    f->resp[7] = (uint8_t)len;
    size_t first = len >= 504 ? 504 : 0;   // a full 512-byte reply carries the block's first bytes
    for (size_t i = 0; i < first; i++)
        f->resp[8 + i] = fake_iclass_byte(f, f->image_bytes + i);
    f->resp_len = 8 + first;
    f->image_bytes += first;
    f->raw_left = len - first;
}

static void command(fake_iclass_t *f, const uint8_t *d, size_t len)
{
    if (len == 11 && be16(d) == 0xd460) {
        image_request(f, d);
        return;
    }
    if (len < 10) {
        violation(f, "command of %zu bytes", len);
        return;
    }
    unsigned code = be16(d), n = be16(d + 7);
    for (int i = 2; i < 7; i++)
        if (d[i])
            violation(f, "command %04x: header byte %d is %02x", code, i, d[i]);
    if (d[9])
        violation(f, "command %04x: header byte 9 is %02x", code, d[9]);
    size_t out = len - 10, in = 0, want_out = 0;
    switch (code) {
    case 0xf320: in = 12; break;
    case 0xff20: in = 16; break;
    case 0xdd20: want_out = 10; break;
    case 0xde20: want_out = 0x2e; break;
    case 0xdb20: case 0xef20: break;
    default: violation(f, "unknown command %04x", code); return;
    }
    if (out != want_out || n != out + in)
        violation(f, "command %04x: %zu bytes of data, length field %u", code, out, n);
    if (out) {
        uint8_t s = 0;
        for (size_t i = 0; i < out; i++)
            s += d[10 + i];
        if (s)
            violation(f, "command %04x: bad checksum", code);
    }
    const uint8_t *p = d + 10;
    if (code == 0xf320 && f->mute_status > 0) {
        f->mute_status--;
        note(f, 's');
        return;     // no reply: the host times out
    }
    if (code == 0xef20 && f->aborts < 32 && (f->fail_aborts & (1u << f->aborts))) {
        note(f, 'F');
        f->aborts++;
        reply(f, 0x1515, 0);
        return;
    }
    if (code == f->fail_command || code == f->cut_command) {
        note(f, code == f->fail_command ? 'F' : 'C');
        reply(f, code == f->fail_command ? 0x1515 : 0x0606, 0);
        if (code == 0xdb20 && code == f->cut_command)
            f->session_open = true;
        return;
    }
    switch (code) {
    case 0xf320:
        note(f, 'S');
        f->status_queries++;
        reply(f, 0x0606, 12);
        f->resp[3] = f->adf_paper ? 0x00 : 0x01;    // byte 1 of the data: ADF paper
        f->resp[13] = 0;
        {
            uint8_t s = 0;
            for (int i = 2; i < 13; i++)
                s += f->resp[i];
            f->resp[13] = (uint8_t)-s;
            if (f->corrupt_status && f->status_queries >= f->corrupt_status)
                f->resp[13] ^= 0x5a;
        }
        return;
    case 0xff20:
        note(f, 'E');
        f->error_infos++;
        reply(f, 0x0606, 16);
        return;
    case 0xdb20:
        note(f, 'B');
        if (f->session_open) {
            reply(f, 0x1414, 0);     // an existing session belongs to its current owner
            return;
        }
        f->session_open = true;
        f->stage = 1;
        f->image_bytes = f->image_total = 0;
        f->raw_left = 0;
        f->blocks = 0;
        reply(f, 0x0606, 0);
        f->pending_read_error = f->start_reply_error;
        f->start_reply_error = 0;
        if (f->mute_start) {
            f->mute_start = false;
            f->resp_len = 0;
        }
        return;
    case 0xdd20:
        note(f, 'R');
        if (f->stage != 1)
            violation(f, "select source out of order (stage %d)", f->stage);
        if (p[0] != 1 || p[5] != 0)
            violation(f, "select source %02x %02x: not the flatbed", p[0], p[5]);
        f->stage = 2;
        reply(f, 0x0606, 0);
        return;
    case 0xde20:
        note(f, 'P');
        if (f->stage != 2)
            violation(f, "scan parameters out of order (stage %d)", f->stage);
        if (be16(p + 4) != be16(p + 6) || !(be16(p + 4) & 0x1000))
            violation(f, "dpi fields %04x %04x", be16(p + 4), be16(p + 6));
        f->dpi = be16(p + 4) & 0x0fff;
        f->x = (int)be32(p + 8);
        f->y = (int)be32(p + 12);
        f->wx = (int)be32(p + 16);
        f->h = (int)be32(p + 20);
        if (f->wx % 32 || f->wx <= 0 || f->h <= 0)
            violation(f, "raw width %d, height %d", f->wx, f->h);
        if (f->x + f->wx > 637 * f->dpi / 75 + 31 || f->y + f->h > 877 * f->dpi / 75)
            violation(f, "area %d+%d x %d+%d beyond the glass at %d dpi", f->x, f->wx, f->y, f->h, f->dpi);
        if (p[0x18] != 0x08 || p[0x19] != 24 || p[0x1f] != 0x7f || p[0x20] != 0xff || p[0x23] != 0x81)
            violation(f, "scan parameters: mode %02x bpp %d", p[0x18], p[0x19]);
        f->image_total = (uint64_t)f->wx * 3 * (f->h - f->short_lines);
        f->stage = 3;
        reply(f, 0x0606, 0);
        return;
    case 0xef20:
        note(f, 'A');
        f->aborts++;
        if (f->session_open && f->image_total && f->image_bytes == f->image_total && !f->raw_left)
            f->scans++;
        f->session_open = false;
        f->stage = 0;
        reply(f, 0x0606, 0);
        return;
    }
}

static int io_write(void *ctx, const uint8_t *data, size_t len, int timeout_ms)
{
    fake_iclass_t *f = ctx;
    if (be16(data) == 0xdb20 && f->fail_start_write) {
        f->fail_start_write = false;
        return PIXMA_EIO;
    }
    // SANE finishes an interrupted scan by aborting, including during raw data.
    if ((f->resp_pos < f->resp_len || f->raw_left) && be16(data) != 0xef20)
        violation(f, "command %04x while %zu reply bytes are unread", be16(data),
                  (f->resp_len - f->resp_pos) + (size_t)f->raw_left);
    f->resp_len = f->resp_pos = 0;
    f->raw_left = 0;
    command(f, data, len);
    if (be16(data) == 0xdb20 && f->accepted_start_write_error) {
        int err = f->accepted_start_write_error;
        f->accepted_start_write_error = 0;
        return err;
    }
    return (int)len;
}

static int io_read(void *ctx, uint8_t *buf, size_t len, int timeout_ms)
{
    fake_iclass_t *f = ctx;
    if (f->pending_read_error) {
        int err = f->pending_read_error;
        f->pending_read_error = 0;
        return err;
    }
    if (f->resp_pos < f->resp_len) {    // a reply is one transfer
        size_t n = f->resp_len - f->resp_pos;
        if (n > len)
            n = len;
        memcpy(buf, f->resp + f->resp_pos, n);
        f->resp_pos += n;
        return (int)n;
    }
    if (f->raw_left) {
        f->raw_reads++;
        if (f->cancel && f->raw_reads == f->cancel_at_read)
            *f->cancel = true;
        if (f->zero_reads > 0) {
            f->zero_reads--;
            return 0;
        }
        size_t n = f->raw_left < len ? (size_t)f->raw_left : len;
        if (f->raw_chunk && n > f->raw_chunk)
            n = f->raw_chunk;
        for (size_t i = 0; i < n; i++)
            buf[i] = fake_iclass_byte(f, f->image_bytes + i);
        f->image_bytes += n;
        f->raw_left -= n;
        return (int)n;
    }
    return PIXMA_ETIMEDOUT;
}

static void io_sleep(void *ctx, int ms)
{
}

pixma_io_t fake_iclass_io(fake_iclass_t *f)
{
    return (pixma_io_t){.write = io_write, .read = io_read, .sleep_ms = io_sleep, .ctx = f};
}
