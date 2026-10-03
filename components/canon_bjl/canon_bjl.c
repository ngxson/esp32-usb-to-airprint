// Canon PIXMA MG2500-series raster stream generator. See canon_bjl.h.
#include "canon_bjl.h"

#include <stdlib.h>
#include <string.h>

// IVEC job wrapper, byte-identical to what Gutenprint sends for this model
// family (print-canon.h prexml_iP2700 / postxml_iP2700). Canon's own driver
// uses the same operations.
static const char XML_START_JOB[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?><cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\">"
    "<ivec:contents><ivec:operation>StartJob</ivec:operation><ivec:param_set servicetype=\"print\">"
    "<ivec:jobID>00000001</ivec:jobID><ivec:bidi>0</ivec:bidi></ivec:param_set></ivec:contents></cmd>";

static const char XML_MODE_SHIFT[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?><cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\" "
    "xmlns:vcn=\"http://www.canon.com/ns/cmd/2008/07/canon/\"><ivec:contents><ivec:operation>VendorCmd</ivec:operation>"
    "<ivec:param_set servicetype=\"print\"><vcn:ijoperation>ModeShift</vcn:ijoperation><vcn:ijmode>1</vcn:ijmode>"
    "<ivec:jobID>00000001</ivec:jobID></ivec:param_set></ivec:contents></cmd>";

static const char XML_END_JOB[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?><cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\">"
    "<ivec:contents><ivec:operation>EndJob</ivec:operation><ivec:param_set servicetype=\"print\">"
    "<ivec:jobID>00000001</ivec:jobID></ivec:param_set></ivec:contents></cmd>";

// Gutenprint canon_PIXMA_MG2400_modes: "300x300dpi" (inkset 13_C2M2Y2K2,
// MODE_FLAG_IP8500) and "600x600dpi" (inkset 13_C3M3Y2K2).
static const bjl_mode_t MODES[BJL_MODE_COUNT] = {
    [BJL_MODE_300DPI] = {"300x300dpi", 300, 1, {0x80, 0x00, 0x01}, {1, 1, 1, 1}, {2, 2, 2, 2}},
    [BJL_MODE_600DPI] = {"600x600dpi", 600, 2, {0x80, 0x80, 0x01}, {2, 2, 1, 1}, {3, 3, 2, 2}},
};

// Paper sizes as Gutenprint sends them. The exact 1/600" dimensions matter:
// the firmware uses them to recognise the paper size.
static const bjl_paper_t PAPERS[BJL_PAPER_COUNT] = {
    [BJL_PAPER_A4] = {"A4", 595, 842, 0x03, 4961, 7016},
    [BJL_PAPER_LETTER] = {"Letter", 612, 792, 0x0d, 5100, 6600},
};
#define BJL_MEDIA_PLAIN 0x00

const bjl_mode_t *bjl_get_mode(bjl_mode_id_t id)
{
    return (id >= 0 && id < BJL_MODE_COUNT) ? &MODES[id] : NULL;
}

const bjl_paper_t *bjl_get_paper(bjl_paper_id_t id)
{
    return (id >= 0 && id < BJL_PAPER_COUNT) ? &PAPERS[id] : NULL;
}

static int area_w_pt(const bjl_paper_t *p)
{
    return p->w_pt - BJL_BORDER_LEFT_PT - BJL_BORDER_RIGHT_PT;
}

static int area_h_pt(const bjl_paper_t *p)
{
    return p->h_pt - BJL_BORDER_TOP_PT - BJL_BORDER_BOTTOM_PT;
}

BJL_HOT static int emit(bjl_job_t *j, const void *data, size_t len)
{
    if (j->err)
        return j->err;
    if (len == 0)
        return 0;
    int r = j->sink(j->user, (const uint8_t *)data, len);
    if (r < 0) {
        j->err = r;
        return r;
    }
    j->bytes_out += len;
    return 0;
}

// ESC ( <cmd> <len16 le> <payload>
BJL_HOT static int emit_cmd(bjl_job_t *j, char cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[5] = {0x1b, '(', (uint8_t)cmd, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
    emit(j, hdr, sizeof(hdr));
    return emit(j, payload, len);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

BJL_HOT size_t bjl_packbits(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < len) {
        // literal run up to the next 3 identical bytes; when there is none,
        // the last (up to) 2 bytes are left for the repeat-run step below
        size_t lit = i;
        while (i + 2 < len && !(in[i] == in[i + 1] && in[i + 1] == in[i + 2]))
            i++;
        while (lit < i) {
            size_t n = i - lit > 128 ? 128 : i - lit;
            out[o++] = (uint8_t)(n - 1);
            memcpy(out + o, in + lit, n);
            o += n;
            lit += n;
        }
        // repeat run (may be a single byte)
        uint8_t v = in[i];
        size_t start = i;
        while (i < len && in[i] == v)
            i++;
        size_t count = i - start;
        while (count > 0) {
            size_t n = count > 128 ? 128 : count;
            out[o++] = (uint8_t)(1 - (int)n);
            out[o++] = v;
            count -= n;
        }
    }
    return o;
}

// 8x8 Bayer matrix for ordered dithering
static const uint8_t BAYER8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

BJL_HOT void bjl_dither_line(const uint8_t *d, int width, int bits, int y, uint8_t *out, int out_bytes)
{
    // thresholds of this matrix row, same values as v > bayer * 4 + 2
    uint8_t th[8];
    for (int k = 0; k < 8; k++)
        th[k] = (uint8_t)(BAYER8[y & 7][k] * 4 + 2);
    if (bits == 1) {
        int x = 0, o = 0;
        for (; x + 8 <= width; x += 8, o++) {     // whole output bytes
            const uint8_t *p = d + x;
            // no ink in these 8 pixels (the common case on text pages)
            if (!(((uintptr_t)p & 3) ? (p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7])
                                      : (((const uint32_t *)p)[0] | ((const uint32_t *)p)[1]))) {
                out[o] = 0;
                continue;
            }
            out[o] = (uint8_t)((p[0] > th[0]) << 7 | (p[1] > th[1]) << 6 | (p[2] > th[2]) << 5 |
                               (p[3] > th[3]) << 4 | (p[4] > th[4]) << 3 | (p[5] > th[5]) << 2 |
                               (p[6] > th[6]) << 1 | (p[7] > th[7]));
        }
        if (o < out_bytes) {
            uint8_t b = 0;
            for (int k = 0; x < width; x++, k++)
                b |= (uint8_t)((d[x] > th[k]) << (7 - k));
            out[o++] = b;
        }
        if (o < out_bytes)
            memset(out + o, 0, out_bytes - o);
    } else {
        // 3 levels: s = 2v in 0..510; base level s / 255, +1 if the
        // remainder beats the threshold
        int o = 0;
        for (int x = 0; x < width; x += 4, o++) {
            if (x + 4 <= width && !(d[x] | d[x + 1] | d[x + 2] | d[x + 3])) {
                out[o] = 0;     // no ink in these 4 pixels
                continue;
            }
            uint8_t b = 0;
            for (int k = 0; k < 4 && x + k < width; k++) {
                int s = d[x + k] * 2;
                int lvl = s >= 510 ? 2 : s >= 255 ? 1 : 0;
                if (lvl < 2 && s - lvl * 255 > th[(x + k) & 7])
                    lvl++;
                b |= (uint8_t)(lvl << (6 - 2 * k));
            }
            out[o] = b;
        }
        if (o < out_bytes)
            memset(out + o, 0, out_bytes - o);
    }
}

uint32_t bjl_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    static const uint32_t nib[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ nib[crc & 15];
        crc = (crc >> 4) ^ nib[crc & 15];
    }
    return ~crc;
}

int bjl_job_init(bjl_job_t *j, bjl_mode_id_t mode_id, bjl_paper_id_t paper_id, bjl_sink_fn sink, void *user)
{
    const bjl_mode_t *m = bjl_get_mode(mode_id);
    const bjl_paper_t *p = bjl_get_paper(paper_id);
    if (!m || !p || !sink)
        return -1;
    memset(j, 0, sizeof(*j));
    j->sink = sink;
    j->user = user;
    j->mode = m;
    j->paper = p;
    j->width = area_w_pt(p) * m->dpi / 72;
    j->height = area_h_pt(p) * m->dpi / 72;
    j->left = BJL_BORDER_LEFT_PT * m->dpi / 72;
    j->top = BJL_BORDER_TOP_PT * m->dpi / 72;
    int max_bytes = 0;
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        int px_per_byte = 8 / m->bits[c];
        j->line_bytes[c] = (j->width + px_per_byte - 1) / px_per_byte;
        if (j->line_bytes[c] > max_bytes)
            max_bytes = j->line_bytes[c];
    }
    // worst case compressed line plus its 0x80 terminator
    j->block_cap = (size_t)BJL_LINES_PER_BLOCK * (BJL_PACKBITS_MAX(max_bytes) + 1);
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        j->block[c] = BJL_MALLOC_BULK(j->block_cap);
        if (!j->block[c]) {
            bjl_job_free(j);
            return -2;
        }
    }
    return 0;
}

void bjl_job_free(bjl_job_t *j)
{
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        free(j->block[c]);
        j->block[c] = NULL;
    }
}

int bjl_emit_start_job(bjl_job_t *j)
{
    return emit(j, XML_START_JOB, sizeof(XML_START_JOB) - 1);
}

int bjl_emit_mode_shift(bjl_job_t *j)
{
    return emit(j, XML_MODE_SHIFT, sizeof(XML_MODE_SHIFT) - 1);
}

int bjl_page_begin(bjl_job_t *j)
{
    const bjl_mode_t *m = j->mode;
    const bjl_paper_t *pp = j->paper;
    uint8_t b[64];

    // ESC [K 00 0f: reset / initialize, first page only (as Gutenprint)
    static const uint8_t reset[] = {0x1b, '[', 'K', 0x02, 0x00, 0x00, 0x0f};
    if (j->pages++ == 0)
        emit(j, reset, sizeof(reset));

    b[0] = 0x01;                                    // data compression on
    emit_cmd(j, 'b', b, 1);
    b[0] = 0x01;                                    // page id
    emit_cmd(j, 'q', b, 1);

    put_be16(b, m->dpi);                            // y resolution
    put_be16(b + 2, m->dpi);                        // x resolution
    emit_cmd(j, 'd', b, 4);

    // ESC (t: 3 header bytes + 13 ink slots of {bits, 0, levels}
    memset(b, 0, 42);
    memcpy(b, m->t_head, 3);
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        b[3 + c * 3] = m->bits[c];
        b[3 + c * 3 + 2] = m->levels[c];
    }
    emit_cmd(j, 't', b, 42);

    b[0] = 0x30;                                    // model id 3 << 4, colour
    b[1] = BJL_MEDIA_PLAIN;
    b[2] = m->quality;
    emit_cmd(j, 'c', b, 3);

    // ESC (p: page geometry, 46 bytes, big endian
    memset(b, 0, 46);
    put_be16(b + 0, (area_h_pt(pp) + 1) * 5 / 6);  // printable length
    put_be16(b + 4, (area_w_pt(pp) + 1) * 5 / 6);  // printable width
    put_be16(b + 12, 600);                          // unit
    put_be32(b + 14, BJL_BORDER_LEFT_PT * 600 / 72);   // area left
    put_be32(b + 18, BJL_BORDER_TOP_PT * 600 / 72);    // area top
    put_be32(b + 22, area_w_pt(pp) * 600 / 72);    // area width
    put_be32(b + 26, area_h_pt(pp) * 600 / 72);    // area length
    put_be32(b + 38, pp->w_600);                    // paper width
    put_be32(b + 42, pp->h_600);                    // paper length
    emit_cmd(j, 'p', b, 46);

    // ESC (P: 00 size 00 media orientation(1,0) 01 00 00
    const uint8_t escP[] = {0x00, pp->size_code, 0x00, BJL_MEDIA_PLAIN, 0x01, 0x00, 0x01, 0x00, 0x00};
    emit_cmd(j, 'P', escP, sizeof(escP));

    static const uint8_t escT[] = {0x03, 0x04, 0x04};   // cartridge: both (black + colour)
    emit_cmd(j, 'T', escT, sizeof(escT));
    static const uint8_t escl[] = {0x34, 0x00, 0x00};   // rear tray, plain media
    emit_cmd(j, 'l', escl, sizeof(escl));
    b[0] = 0x00;                                    // no page rotation
    emit_cmd(j, 'v', b, 1);

    b[0] = 0x01;                                    // multiraster mode
    emit_cmd(j, 'I', b, 1);
    b[0] = BJL_LINES_PER_BLOCK;
    emit_cmd(j, 'J', b, 1);
    emit_cmd(j, 'L', (const uint8_t *)"CMYK", 4);   // channel order of ESC (F blocks

    j->lines_in_block = 0;
    j->lines_done = 0;
    j->in_page = 1;
    for (int c = 0; c < BJL_NUM_CHANNELS; c++)
        j->block_len[c] = 0;
    return j->err;
}

BJL_HOT static int flush_block(bjl_job_t *j)
{
    // pad a partial block with empty lines
    for (; j->lines_in_block < BJL_LINES_PER_BLOCK; j->lines_in_block++)
        for (int c = 0; c < BJL_NUM_CHANNELS; c++)
            j->block[c][j->block_len[c]++] = 0x80;
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        emit_cmd(j, 'F', j->block[c], (uint16_t)j->block_len[c]);
        j->block_len[c] = 0;
    }
    j->lines_in_block = 0;
    return j->err;
}

BJL_HOT static int is_blank(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

BJL_HOT int bjl_page_add_line(bjl_job_t *j, const uint8_t *const planes[BJL_NUM_CHANNELS])
{
    if (!j->in_page || j->lines_done >= j->height)
        return j->err ? j->err : -3;
    for (int c = 0; c < BJL_NUM_CHANNELS; c++) {
        const uint8_t *p = planes ? planes[c] : NULL;
        uint8_t *dst = j->block[c] + j->block_len[c];
        size_t n = 0;
        if (p && !is_blank(p, j->line_bytes[c]))
            n = bjl_packbits(p, j->line_bytes[c], dst);
        dst[n] = 0x80;                              // line terminator
        j->block_len[c] += n + 1;
    }
    j->lines_done++;
    if (++j->lines_in_block == BJL_LINES_PER_BLOCK)
        flush_block(j);
    return j->err;
}

int bjl_page_end(bjl_job_t *j)
{
    if (!j->in_page)
        return -3;
    // send blank lines up to the full page height so the printer always gets
    // a complete area (cheap: 1 byte per line per channel)
    while (j->lines_done < j->height && !j->err)
        bjl_page_add_line(j, NULL);
    if (j->lines_in_block)
        flush_block(j);
    static const uint8_t ff = 0x0c;                 // eject page
    emit(j, &ff, 1);
    uint8_t off = 0x00;                             // data compression off
    emit_cmd(j, 'b', &off, 1);
    j->in_page = 0;
    return j->err;
}

int bjl_emit_end_job(bjl_job_t *j)
{
    static const uint8_t esc_at[] = {0x1b, '@'};
    emit(j, esc_at, sizeof(esc_at));
    return emit(j, XML_END_JOB, sizeof(XML_END_JOB) - 1);
}
