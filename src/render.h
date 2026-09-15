#ifndef DISTILL_RENDER_H
#define DISTILL_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DRenderContext DRenderContext;
typedef struct DRenderFont DRenderFont;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} DRenderColor;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} DRenderRect;

typedef struct {
    int height;
    int ascent;
    int descent;
    int x_height;
    bool draw_spaces;
} DRenderFontMetrics;

typedef struct {
    size_t begin;
    size_t end;
} DRenderTextLine;

DRenderContext *d_render_create(int width, int height, const char *font_dir);
void d_render_destroy(DRenderContext *context);
int d_render_write_png(DRenderContext *context, const char *path);

DRenderFont *d_render_font_create(DRenderContext *context, const char *face_name,
                                  int size, int weight, bool italic,
                                  unsigned int decoration, DRenderFontMetrics *metrics);
void d_render_font_destroy(DRenderFont *font);
int d_render_text_width(DRenderFont *font, const char *text);
size_t d_render_wrap_text(DRenderFont *font, const char *text, int max_width,
                          DRenderTextLine *lines, size_t line_capacity);
void d_render_draw_text(DRenderContext *context, const char *text, DRenderFont *font,
                        DRenderColor color, DRenderRect position);

void d_render_fill(DRenderContext *context, DRenderRect rect, DRenderColor color);
void d_render_stroke(DRenderContext *context, DRenderRect rect, int width,
                     DRenderColor color);
void d_render_draw_marker(DRenderContext *context, DRenderRect rect,
                          DRenderColor color, bool ordered, int index);
void d_render_set_clip(DRenderContext *context, DRenderRect rect);
void d_render_del_clip(DRenderContext *context);

#ifdef __cplusplus
}
#endif

#endif
