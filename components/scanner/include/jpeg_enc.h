// Baseline JPEG encoder fed one RGB line at a time: only one 16-line strip
// is kept in memory, so a page of any size streams out as it is scanned.
// YCbCr 4:2:0, standard Huffman tables. Integer arithmetic only, so the
// output is byte-identical on the ESP32 and in the host tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Receives the encoded bytes. Return 0 on success, < 0 to abort.
typedef int (*jpeg_write_fn)(void *user, const uint8_t *data, size_t len);

typedef struct jpeg_enc jpeg_enc_t;

// Writes the headers. quality 1..100; dpi goes into the JFIF header (0 = none).
// NULL on bad arguments, out of memory or a failed write.
jpeg_enc_t *jpeg_enc_create(int width, int height, int quality, int dpi, jpeg_write_fn write, void *user);
// The next line: width * 3 bytes of RGB. Lines beyond the height are ignored.
// Returns 0, or < 0 once a write has failed.
int jpeg_enc_add_line(jpeg_enc_t *e, const uint8_t *rgb);
// Fills the missing lines with white and writes the end of the image.
int jpeg_enc_finish(jpeg_enc_t *e);
void jpeg_enc_free(jpeg_enc_t *e);

int jpeg_enc_lines(const jpeg_enc_t *e);           // lines added so far
uint64_t jpeg_enc_bytes(const jpeg_enc_t *e);      // bytes written so far

#ifdef __cplusplus
}
#endif
