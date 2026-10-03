// Strip-wise baseline JPEG encoder (ITU T.81), YCbCr 4:2:0.
//
// Lines are converted to YCbCr as they arrive: Y into a 16-row strip, Cb/Cr
// averaged over 2x2 pixels into 8-row strips. Each full strip is one row of
// 16x16 MCUs (4 Y blocks + Cb + Cr) and is encoded right away. The DCT is
// libjpeg's accurate integer one (jfdctint.c), quantisation uses fixed-point
// reciprocals, and the Huffman tables are the standard ones of Annex K.
#include "jpeg_enc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OUT_BUF 1024

struct jpeg_enc {
    int w, h, pw;               // pw: width padded to a multiple of 16
    int line;                   // lines added
    int row;                    // row within the current strip (0..15)
    uint8_t *y;                 // 16 x pw
    uint8_t *cb, *cr;           // 8 x pw/2
    int32_t *acc;               // pw/2 x (Cb, Cr): chroma sums of the even line, 16.16 fixed point
    uint32_t recip[2][64];      // quantisation reciprocals (natural order), 2^18 / (8 q)
    uint16_t dc_code[2][12], ac_code[2][256];
    uint8_t dc_size[2][12], ac_size[2][256];
    int dc_pred[3];
    uint32_t bits;
    int nbits;
    uint8_t out[OUT_BUF];
    size_t out_len;
    uint64_t bytes;
    int err;
    jpeg_write_fn write;
    void *user;
};

// natural index of each zigzag position
static const uint8_t ZZ[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Annex K.1 quantisation tables, natural order
static const uint8_t Q_LUMA[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t Q_CHROMA[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

// Annex K.3 Huffman tables: code counts per length 1..16, then the values
static const uint8_t DC_LUMA_BITS[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t DC_CHROMA_BITS[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t DC_VALS[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t AC_LUMA_BITS[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t AC_LUMA_VALS[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t AC_CHROMA_BITS[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t AC_CHROMA_VALS[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

// ---------------------------------------------------------------------------
// output

static void flush_out(jpeg_enc_t *e)
{
    if (e->out_len && !e->err && e->write(e->user, e->out, e->out_len) < 0)
        e->err = -1;
    e->bytes += e->out_len;
    e->out_len = 0;
}

static inline void put_byte(jpeg_enc_t *e, uint8_t b)
{
    e->out[e->out_len++] = b;
    if (e->out_len == OUT_BUF)
        flush_out(e);
}

static void put_bytes(jpeg_enc_t *e, const uint8_t *p, size_t n)
{
    while (n--)
        put_byte(e, *p++);
}

static void put_u16(jpeg_enc_t *e, unsigned v)
{
    put_byte(e, (uint8_t)(v >> 8));
    put_byte(e, (uint8_t)v);
}

// entropy-coded data: MSB first, 0xFF followed by a stuffed 0x00
static inline void put_bits(jpeg_enc_t *e, uint32_t code, int size)
{
    e->bits = (e->bits << size) | (code & ((1u << size) - 1));
    e->nbits += size;
    while (e->nbits >= 8) {
        uint8_t b = (uint8_t)(e->bits >> (e->nbits - 8));
        put_byte(e, b);
        if (b == 0xff)
            put_byte(e, 0);
        e->nbits -= 8;
    }
}

// ---------------------------------------------------------------------------
// tables and headers

static void build_huffman(const uint8_t bits[16], const uint8_t *vals, uint16_t *code, uint8_t *size)
{
    unsigned c = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++, k++) {
            code[vals[k]] = (uint16_t)c++;
            size[vals[k]] = (uint8_t)len;
        }
        c <<= 1;
    }
}

static void write_dht(jpeg_enc_t *e, uint8_t id, const uint8_t bits[16], const uint8_t *vals)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += bits[i];
    put_byte(e, id);
    put_bytes(e, bits, 16);
    put_bytes(e, vals, n);
}

static void write_headers(jpeg_enc_t *e, const uint8_t q[2][64], int dpi)
{
    static const uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1};
    put_u16(e, 0xffd8);                 // SOI
    put_u16(e, 0xffe0);                 // APP0 JFIF
    put_u16(e, 16);
    put_bytes(e, jfif, sizeof(jfif));
    put_byte(e, dpi > 0 ? 1 : 0);       // density unit: dots per inch
    put_u16(e, dpi > 0 ? dpi : 1);
    put_u16(e, dpi > 0 ? dpi : 1);
    put_byte(e, 0);                     // no thumbnail
    put_byte(e, 0);

    put_u16(e, 0xffdb);                 // DQT, both tables, zigzag order
    put_u16(e, 2 + 2 * 65);
    for (int t = 0; t < 2; t++) {
        put_byte(e, (uint8_t)t);
        for (int k = 0; k < 64; k++)
            put_byte(e, q[t][ZZ[k]]);
    }

    put_u16(e, 0xffc0);                 // SOF0: baseline
    put_u16(e, 17);
    put_byte(e, 8);
    put_u16(e, (unsigned)e->h);
    put_u16(e, (unsigned)e->w);
    put_byte(e, 3);
    static const uint8_t comps[9] = {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
    put_bytes(e, comps, sizeof(comps));

    put_u16(e, 0xffc4);                 // DHT, all four tables
    put_u16(e, 2 + 4 * 17 + 12 + 12 + 162 + 162);
    write_dht(e, 0x00, DC_LUMA_BITS, DC_VALS);
    write_dht(e, 0x10, AC_LUMA_BITS, AC_LUMA_VALS);
    write_dht(e, 0x01, DC_CHROMA_BITS, DC_VALS);
    write_dht(e, 0x11, AC_CHROMA_BITS, AC_CHROMA_VALS);

    put_u16(e, 0xffda);                 // SOS
    put_u16(e, 12);
    static const uint8_t sos[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    put_bytes(e, sos, sizeof(sos));
}

// ---------------------------------------------------------------------------
// block coding

#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

// libjpeg's jpeg_fdct_islow: output scaled up by 8
static void fdct(int32_t *d)
{
    for (int pass = 0; pass < 2; pass++) {
        int step = pass ? 8 : 1;        // pass 0: rows, pass 1: columns
        int next = pass ? 1 : 8;
        for (int i = 0; i < 8; i++) {
            int32_t *p = d + i * next;
            int32_t tmp0 = p[0] + p[7 * step], tmp7 = p[0] - p[7 * step];
            int32_t tmp1 = p[step] + p[6 * step], tmp6 = p[step] - p[6 * step];
            int32_t tmp2 = p[2 * step] + p[5 * step], tmp5 = p[2 * step] - p[5 * step];
            int32_t tmp3 = p[3 * step] + p[4 * step], tmp4 = p[3 * step] - p[4 * step];
            int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
            int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
            int sh = pass ? CONST_BITS + PASS1_BITS : CONST_BITS - PASS1_BITS;
            if (pass) {
                p[0] = DESCALE(tmp10 + tmp11, PASS1_BITS);
                p[4 * step] = DESCALE(tmp10 - tmp11, PASS1_BITS);
            } else {
                p[0] = (tmp10 + tmp11) * (1 << PASS1_BITS);
                p[4 * step] = (tmp10 - tmp11) * (1 << PASS1_BITS);
            }
            int32_t z1 = (tmp12 + tmp13) * FIX_0_541196100;
            p[2 * step] = DESCALE(z1 + tmp13 * FIX_0_765366865, sh);
            p[6 * step] = DESCALE(z1 - tmp12 * FIX_1_847759065, sh);

            z1 = tmp4 + tmp7;
            int32_t z2 = tmp5 + tmp6, z3 = tmp4 + tmp6, z4 = tmp5 + tmp7;
            int32_t z5 = (z3 + z4) * FIX_1_175875602;
            tmp4 *= FIX_0_298631336;
            tmp5 *= FIX_2_053119869;
            tmp6 *= FIX_3_072711026;
            tmp7 *= FIX_1_501321110;
            z1 *= -FIX_0_899976223;
            z2 *= -FIX_2_562915447;
            z3 = z3 * -FIX_1_961570560 + z5;
            z4 = z4 * -FIX_0_390180644 + z5;
            p[7 * step] = DESCALE(tmp4 + z1 + z3, sh);
            p[5 * step] = DESCALE(tmp5 + z2 + z4, sh);
            p[3 * step] = DESCALE(tmp6 + z2 + z3, sh);
            p[step] = DESCALE(tmp7 + z1 + z4, sh);
        }
    }
}

static inline int bit_length(unsigned v)
{
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

// Encodes one 8x8 block of samples (stride bytes apart) of component c.
static void encode_block(jpeg_enc_t *e, const uint8_t *src, int stride, int c)
{
    int32_t d[64];
    for (int r = 0; r < 8; r++, src += stride)
        for (int k = 0; k < 8; k++)
            d[r * 8 + k] = (int32_t)src[k] - 128;
    fdct(d);

    int t = c ? 1 : 0;
    const uint32_t *recip = e->recip[t];
    for (int i = 0; i < 64; i++) {
        int32_t v = d[i];
        int32_t a = v < 0 ? -v : v;
        a = (int32_t)(((uint32_t)a * recip[i] + (1u << 17)) >> 18);
        if (a > 1023)
            a = 1023;           // baseline range (only reachable at quality ~100)
        d[i] = v < 0 ? -a : a;
    }

    int diff = d[0] - e->dc_pred[c];
    e->dc_pred[c] = d[0];
    int n = bit_length((unsigned)(diff < 0 ? -diff : diff));
    put_bits(e, e->dc_code[t][n], e->dc_size[t][n]);
    if (n)
        put_bits(e, (uint32_t)(diff < 0 ? diff - 1 : diff), n);

    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = d[ZZ[k]];
        if (!v) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(e, e->ac_code[t][0xf0], e->ac_size[t][0xf0]);     // ZRL
            run -= 16;
        }
        n = bit_length((unsigned)(v < 0 ? -v : v));
        int sym = (run << 4) | n;
        put_bits(e, e->ac_code[t][sym], e->ac_size[t][sym]);
        put_bits(e, (uint32_t)(v < 0 ? v - 1 : v), n);
        run = 0;
    }
    if (run)
        put_bits(e, e->ac_code[t][0], e->ac_size[t][0]);               // EOB
}

// Encodes the strip; rows beyond `rows` repeat the last one.
static void encode_strip(jpeg_enc_t *e, int rows)
{
    int cw = e->pw / 2;
    for (int r = rows; r < 16; r++)
        memcpy(e->y + r * e->pw, e->y + (rows - 1) * e->pw, e->pw);
    int crows = (rows + 1) / 2;
    for (int r = crows; r < 8; r++) {
        memcpy(e->cb + r * cw, e->cb + (crows - 1) * cw, cw);
        memcpy(e->cr + r * cw, e->cr + (crows - 1) * cw, cw);
    }
    for (int mx = 0; mx < e->pw; mx += 16) {
        const uint8_t *y = e->y + mx;
        encode_block(e, y, e->pw, 0);
        encode_block(e, y + 8, e->pw, 0);
        encode_block(e, y + 8 * e->pw, e->pw, 0);
        encode_block(e, y + 8 * e->pw + 8, e->pw, 0);
        encode_block(e, e->cb + mx / 2, cw, 1);
        encode_block(e, e->cr + mx / 2, cw, 2);
    }
}

static inline uint8_t clamp8(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

// Writes the averaged chroma of the current line pair (scale: number of
// lines summed, 1 or 2).
static void store_chroma(jpeg_enc_t *e, int lines)
{
    int cw = e->pw / 2;
    uint8_t *cb = e->cb + (e->row / 2) * cw, *cr = e->cr + (e->row / 2) * cw;
    int shift = lines == 2 ? 18 : 17;  // 2 or 4 pixels summed, 16.16 fixed point
    int32_t bias = (128 << shift) + (1 << (shift - 1));
    for (int i = 0; i < cw; i++) {
        cb[i] = clamp8((e->acc[2 * i] + bias) >> shift);
        cr[i] = clamp8((e->acc[2 * i + 1] + bias) >> shift);
    }
}

// ---------------------------------------------------------------------------
// API

jpeg_enc_t *jpeg_enc_create(int width, int height, int quality, int dpi, jpeg_write_fn write, void *user)
{
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 || !write)
        return NULL;
    jpeg_enc_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->w = width;
    e->h = height;
    e->pw = (width + 15) & ~15;
    e->write = write;
    e->user = user;
    e->y = malloc((size_t)16 * e->pw);
    e->cb = malloc((size_t)8 * e->pw / 2);
    e->cr = malloc((size_t)8 * e->pw / 2);
    e->acc = malloc(sizeof(int32_t) * e->pw);
    if (!e->y || !e->cb || !e->cr || !e->acc) {
        jpeg_enc_free(e);
        return NULL;
    }

    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;    // as libjpeg
    uint8_t q[2][64];
    for (int i = 0; i < 64; i++) {
        for (int t = 0; t < 2; t++) {
            int v = ((t ? Q_CHROMA[i] : Q_LUMA[i]) * scale + 50) / 100;
            q[t][i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
            uint32_t div = 8u * q[t][i];            // the DCT output is scaled by 8
            e->recip[t][i] = ((1u << 18) + div / 2) / div;
        }
    }
    build_huffman(DC_LUMA_BITS, DC_VALS, e->dc_code[0], e->dc_size[0]);
    build_huffman(DC_CHROMA_BITS, DC_VALS, e->dc_code[1], e->dc_size[1]);
    build_huffman(AC_LUMA_BITS, AC_LUMA_VALS, e->ac_code[0], e->ac_size[0]);
    build_huffman(AC_CHROMA_BITS, AC_CHROMA_VALS, e->ac_code[1], e->ac_size[1]);

    write_headers(e, q, dpi);
    flush_out(e);
    if (e->err) {
        jpeg_enc_free(e);
        return NULL;
    }
    return e;
}

int jpeg_enc_add_line(jpeg_enc_t *e, const uint8_t *rgb)
{
    if (e->err)
        return e->err;
    if (e->line >= e->h)
        return 0;
    uint8_t *y = e->y + e->row * e->pw;
    bool odd = e->row & 1;
    int cw = e->pw / 2;
    int last = e->w - 1;
    for (int i = 0; i < cw; i++) {
        // pixel pair; columns past the edge repeat the last pixel
        int x0 = 2 * i < last ? 2 * i : last, x1 = 2 * i + 1 < last ? 2 * i + 1 : last;
        const uint8_t *p0 = rgb + 3 * x0, *p1 = rgb + 3 * x1;
        int32_t r = p0[0] + p1[0], g = p0[1] + p1[1], b = p0[2] + p1[2];
        y[2 * i] = (uint8_t)((19595 * p0[0] + 38470 * p0[1] + 7471 * p0[2] + 32768) >> 16);
        y[2 * i + 1] = (uint8_t)((19595 * p1[0] + 38470 * p1[1] + 7471 * p1[2] + 32768) >> 16);
        int32_t cb = -11059 * r - 21709 * g + 32768 * b;
        int32_t cr = 32768 * r - 27439 * g - 5329 * b;
        if (odd) {
            e->acc[2 * i] += cb;
            e->acc[2 * i + 1] += cr;
        } else {
            e->acc[2 * i] = cb;
            e->acc[2 * i + 1] = cr;
        }
    }
    e->line++;
    bool strip_end = e->row == 15 || e->line == e->h;
    if (odd)
        store_chroma(e, 2);
    else if (strip_end)
        store_chroma(e, 1);     // odd number of lines: the last one alone
    if (strip_end) {
        encode_strip(e, e->row + 1);
        e->row = 0;
    } else {
        e->row++;
    }
    return e->err;
}

int jpeg_enc_finish(jpeg_enc_t *e)
{
    if (e->line < e->h) {
        uint8_t *white = malloc((size_t)e->w * 3);
        if (!white)
            return -1;
        memset(white, 255, (size_t)e->w * 3);
        while (e->line < e->h && !e->err)
            jpeg_enc_add_line(e, white);
        free(white);
    }
    if (e->nbits)
        put_bits(e, 0x7f, 8 - e->nbits);    // pad with 1 bits
    put_u16(e, 0xffd9);                     // EOI
    flush_out(e);
    return e->err;
}

void jpeg_enc_free(jpeg_enc_t *e)
{
    if (!e)
        return;
    free(e->y);
    free(e->cb);
    free(e->cr);
    free(e->acc);
    free(e);
}

int jpeg_enc_lines(const jpeg_enc_t *e)
{
    return e->line;
}

uint64_t jpeg_enc_bytes(const jpeg_enc_t *e)
{
    return e->bytes + e->out_len;
}
