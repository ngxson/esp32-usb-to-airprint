// Minimal IPP/1.1-2.0 message codec (RFC 8010): request parsing and
// response building. Portable C, used on the ESP32 and in the host harness.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// delimiter tags
#define IPP_TAG_OPERATION 0x01
#define IPP_TAG_JOB 0x02
#define IPP_TAG_END 0x03
#define IPP_TAG_PRINTER 0x04
#define IPP_TAG_UNSUPPORTED_GROUP 0x05

// value tags
#define IPP_TAG_UNSUPPORTED_VALUE 0x10
#define IPP_TAG_UNKNOWN 0x12
#define IPP_TAG_NOVALUE 0x13
#define IPP_TAG_INTEGER 0x21
#define IPP_TAG_BOOLEAN 0x22
#define IPP_TAG_ENUM 0x23
#define IPP_TAG_STRING 0x30
#define IPP_TAG_DATE 0x31
#define IPP_TAG_RESOLUTION 0x32
#define IPP_TAG_RANGE 0x33
#define IPP_TAG_BEGIN_COLLECTION 0x34
#define IPP_TAG_END_COLLECTION 0x37
#define IPP_TAG_TEXT 0x41
#define IPP_TAG_NAME 0x42
#define IPP_TAG_KEYWORD 0x44
#define IPP_TAG_URI 0x45
#define IPP_TAG_URISCHEME 0x46
#define IPP_TAG_CHARSET 0x47
#define IPP_TAG_LANGUAGE 0x48
#define IPP_TAG_MIMETYPE 0x49
#define IPP_TAG_MEMBERNAME 0x4a

// operations
#define IPP_OP_PRINT_JOB 0x0002
#define IPP_OP_VALIDATE_JOB 0x0004
#define IPP_OP_CREATE_JOB 0x0005
#define IPP_OP_SEND_DOCUMENT 0x0006
#define IPP_OP_CANCEL_JOB 0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES 0x0009
#define IPP_OP_GET_JOBS 0x000a
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000b
#define IPP_OP_CANCEL_MY_JOBS 0x0039
#define IPP_OP_CLOSE_JOB 0x003b
#define IPP_OP_IDENTIFY_PRINTER 0x003c

// status codes
#define IPP_OK 0x0000
#define IPP_OK_IGNORED 0x0001
#define IPP_BAD_REQUEST 0x0400
#define IPP_NOT_POSSIBLE 0x0404
#define IPP_NOT_FOUND 0x0406
#define IPP_DOCUMENT_FORMAT_NOT_SUPPORTED 0x040a
#define IPP_ATTRIBUTES_NOT_SUPPORTED 0x040b
#define IPP_COMPRESSION_NOT_SUPPORTED 0x040f
#define IPP_DOCUMENT_FORMAT_ERROR 0x0411
#define IPP_INTERNAL_ERROR 0x0500
#define IPP_OPERATION_NOT_SUPPORTED 0x0501
#define IPP_VERSION_NOT_SUPPORTED 0x0503
#define IPP_DEVICE_ERROR 0x0504
#define IPP_NOT_ACCEPTING_JOBS 0x0506
#define IPP_BUSY 0x0507
#define IPP_JOB_CANCELED 0x0508

// ---------------------------------------------------------------------------
// request

typedef struct {
    uint8_t group;              // delimiter tag the attribute belongs to
    uint8_t tag;                // value tag
    const char *name;           // not NUL terminated, see name_len
    uint16_t name_len;          // 0 for additional values of the previous attribute
    const uint8_t *value;
    uint16_t value_len;
} ipp_attr_t;

#define IPP_MAX_ATTRS 160

typedef struct {
    uint8_t major, minor;
    uint16_t op;
    uint32_t request_id;
    int nattrs;
    ipp_attr_t attrs[IPP_MAX_ATTRS];
    uint8_t *raw;               // attribute section storage (owned)
    size_t raw_len;
} ipp_request_t;

// Reads exactly n bytes; returns n, or <= 0 on EOF/error.
typedef int (*ipp_read_fn)(void *ctx, uint8_t *buf, size_t n);

// Reads the request header and attribute section (up to the end-of-attributes
// tag) from the stream; document data, if any, follows in the stream.
// Returns 0 or an IPP status code (IPP_BAD_REQUEST, ...).
int ipp_request_read(ipp_request_t *req, ipp_read_fn read, void *ctx);
void ipp_request_free(ipp_request_t *req);

// First attribute with this name in the given group (0 = any group).
const ipp_attr_t *ipp_find(const ipp_request_t *req, uint8_t group, const char *name);
// Index of the attribute (for iterating its additional values), -1 if absent.
int ipp_find_index(const ipp_request_t *req, uint8_t group, const char *name);
// Copies a string value into out (NUL terminated), returns out or "" .
const char *ipp_attr_str(const ipp_attr_t *a, char *out, size_t out_len);
int ipp_attr_int(const ipp_attr_t *a, int def);
bool ipp_attr_is(const ipp_attr_t *a, const char *s);

// ---------------------------------------------------------------------------
// response

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool oom;
} ipp_out_t;

void ipp_out_init(ipp_out_t *o);
void ipp_out_free(ipp_out_t *o);
void ipp_out_header(ipp_out_t *o, uint8_t major, uint8_t minor, uint16_t status, uint32_t request_id);
void ipp_out_group(ipp_out_t *o, uint8_t group_tag);
void ipp_out_end(ipp_out_t *o);
// Appends the encoded attributes of src to dst.
void ipp_out_append(ipp_out_t *dst, const ipp_out_t *src);

// One value. name == NULL adds another value to the previous attribute.
void ipp_out_raw(ipp_out_t *o, uint8_t tag, const char *name, const void *value, size_t len);
void ipp_out_str(ipp_out_t *o, uint8_t tag, const char *name, const char *s);
void ipp_out_int(ipp_out_t *o, uint8_t tag, const char *name, int32_t v);    // integer / enum
void ipp_out_bool(ipp_out_t *o, const char *name, bool v);
void ipp_out_range(ipp_out_t *o, const char *name, int32_t lo, int32_t hi);
void ipp_out_res(ipp_out_t *o, const char *name, int32_t xres, int32_t yres);  // dots per inch
void ipp_out_date(ipp_out_t *o, const char *name, int64_t unix_time);
// Several string values for one attribute (NULL terminated list).
void ipp_out_strs(ipp_out_t *o, uint8_t tag, const char *name, const char *const *values);

// Collections: begin (name NULL = additional value), members, end.
void ipp_out_coll_begin(ipp_out_t *o, const char *name);
void ipp_out_member(ipp_out_t *o, const char *member);    // memberAttrName; next value is its value
void ipp_out_coll_end(ipp_out_t *o);

#ifdef __cplusplus
}
#endif
