/* The reference painter: paint.h over Cairo, as render.c painted before the
   software painter replaced it. Linked only into distill-paint-cairo, the
   tool `check.py --paint` compares the software painter against; nothing
   that ships links Cairo. Each operation is the Cairo code render.c had. */

#include "paint.h"

#include <cairo/cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct DPaint {
    cairo_surface_t *surface;
    cairo_t *cairo;
};

static void set_color(cairo_t *cairo, DRenderColor color)
{
    cairo_set_source_rgba(cairo, color.red / 255.0, color.green / 255.0,
                          color.blue / 255.0, color.alpha / 255.0);
}

DPaint *d_paint_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    DPaint *paint = calloc(1, sizeof(*paint));
    if (!paint) return NULL;
    paint->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    paint->cairo = cairo_create(paint->surface);
    if (cairo_status(paint->cairo) != CAIRO_STATUS_SUCCESS) { d_paint_destroy(paint); return NULL; }
    cairo_set_source_rgb(paint->cairo, 0.96, 0.95, 0.92);
    cairo_paint(paint->cairo);
    return paint;
}

void d_paint_destroy(DPaint *paint)
{
    if (!paint) return;
    if (paint->cairo) cairo_destroy(paint->cairo);
    if (paint->surface) cairo_surface_destroy(paint->surface);
    free(paint);
}

void d_paint_clear(DPaint *paint)
{
    cairo_save(paint->cairo);
    cairo_reset_clip(paint->cairo);
    cairo_set_source_rgb(paint->cairo, 0.96, 0.95, 0.92);
    cairo_set_operator(paint->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_paint(paint->cairo);
    cairo_restore(paint->cairo);
}

bool d_paint_pixels(DPaint *paint, DRenderPixels *pixels)
{
    if (!paint || !pixels) return false;
    cairo_surface_flush(paint->surface);
    *pixels = (DRenderPixels){(uint32_t *)cairo_image_surface_get_data(paint->surface),
                              cairo_image_surface_get_width(paint->surface),
                              cairo_image_surface_get_height(paint->surface),
                              cairo_image_surface_get_stride(paint->surface)};
    return true;
}

bool d_paint_write_png(DPaint *paint, const char *path)
{
    cairo_surface_flush(paint->surface);
    return cairo_surface_write_to_png(paint->surface, path) == CAIRO_STATUS_SUCCESS;
}

void d_paint_fill(DPaint *paint, DRenderRect rect, DRenderColor color)
{
    cairo_save(paint->cairo);
    set_color(paint->cairo, color);
    cairo_rectangle(paint->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_fill(paint->cairo);
    cairo_restore(paint->cairo);
}

void d_paint_stroke(DPaint *paint, DRenderRect rect, int width, DRenderColor color)
{
    cairo_save(paint->cairo);
    set_color(paint->cairo, color);
    cairo_set_line_width(paint->cairo, width);
    cairo_rectangle(paint->cairo, rect.x + width / 2.0, rect.y + width / 2.0,
                    rect.width - width, rect.height - width);
    cairo_stroke(paint->cairo);
    cairo_restore(paint->cairo);
}

void d_paint_rule(DPaint *paint, double x0, double x1, double y, double thickness,
                  DRenderColor color)
{
    cairo_save(paint->cairo);
    set_color(paint->cairo, color);
    cairo_set_line_width(paint->cairo, thickness);
    cairo_move_to(paint->cairo, x0, y);
    cairo_line_to(paint->cairo, x1, y);
    cairo_stroke(paint->cairo);
    cairo_restore(paint->cairo);
}

void d_paint_mask(DPaint *paint, const unsigned char *mask, int width, int height, int pitch,
                  double x, double y, DRenderColor color)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return;
    unsigned char *target = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int row = 0; row < height; ++row)
        memcpy(target + row * stride, mask + (size_t)row * (size_t)pitch, (size_t)width);
    cairo_surface_mark_dirty(surface);
    set_color(paint->cairo, color);
    cairo_mask_surface(paint->cairo, surface, x, y);
    cairo_surface_destroy(surface);
}

void d_paint_image(DPaint *paint, DRenderRect rect, const uint32_t *pixels,
                   int width, int height)
{
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride < 0 || rect.width <= 0 || rect.height <= 0) return;
    unsigned char *packed = malloc((size_t)stride * (size_t)height);
    if (!packed) return;
    for (int y = 0; y < height; ++y)
        memcpy(packed + (size_t)y * (size_t)stride, pixels + (size_t)y * (size_t)width,
               (size_t)width * 4);
    cairo_surface_t *image = cairo_image_surface_create_for_data(packed, CAIRO_FORMAT_ARGB32,
                                                                 width, height, stride);
    cairo_save(paint->cairo);
    cairo_rectangle(paint->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_clip(paint->cairo);
    cairo_translate(paint->cairo, rect.x, rect.y);
    cairo_scale(paint->cairo, (double)rect.width / width, (double)rect.height / height);
    cairo_set_source_surface(paint->cairo, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(paint->cairo), CAIRO_FILTER_GOOD);
    cairo_paint(paint->cairo);
    cairo_restore(paint->cairo);
    cairo_surface_finish(image);
    cairo_surface_destroy(image);
    free(packed);
}

void d_paint_disc(DPaint *paint, double cx, double cy, double radius, DRenderColor color)
{
    cairo_save(paint->cairo);
    set_color(paint->cairo, color);
    cairo_arc(paint->cairo, cx, cy, radius, 0, 2 * M_PI);
    cairo_fill(paint->cairo);
    cairo_restore(paint->cairo);
}

void d_paint_clip_push(DPaint *paint, DRenderRect rect)
{
    cairo_save(paint->cairo);
    cairo_rectangle(paint->cairo, rect.x, rect.y, rect.width, rect.height);
    cairo_clip(paint->cairo);
}

void d_paint_clip_pop(DPaint *paint)
{
    cairo_restore(paint->cairo);
}
