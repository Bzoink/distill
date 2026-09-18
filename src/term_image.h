#ifndef DISTILL_TERM_IMAGE_H
#define DISTILL_TERM_IMAGE_H

/* Images for a terminal: the two protocols a terminal can draw pixels
   through, encoded here rather than by a library.

   Both are pure functions of a decoded image and a box. The shell decides
   which protocol a terminal speaks and where the box is; this only turns
   pixels into the bytes that draw them there. */

#include "image.h"

#include <stddef.h>
#include <stdint.h>

/* Fit an image inside a box of pixels, keeping its aspect, never enlarging.
   The result is opaque RGB: premultiplied pixels are composited over
   `background` (0xRRGGBB). Caller frees *rgb. */
bool d_term_image_fit(const DImage *image, int box_width, int box_height,
                      uint32_t background, uint8_t **rgb, int *width, int *height);

/* The kitty graphics protocol: transmit and display RGB pixels, sized to
   `columns` x `rows` cells at the cursor. `id` names the image so it can be
   deleted later. NUL-terminated; caller frees. */
char *d_term_image_kitty(const uint8_t *rgb, int width, int height,
                         int columns, int rows, unsigned id);
/* Remove every image this program placed with the kitty protocol. */
const char *d_term_image_kitty_clear(void);

/* Sixel, drawn at the cursor at the image's own pixel size: fit it first.
   Colours are reduced to a fixed 6x6x6 cube plus grays, which is stable
   across calls and needs no second pass over the pixels. NUL-terminated;
   caller frees. */
char *d_term_image_sixel(const uint8_t *rgb, int width, int height);

#endif
