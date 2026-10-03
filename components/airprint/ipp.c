#include "ipp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IPP_MAX_ATTR_SECTION (32 * 1024)

// --- request ----------------------------------------------------------------

static int read_n(ipp_read_fn read, void *ctx, uint8_t *buf, size_t n)
{
    return n == 0 ? 0 : (read(ctx, buf, n) == (int)n ? 0 : -1);
}

int ipp_request_read(ipp_request_t *req, ipp_read_fn read, void *ctx)
{
    memset(req, 0, sizeof(*req));
    uint8_t hdr[8];
    if (read_n(read, ctx, hdr, 8))
        return IPP_BAD_REQUEST;
    req->major = hdr[0];
    req->minor = hdr[1];
    req->op = (uint16_t)(hdr[2] << 8 | hdr[3]);
    req->request_id = (uint32_t)hdr[4] << 24 | hdr[5] << 16 | hdr[6] << 8 | hdr[7];

    // Copy the attribute section into one buffer, then index it. Values are
    // read with their length prefixes so the parse below is bounds-safe.
    size_t cap = 2048;
    req->raw = malloc(cap);
    if (!req->raw)
        return IPP_INTERNAL_ERROR;
    for (;;) {
        uint8_t tag;
        if (read_n(read, ctx, &tag, 1))
            return IPP_BAD_REQUEST;
        size_t need = 1 + (tag > 0x0f ? 4 : 0);
        if (req->raw_len + need + 1 > cap) {
            if (cap >= IPP_MAX_ATTR_SECTION)
                return IPP_BAD_REQUEST;
            cap *= 2;
            uint8_t *nb = realloc(req->raw, cap);
            if (!nb)
                return IPP_INTERNAL_ERROR;
            req->raw = nb;
        }
        req->raw[req->raw_len++] = tag;
        if (tag == IPP_TAG_END)
            break;
        if (tag <= 0x0f)
            continue;
        // name-length, name, value-length, value
        for (int part = 0; part < 2; part++) {
            uint8_t lenb[2];
            if (read_n(read, ctx, lenb, 2))
                return IPP_BAD_REQUEST;
            size_t len = (size_t)lenb[0] << 8 | lenb[1];
            if (req->raw_len + 2 + len + 1 > cap) {
                while (req->raw_len + 2 + len + 1 > cap)
                    cap *= 2;
                if (cap > IPP_MAX_ATTR_SECTION)
                    return IPP_BAD_REQUEST;
                uint8_t *nb = realloc(req->raw, cap);
                if (!nb)
                    return IPP_INTERNAL_ERROR;
                req->raw = nb;
            }
            req->raw[req->raw_len++] = lenb[0];
            req->raw[req->raw_len++] = lenb[1];
            if (read_n(read, ctx, req->raw + req->raw_len, len))
                return IPP_BAD_REQUEST;
            req->raw_len += len;
        }
    }

    // index
    uint8_t group = 0;
    size_t p = 0;
    while (p < req->raw_len) {
        uint8_t tag = req->raw[p++];
        if (tag == IPP_TAG_END)
            break;
        if (tag <= 0x0f) {
            group = tag;
            continue;
        }
        uint16_t nl = (uint16_t)(req->raw[p] << 8 | req->raw[p + 1]);
        const char *name = (const char *)req->raw + p + 2;
        p += 2 + nl;
        uint16_t vl = (uint16_t)(req->raw[p] << 8 | req->raw[p + 1]);
        const uint8_t *val = req->raw + p + 2;
        p += 2 + vl;
        if (req->nattrs < IPP_MAX_ATTRS) {
            ipp_attr_t *a = &req->attrs[req->nattrs++];
            a->group = group;
            a->tag = tag;
            a->name = name;
            a->name_len = nl;
            a->value = val;
            a->value_len = vl;
        }
    }
    if (req->major < 1 || req->major > 2 || (req->major == 1 && req->minor > 1) || (req->major == 2 && req->minor > 2))
        return IPP_VERSION_NOT_SUPPORTED;
    return 0;
}

void ipp_request_free(ipp_request_t *req)
{
    free(req->raw);
    req->raw = NULL;
}

static bool name_eq(const ipp_attr_t *a, const char *name)
{
    size_t n = strlen(name);
    return a->name_len == n && memcmp(a->name, name, n) == 0;
}

int ipp_find_index(const ipp_request_t *req, uint8_t group, const char *name)
{
    for (int i = 0; i < req->nattrs; i++)
        if ((group == 0 || req->attrs[i].group == group) && name_eq(&req->attrs[i], name))
            return i;
    return -1;
}

const ipp_attr_t *ipp_find(const ipp_request_t *req, uint8_t group, const char *name)
{
    int i = ipp_find_index(req, group, name);
    return i < 0 ? NULL : &req->attrs[i];
}

const char *ipp_attr_str(const ipp_attr_t *a, char *out, size_t out_len)
{
    if (!out_len)
        return "";
    out[0] = 0;
    if (!a)
        return out;
    size_t n = a->value_len < out_len - 1 ? a->value_len : out_len - 1;
    memcpy(out, a->value, n);
    out[n] = 0;
    return out;
}

int ipp_attr_int(const ipp_attr_t *a, int def)
{
    if (!a || (a->tag != IPP_TAG_INTEGER && a->tag != IPP_TAG_ENUM) || a->value_len != 4)
        return def;
    return (int32_t)((uint32_t)a->value[0] << 24 | a->value[1] << 16 | a->value[2] << 8 | a->value[3]);
}

bool ipp_attr_is(const ipp_attr_t *a, const char *s)
{
    size_t n = strlen(s);
    return a && a->value_len == n && memcmp(a->value, s, n) == 0;
}

// --- response ---------------------------------------------------------------

void ipp_out_init(ipp_out_t *o)
{
    memset(o, 0, sizeof(*o));
}

void ipp_out_free(ipp_out_t *o)
{
    free(o->buf);
    memset(o, 0, sizeof(*o));
}

static void put(ipp_out_t *o, const void *data, size_t n)
{
    if (o->oom)
        return;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 1024;
        while (cap < o->len + n)
            cap *= 2;
        uint8_t *nb = realloc(o->buf, cap);
        if (!nb) {
            o->oom = true;
            return;
        }
        o->buf = nb;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, data, n);
    o->len += n;
}

static void put8(ipp_out_t *o, uint8_t v)
{
    put(o, &v, 1);
}

static void put16(ipp_out_t *o, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(o, b, 2);
}

static void put32(ipp_out_t *o, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u};
    put(o, b, 4);
}

void ipp_out_header(ipp_out_t *o, uint8_t major, uint8_t minor, uint16_t status, uint32_t request_id)
{
    put8(o, major);
    put8(o, minor);
    put16(o, status);
    put32(o, (int32_t)request_id);
}

void ipp_out_group(ipp_out_t *o, uint8_t group_tag)
{
    put8(o, group_tag);
}

void ipp_out_end(ipp_out_t *o)
{
    put8(o, IPP_TAG_END);
}

void ipp_out_append(ipp_out_t *dst, const ipp_out_t *src)
{
    if (src->oom)
        dst->oom = true;
    else
        put(dst, src->buf, src->len);
}

void ipp_out_raw(ipp_out_t *o, uint8_t tag, const char *name, const void *value, size_t len)
{
    put8(o, tag);
    size_t nl = name ? strlen(name) : 0;
    put16(o, (uint16_t)nl);
    put(o, name, nl);
    put16(o, (uint16_t)len);
    put(o, value, len);
}

void ipp_out_str(ipp_out_t *o, uint8_t tag, const char *name, const char *s)
{
    ipp_out_raw(o, tag, name, s, strlen(s));
}

void ipp_out_int(ipp_out_t *o, uint8_t tag, const char *name, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u};
    ipp_out_raw(o, tag, name, b, 4);
}

void ipp_out_bool(ipp_out_t *o, const char *name, bool v)
{
    uint8_t b = v ? 1 : 0;
    ipp_out_raw(o, IPP_TAG_BOOLEAN, name, &b, 1);
}

void ipp_out_range(ipp_out_t *o, const char *name, int32_t lo, int32_t hi)
{
    uint8_t b[8];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)((uint32_t)lo >> (24 - 8 * i));
        b[4 + i] = (uint8_t)((uint32_t)hi >> (24 - 8 * i));
    }
    ipp_out_raw(o, IPP_TAG_RANGE, name, b, 8);
}

void ipp_out_res(ipp_out_t *o, const char *name, int32_t xres, int32_t yres)
{
    uint8_t b[9];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)((uint32_t)xres >> (24 - 8 * i));
        b[4 + i] = (uint8_t)((uint32_t)yres >> (24 - 8 * i));
    }
    b[8] = 3;   // dots per inch
    ipp_out_raw(o, IPP_TAG_RESOLUTION, name, b, 9);
}

void ipp_out_date(ipp_out_t *o, const char *name, int64_t unix_time)
{
    time_t t = (time_t)unix_time;
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t b[11] = {
        (uint8_t)((tm.tm_year + 1900) >> 8), (uint8_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1),
        (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, 0, '+', 0, 0,
    };
    ipp_out_raw(o, IPP_TAG_DATE, name, b, sizeof(b));
}

void ipp_out_strs(ipp_out_t *o, uint8_t tag, const char *name, const char *const *values)
{
    for (int i = 0; values[i]; i++)
        ipp_out_str(o, tag, i == 0 ? name : NULL, values[i]);
}

void ipp_out_coll_begin(ipp_out_t *o, const char *name)
{
    ipp_out_raw(o, IPP_TAG_BEGIN_COLLECTION, name, NULL, 0);
}

void ipp_out_member(ipp_out_t *o, const char *member)
{
    ipp_out_raw(o, IPP_TAG_MEMBERNAME, NULL, member, strlen(member));
}

void ipp_out_coll_end(ipp_out_t *o)
{
    ipp_out_raw(o, IPP_TAG_END_COLLECTION, NULL, NULL, 0);
}
