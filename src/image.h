#ifndef DISTILL_IMAGE_H
#define DISTILL_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded still image, in the one format the paint layer wants: 32-bit
   pixels as 0xAARRGGBB with the colour channels already multiplied by alpha.
   That is Cairo's ARGB32, and it is also what a framebuffer blit wants, so
   the decoder does the conversion once rather than every target doing it. */
typedef struct {
    int width;
    int height;
    uint32_t *pixels;   /* width * height, row-major, owned by the image */
} DImage;

/* Decoding is bounded rather than trusting the header: a two-byte field can
   claim a gigapixel image, and the allocation happens before any of the file
   has been validated. */
#define DISTILL_IMAGE_MAX_DIMENSION 16384
#define DISTILL_IMAGE_MAX_PIXELS (50 * 1000 * 1000)

/* Decode PNG, JPEG or WebP bytes. The format is taken from the leading bytes,
   never from a file name or a served content type, both of which are page
   controlled. Returns false and leaves *out zeroed on any failure. */
bool d_image_decode(const unsigned char *bytes, size_t length, DImage *out);

/* Decode a local file. The renderer never fetches: whatever is drawn was put
   on disk by the engine, which is the only part of the program that talks to
   the network. */
bool d_image_load_file(const char *path, DImage *out);

void d_image_free(DImage *image);

/* Why the last decode in this thread failed, for a caller that wants to say
   so rather than showing an empty box. Never NULL. */
const char *d_image_last_error(void);

/* The name of the format these bytes are in ("png", "jpeg", "webp", "gif",
   "svg"), or "" when the leading bytes match nothing known. Reported so an
   unsupported image can say which format it is instead of failing blankly. */
const char *d_image_format_name(const unsigned char *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
