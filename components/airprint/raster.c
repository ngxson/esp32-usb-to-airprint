// URF and PWG raster share the same per-line compression:
//   line repeat byte r: the decoded line is used r + 1 times
//   then packets until the line is full:
//     n < 128 : the next pixel is repeated n + 1 times
//     n = 128 : the rest of the line is white
//     n > 128 : 257 - n literal pixels follow
#include "raster.h"

#include "canon_bjl.h"    // BJL_HOT

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PWG_HEADER_SIZE 1796

typedef struct {
    raster_read_fn read;
    void *ctx;
    uint8_t buf[4096];
    size_t pos, len;
    int eof;
} reader_t;

// Buffered exact read; returns 0 ok, 1 clean EOF before any byte, -1 error/truncated.
BJL_HOT static int rd(reader_t *r, uint8_t *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (r->pos == r->len) {
            if (r->eof)
                return got == 0 ? 1 : -1;
            int k = r->read(r->ctx, r->buf, sizeof(r->buf));
            if (k < 0)
                return -1;
            if (k == 0) {
                r->eof = 1;
                continue;
            }
            r->pos = 0;
            r->len = (size_t)k;
        }
        size_t take = r->len - r->pos;
        if (take > n - got)
            take = n - got;
        memcpy(out + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

BJL_HOT static int decode_page(reader_t *r, const raster_page_t *pg, const raster_cb_t *cb, void *user)
{
    const int bpp = pg->bytes_pp;
    const size_t line_bytes = (size_t)pg->width * bpp;
    uint8_t *line = BJL_MALLOC_FAST(line_bytes);
    if (!line)
        return RASTER_ERR_NOMEM;
    int ret = cb->page_begin ? cb->page_begin(user, pg) : 0;
    int y = 0;
    while (ret == 0 && y < pg->height) {
        uint8_t rep;
        if (rd(r, &rep, 1)) {
            ret = RASTER_ERR_IO;
            break;
        }
        size_t x = 0;   // in bytes
        bool white = true;      // only white runs / fills so far
        while (x < line_bytes) {
            uint8_t n;
            if (rd(r, &n, 1)) {
                ret = RASTER_ERR_IO;
                break;
            }
            if (n == 128) {
                memset(line + x, 0xff, line_bytes - x);
                x = line_bytes;
            } else if (n < 128) {
                size_t count = (size_t)n + 1;
                uint8_t px[4];
                if (rd(r, px, bpp)) {
                    ret = RASTER_ERR_IO;
                    break;
                }
                if (x + count * bpp > line_bytes) {
                    ret = RASTER_ERR_FORMAT;
                    break;
                }
                if (bpp == 1 || (px[0] == px[1] && px[1] == px[2])) {
                    memset(line + x, px[0], count * bpp);     // gray / white runs
                } else {
                    // copy the pixel once, then double the filled span
                    memcpy(line + x, px, bpp);
                    size_t done = bpp, total = count * bpp;
                    while (done < total) {
                        size_t n = done < total - done ? done : total - done;
                        memcpy(line + x + done, line + x, n);
                        done += n;
                    }
                }
                if (px[0] != 0xff || (bpp == 3 && (px[1] != 0xff || px[2] != 0xff)))
                    white = false;
                x += count * bpp;
            } else {
                size_t count = (size_t)(257 - n);
                if (x + count * bpp > line_bytes) {
                    ret = RASTER_ERR_FORMAT;
                    break;
                }
                if (rd(r, line + x, count * bpp)) {
                    ret = RASTER_ERR_IO;
                    break;
                }
                white = false;
                x += count * bpp;
            }
        }
        if (ret)
            break;
        // an all-white line is passed as NULL so consumers can skip it
        for (int k = 0; k <= rep && y < pg->height && ret == 0; k++, y++)
            ret = cb->line ? cb->line(user, y, white ? NULL : line) : 0;
    }
    if (ret == 0 && cb->page_end)
        ret = cb->page_end(user, pg);
    free(line);
    return ret;
}

int raster_decode(raster_read_fn read, void *ctx, const raster_cb_t *cb, void *user)
{
    reader_t *r = calloc(1, sizeof(*r));
    if (!r)
        return RASTER_ERR_NOMEM;
    r->read = read;
    r->ctx = ctx;

    int ret = 0, pages = 0;
    uint8_t magic[4];
    if (rd(r, magic, 4)) {
        ret = RASTER_ERR_FORMAT;
    } else if (memcmp(magic, "UNIR", 4) == 0) {
        // "UNIRAST\0" + page count, then per page a 32-byte header
        uint8_t h[8];
        if (rd(r, h, 8) || memcmp(h, "AST\0", 4) != 0) {
            ret = RASTER_ERR_FORMAT;
        } else {
            uint32_t npages = be32(h + 4);
            uint8_t ph[32];
            for (;;) {
                int e = rd(r, ph, sizeof(ph));
                if (e == 1)
                    break;      // end of data
                if (e) {
                    ret = RASTER_ERR_IO;
                    break;
                }
                // ph[0] bits per pixel, ph[1] colour space, ph[2] duplex,
                // ph[3] quality, ph[12..23] width, height, dpi
                raster_page_t pg = {
                    .page = pages + 1,
                    .width = (int)be32(ph + 12),
                    .height = (int)be32(ph + 16),
                    .dpi = (int)be32(ph + 20),
                    .bytes_pp = ph[0] / 8,
                };
                if ((ph[0] != 8 && ph[0] != 24) || pg.width <= 0 || pg.height <= 0 || pg.width > 20000 ||
                    pg.height > 30000) {
                    ret = ph[0] != 8 && ph[0] != 24 ? RASTER_ERR_UNSUPPORTED : RASTER_ERR_FORMAT;
                    break;
                }
                if ((ret = decode_page(r, &pg, cb, user)) != 0)
                    break;
                pages++;
                if (npages && (uint32_t)pages == npages)
                    break;
            }
        }
    } else if (memcmp(magic, "RaS2", 4) == 0) {
        uint8_t *ph = malloc(PWG_HEADER_SIZE);
        if (!ph)
            ret = RASTER_ERR_NOMEM;
        while (ph) {
            int e = rd(r, ph, PWG_HEADER_SIZE);
            if (e == 1)
                break;
            if (e) {
                ret = RASTER_ERR_IO;
                break;
            }
            uint32_t xres = be32(ph + 276), yres = be32(ph + 280);
            uint32_t bits_pp = be32(ph + 388), cspace = be32(ph + 400);
            raster_page_t pg = {
                .page = pages + 1,
                .width = (int)be32(ph + 372),
                .height = (int)be32(ph + 376),
                .dpi = (int)xres,
                .bytes_pp = (int)bits_pp / 8,
            };
            // supported: sgray_8 (colour space 18, 8 bpp), srgb_8 (19, 24 bpp)
            if (!((cspace == 18 && bits_pp == 8) || (cspace == 19 && bits_pp == 24)) || xres != yres) {
                ret = RASTER_ERR_UNSUPPORTED;
                break;
            }
            if (pg.width <= 0 || pg.height <= 0 || pg.width > 20000 || pg.height > 30000) {
                ret = RASTER_ERR_FORMAT;
                break;
            }
            if ((ret = decode_page(r, &pg, cb, user)) != 0)
                break;
            pages++;
        }
        free(ph);
    } else {
        ret = RASTER_ERR_FORMAT;
    }
    free(r);
    return ret ? ret : pages;
}

const char *raster_strerror(int err)
{
    switch (err) {
    case RASTER_ERR_IO: return "read error or truncated document";
    case RASTER_ERR_FORMAT: return "not a valid URF / PWG raster document";
    case RASTER_ERR_UNSUPPORTED: return "unsupported raster type";
    case RASTER_ERR_NOMEM: return "out of memory";
    default: return err > 0 ? "ok" : "error";
    }
}
