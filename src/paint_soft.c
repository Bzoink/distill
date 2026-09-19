/* The software painter: paint.h over a plain buffer of 32-bit pixels.

   Portable C and nothing else, so every target -- the terminal's images, the
   framebuffer, Muroxi, a desktop window -- draws the same pixels. It stands
   in for Cairo, which render.c used to paint with, and is held to Cairo's
   output by `check.py --paint`:

     compositing  premultiplied "over", with pixman's rounding of a*b/255
     glyphs       8-bit coverage masks; placed at a fractional position by
                  bilinear sampling, as Cairo samples a translated mask
     rules        exact area coverage, as Cairo antialiases a stroke
     images       box-filtered when shrunk, bilinear otherwise, transparent
                  outside the source -- Cairo's GOOD filter, EXTEND_NONE
     the disc     4x4 samples per edge pixel

   Built with DISTILL_PAINT_DEFECT it draws every glyph a row low: the
   negative control that shows --paint can see a wrong pixel. */

#include "paint.h"

#include "cutil.h"

#include <math.h>
#include <png.h>

/* The page colour behind everything, as Cairo rounds 0.96, 0.95, 0.92. */
#define PAGE_RED 245
#define PAGE_GREEN 243
#define PAGE_BLUE 235

#define CLIP_DEPTH_MAX 64

struct DPaint {
    int width, height;
    int stride;                    /* in pixels */
    uint32_t *pixels;
    DRenderRect clips[CLIP_DEPTH_MAX];
    int clip_depth;
    int clip_overflow;             /* pushes beyond the stack, popped first */
};

typedef struct {
    int x0, y0, x1, y1;            /* half-open */
} Box;

static inline uint32_t div255(uint32_t value)
{
    value += 0x80u;
    return (value + (value >> 8)) >> 8;
}

static Box current_clip(const DPaint *paint)
{
    if (!paint->clip_depth) return (Box){0, 0, paint->width, paint->height};
    DRenderRect r = paint->clips[paint->clip_depth - 1];
    return (Box){r.x, r.y, r.x + r.width, r.y + r.height};
}

static Box intersect(Box a, Box b)
{
    Box out = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
               a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
    if (out.x1 < out.x0) out.x1 = out.x0;
    if (out.y1 < out.y0) out.y1 = out.y0;
    return out;
}

/* A premultiplied source pixel, weighted by coverage, over the destination. */
static inline void blend(uint32_t *dst, uint32_t source, uint32_t coverage)
{
    if (coverage == 0) return;
    uint32_t sa = source >> 24, sr = (source >> 16) & 0xff, sg = (source >> 8) & 0xff, sb = source & 0xff;
    if (coverage < 255) {
        sa = div255(sa * coverage);
        sr = div255(sr * coverage);
        sg = div255(sg * coverage);
        sb = div255(sb * coverage);
    }
    if (sa == 0 && (sr | sg | sb) == 0) return;
    uint32_t d = *dst;
    uint32_t inverse = 255 - sa;
    uint32_t a = sa + div255((d >> 24) * inverse);
    uint32_t r = sr + div255(((d >> 16) & 0xff) * inverse);
    uint32_t g = sg + div255(((d >> 8) & 0xff) * inverse);
    uint32_t b = sb + div255((d & 0xff) * inverse);
    *dst = (a > 255 ? 255u : a) << 24 | (r > 255 ? 255u : r) << 16 |
           (g > 255 ? 255u : g) << 8 | (b > 255 ? 255u : b);
}

static inline uint32_t premultiply(DRenderColor color)
{
    uint32_t a = color.alpha;
    return a << 24 | div255(color.red * a) << 16 | div255(color.green * a) << 8 | div255(color.blue * a);
}

static inline uint32_t *pixel_at(DPaint *paint, int x, int y)
{
    return paint->pixels + (size_t)y * (size_t)paint->stride + (size_t)x;
}

DPaint *d_paint_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    DPaint *paint = calloc(1, sizeof(*paint));
    if (!paint) return NULL;
    paint->width = width;
    paint->height = height;
    paint->stride = width;
    if ((size_t)height > SIZE_MAX / sizeof(uint32_t) / (size_t)width) { free(paint); return NULL; }
    paint->pixels = malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    if (!paint->pixels) { free(paint); return NULL; }
    d_paint_clear(paint);
    return paint;
}

void d_paint_destroy(DPaint *paint)
{
    if (!paint) return;
    free(paint->pixels);
    free(paint);
}

void d_paint_clear(DPaint *paint)
{
    if (!paint) return;
    uint32_t page = 0xffu << 24 | PAGE_RED << 16 | PAGE_GREEN << 8 | PAGE_BLUE;
    for (int y = 0; y < paint->height; ++y) {
        uint32_t *row = pixel_at(paint, 0, y);
        for (int x = 0; x < paint->width; ++x) row[x] = page;
    }
}

bool d_paint_pixels(DPaint *paint, DRenderPixels *pixels)
{
    if (!paint || !pixels) return false;
    *pixels = (DRenderPixels){paint->pixels, paint->width, paint->height,
                              paint->stride * (int)sizeof(uint32_t)};
    return true;
}

bool d_paint_write_png(DPaint *paint, const char *path)
{
    if (!paint || !path) return false;
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = (png_uint_32)paint->width;
    image.height = (png_uint_32)paint->height;
    /* The words' bytes in memory, which is what libpng reads. The surface is
       opaque, so premultiplied and straight colour are the same. */
    const uint32_t probe = 1;
    bool little = *(const unsigned char *)&probe == 1;
    image.format = little ? PNG_FORMAT_BGRA : PNG_FORMAT_ARGB;
    int ok = png_image_write_to_file(&image, path, 0, paint->pixels,
                                     paint->stride * 4, NULL);
    png_image_free(&image);
    return ok != 0;
}

void d_paint_fill(DPaint *paint, DRenderRect rect, DRenderColor color)
{
    if (!paint || !color.alpha || rect.width <= 0 || rect.height <= 0) return;
    Box box = intersect(current_clip(paint),
                        (Box){rect.x, rect.y, rect.x + rect.width, rect.y + rect.height});
    uint32_t source = premultiply(color);
    for (int y = box.y0; y < box.y1; ++y) {
        uint32_t *row = pixel_at(paint, 0, y);
        if (color.alpha == 255) {
            for (int x = box.x0; x < box.x1; ++x) row[x] = source;
        } else {
            for (int x = box.x0; x < box.x1; ++x) blend(&row[x], source, 255);
        }
    }
}

void d_paint_stroke(DPaint *paint, DRenderRect rect, int width, DRenderColor color)
{
    if (!paint || width <= 0 || !color.alpha) return;
    if (rect.width <= 2 * width || rect.height <= 2 * width) {
        d_paint_fill(paint, rect, color);
        return;
    }
    /* Four bands that do not overlap, so a translucent border is laid once. */
    d_paint_fill(paint, (DRenderRect){rect.x, rect.y, rect.width, width}, color);
    d_paint_fill(paint, (DRenderRect){rect.x, rect.y + rect.height - width, rect.width, width}, color);
    d_paint_fill(paint, (DRenderRect){rect.x, rect.y + width, width, rect.height - 2 * width}, color);
    d_paint_fill(paint, (DRenderRect){rect.x + rect.width - width, rect.y + width, width,
                                      rect.height - 2 * width}, color);
}

/* How much of [cell, cell + 1) lies within [from, to]. */
static inline double overlap(int cell, double from, double to)
{
    double lo = from > cell ? from : cell, hi = to < cell + 1 ? to : cell + 1;
    return hi > lo ? hi - lo : 0.0;
}

void d_paint_rule(DPaint *paint, double x0, double x1, double y, double thickness,
                  DRenderColor color)
{
    if (!paint || !color.alpha || x1 <= x0 || thickness <= 0) return;
    double top = y - thickness / 2, bottom = y + thickness / 2;
    Box box = intersect(current_clip(paint),
                        (Box){(int)floor(x0), (int)floor(top), (int)ceil(x1), (int)ceil(bottom)});
    uint32_t source = premultiply(color);
    for (int py = box.y0; py < box.y1; ++py) {
        double cover_y = overlap(py, top, bottom);
        uint32_t *row = pixel_at(paint, 0, py);
        for (int px = box.x0; px < box.x1; ++px) {
            double cover = cover_y * overlap(px, x0, x1);
            blend(&row[px], source, (uint32_t)lround(cover * 255.0));
        }
    }
}

static inline uint32_t mask_at(const unsigned char *mask, int width, int height, int pitch,
                               int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    return mask[(size_t)y * (size_t)pitch + (size_t)x];
}

void d_paint_mask(DPaint *paint, const unsigned char *mask, int width, int height, int pitch,
                  double x, double y, DRenderColor color)
{
    if (!paint || !mask || width <= 0 || height <= 0 || !color.alpha) return;
#ifdef DISTILL_PAINT_DEFECT
    y += 1.0;
#endif
    uint32_t source = premultiply(color);
    double fx = x - floor(x), fy = y - floor(y);
    int left = (int)floor(x), top = (int)floor(y);
    bool whole = fx == 0.0 && fy == 0.0;
    Box box = intersect(current_clip(paint),
                        (Box){left, top, left + width + (whole ? 0 : 1), top + height + (whole ? 0 : 1)});
    /* Weights of the four mask texels behind a destination pixel, in 1/256. */
    uint32_t wx = (uint32_t)lround(fx * 256.0), wy = (uint32_t)lround(fy * 256.0);
    for (int py = box.y0; py < box.y1; ++py) {
        uint32_t *row = pixel_at(paint, 0, py);
        int my = py - top;
        for (int px = box.x0; px < box.x1; ++px) {
            int mx = px - left;
            uint32_t coverage;
            if (whole) {
                coverage = mask[(size_t)my * (size_t)pitch + (size_t)mx];
            } else {
                /* The texel to the upper left of this pixel's centre and the
                   three beside it; the mask starts fx, fy into the pixel. */
                uint32_t a = mask_at(mask, width, height, pitch, mx - 1, my - 1);
                uint32_t b = mask_at(mask, width, height, pitch, mx, my - 1);
                uint32_t c = mask_at(mask, width, height, pitch, mx - 1, my);
                uint32_t d = mask_at(mask, width, height, pitch, mx, my);
                uint32_t upper = a * wx + b * (256 - wx);
                uint32_t lower = c * wx + d * (256 - wx);
                coverage = (upper * wy + lower * (256 - wy) + 32768) >> 16;
            }
            blend(&row[px], source, coverage);
        }
    }
}

/* A source pixel, transparent outside the image. */
static inline uint32_t source_at(const uint32_t *pixels, int width, int height, int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    return pixels[(size_t)y * (size_t)width + (size_t)x];
}

static uint32_t bilinear(const uint32_t *pixels, int width, int height, double u, double v)
{
    int x = (int)floor(u), y = (int)floor(v);
    double fx = u - x, fy = v - y;
    uint32_t corner[4] = {source_at(pixels, width, height, x, y),
                          source_at(pixels, width, height, x + 1, y),
                          source_at(pixels, width, height, x, y + 1),
                          source_at(pixels, width, height, x + 1, y + 1)};
    double weight[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        double sum = 0;
        for (int i = 0; i < 4; ++i) sum += weight[i] * ((corner[i] >> shift) & 0xff);
        long channel = lround(sum);
        out |= (uint32_t)(channel < 0 ? 0 : channel > 255 ? 255 : channel) << shift;
    }
    return out;
}

/* The mean of the source over [u0, u1) x [v0, v1), each texel weighted by
   how much of it the footprint covers. */
static uint32_t box_average(const uint32_t *pixels, int width, int height,
                            double u0, double u1, double v0, double v1)
{
    double sum[4] = {0}, area = 0;
    for (int y = (int)floor(v0); y < (int)ceil(v1); ++y) {
        double cy = overlap(y, v0, v1);
        for (int x = (int)floor(u0); x < (int)ceil(u1); ++x) {
            double c = cy * overlap(x, u0, u1);
            uint32_t p = source_at(pixels, width, height, x, y);
            for (int i = 0; i < 4; ++i) sum[i] += c * ((p >> (i * 8)) & 0xff);
            area += c;
        }
    }
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        long channel = area > 0 ? lround(sum[i] / area) : 0;
        out |= (uint32_t)(channel > 255 ? 255 : channel) << (i * 8);
    }
    return out;
}

void d_paint_image(DPaint *paint, DRenderRect rect, const uint32_t *pixels,
                   int width, int height)
{
    if (!paint || !pixels || width <= 0 || height <= 0 || rect.width <= 0 || rect.height <= 0)
        return;
    Box box = intersect(current_clip(paint),
                        (Box){rect.x, rect.y, rect.x + rect.width, rect.y + rect.height});
    double sx = (double)width / rect.width, sy = (double)height / rect.height;
    /* Cairo's GOOD filter: a box filter once the image is shrunk by more than
       a third, bilinear otherwise. */
    bool shrink = sx > 1.34 || sy > 1.34;
    for (int py = box.y0; py < box.y1; ++py) {
        uint32_t *row = pixel_at(paint, 0, py);
        double dy = py - rect.y;
        for (int px = box.x0; px < box.x1; ++px) {
            double dx = px - rect.x;
            uint32_t source = shrink
                ? box_average(pixels, width, height, dx * sx, (dx + 1) * sx, dy * sy, (dy + 1) * sy)
                : bilinear(pixels, width, height, (dx + 0.5) * sx - 0.5, (dy + 0.5) * sy - 0.5);
            blend(&row[px], source, 255);
        }
    }
}

void d_paint_disc(DPaint *paint, double cx, double cy, double radius, DRenderColor color)
{
    if (!paint || radius <= 0 || !color.alpha) return;
    Box box = intersect(current_clip(paint),
                        (Box){(int)floor(cx - radius), (int)floor(cy - radius),
                              (int)ceil(cx + radius), (int)ceil(cy + radius)});
    uint32_t source = premultiply(color);
    double r2 = radius * radius;
    for (int py = box.y0; py < box.y1; ++py) {
        uint32_t *row = pixel_at(paint, 0, py);
        for (int px = box.x0; px < box.x1; ++px) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    double ux = px + (sx + 0.5) / 4 - cx, uy = py + (sy + 0.5) / 4 - cy;
                    inside += ux * ux + uy * uy <= r2;
                }
            blend(&row[px], source, (uint32_t)(inside * 255 + 8) / 16);
        }
    }
}

void d_paint_clip_push(DPaint *paint, DRenderRect rect)
{
    if (!paint) return;
    if (paint->clip_depth == CLIP_DEPTH_MAX) { ++paint->clip_overflow; return; }
    Box narrowed = intersect(current_clip(paint),
                             (Box){rect.x, rect.y, rect.x + rect.width, rect.y + rect.height});
    paint->clips[paint->clip_depth++] = (DRenderRect){narrowed.x0, narrowed.y0,
                                                      narrowed.x1 - narrowed.x0,
                                                      narrowed.y1 - narrowed.y0};
}

void d_paint_clip_pop(DPaint *paint)
{
    if (!paint) return;
    if (paint->clip_overflow) { --paint->clip_overflow; return; }
    if (paint->clip_depth) --paint->clip_depth;
}
