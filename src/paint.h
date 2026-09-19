#ifndef DISTILL_PAINT_H
#define DISTILL_PAINT_H

/* The painter: what render.c draws with, into a buffer of pixels.

   render.c owns fonts, shaping and measurement; this owns pixels. One
   implementation is linked, chosen at link time: paint_soft.c, portable C
   over a plain XRGB8888 buffer, in every shipped binary. The Cairo painter it
   replaced survives only as tests/paint_cairo.c, the reference that
   `check.py --paint` compares it against.

   The buffer is 32-bit words, 0xAARRGGBB with colour multiplied by alpha --
   the layout Cairo calls ARGB32 and a framebuffer calls XRGB8888, since the
   surface is always opaque. Coordinates that litehtml or the shaper produce
   as fractions are passed as fractions, so the coverage at the edge of a
   glyph, a rule or a circle is the painter's to get right. */

#include "render.h"

typedef struct DPaint DPaint;

/* A surface filled with the page colour. NULL if it cannot be allocated. */
DPaint *d_paint_create(int width, int height);
void d_paint_destroy(DPaint *paint);

/* Fill everything, ignoring the clip, with the page colour. */
void d_paint_clear(DPaint *paint);

bool d_paint_pixels(DPaint *paint, DRenderPixels *pixels);
bool d_paint_write_png(DPaint *paint, const char *path);

/* Rectangles on whole pixels. */
void d_paint_fill(DPaint *paint, DRenderRect rect, DRenderColor color);
/* The inside border of `rect`, `width` pixels deep. */
void d_paint_stroke(DPaint *paint, DRenderRect rect, int width, DRenderColor color);

/* A horizontal rule `thickness` tall centred on `y`, from x0 to x1: text
   decoration, whose ends and position fall between pixels. */
void d_paint_rule(DPaint *paint, double x0, double x1, double y, double thickness,
                  DRenderColor color);

/* An 8-bit coverage mask -- a rendered glyph -- in `color`, its top-left at
   (x, y), which need not be a whole pixel. */
void d_paint_mask(DPaint *paint, const unsigned char *mask, int width, int height, int pitch,
                  double x, double y, DRenderColor color);

/* Premultiplied 0xAARRGGBB pixels, scaled into `rect`. */
void d_paint_image(DPaint *paint, DRenderRect rect, const uint32_t *pixels,
                   int width, int height);

void d_paint_disc(DPaint *paint, double cx, double cy, double radius, DRenderColor color);

/* Clips nest: each narrows the last, and pop restores the one before. */
void d_paint_clip_push(DPaint *paint, DRenderRect rect);
void d_paint_clip_pop(DPaint *paint);

#endif
