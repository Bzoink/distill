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

/* Pixels a context has drawn: 32-bit words, 0xAARRGGBB with colour
   multiplied by alpha -- Cairo's ARGB32, a framebuffer's XRGB8888 -- and
   `stride` bytes from one row to the next, which callers must not assume is
   width * 4. The context owns them. */
typedef struct {
    uint32_t *data;
    int width;
    int height;
    int stride;
} DRenderPixels;

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

/* Geometry and attributes for native controls placed over a reader surface. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    unsigned int form_index;
    char tag[16];
    char type[32];
    char id[64];
    char name[128];
    char value[512];
    char caption[512];
    char placeholder[256];
    /* Each select choice is value, label, selectedness, separated by ASCII
       unit/record separators. The renderer owns the markup; this compact C
       ABI avoids exposing the layout tree to the GTK shell. */
    char options[4096];
    char action[2048];
    char method[16];
    bool checked;
    bool disabled;
    bool required;
} DFormBox;

/* One run of text as the renderer drew it: the string, the box it filled, and
   the block it belongs to. Selection is built from these -- the layout records
   each run already broken to a line, which is the granularity a reader
   selects at. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    /* Runs sharing a block are one paragraph when copied; a change of block is
       a line break. The id is only meaningful within one document. */
    unsigned int block;
    /* The block's tag, so the caller can tell a list item from a paragraph
       without a second lookup. */
    char block_tag[12];
    char text[256];
} DTextBox;

/* Hit-test data for links in the rendered reader template. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    char href[2048];
} DLinkBox;

DRenderContext *d_render_create(int width, int height, const char *font_dir);
void d_render_destroy(DRenderContext *context);
int d_render_write_png(DRenderContext *context, const char *path);
void d_render_clear(DRenderContext *context);
bool d_render_pixels(DRenderContext *context, DRenderPixels *pixels);

DRenderFont *d_render_font_create(DRenderContext *context, const char *face_name,
                                  int size, int weight, bool italic,
                                  unsigned int decoration, DRenderFontMetrics *metrics);
void d_render_font_destroy(DRenderFont *font);
int d_render_text_width(DRenderFont *font, const char *text);
/* Where a point falls in a run, and where a character sits in it. Both answer
   in cluster steps rather than bytes: a ligature is one place to stand and a
   combining mark belongs to the letter it is on, so a selection can never cut
   a character in half. Shaping is what knows the clusters, which is why these
   live beside the shaper rather than in the shell. Both read a run in logical
   order, which is the selection model the reader offers. */
size_t d_render_text_offset_at(DRenderFont *font, const char *text, int x);
int d_render_text_x_at(DRenderFont *font, const char *text, size_t offset);
size_t d_render_wrap_text(DRenderFont *font, const char *text, int max_width,
                          DRenderTextLine *lines, size_t line_capacity);
void d_render_draw_text(DRenderContext *context, const char *text, DRenderFont *font,
                        DRenderColor color, DRenderRect position);

void d_render_fill(DRenderContext *context, DRenderRect rect, DRenderColor color);

/* Draw decoded pixels scaled into rect. The pixels are 0xAARRGGBB with the
   colour channels already multiplied by alpha, which is what d_image_decode
   produces; the caller keeps ownership and may free them on return. */
void d_render_draw_image(DRenderContext *context, DRenderRect rect,
                         const uint32_t *pixels, int width, int height);
void d_render_stroke(DRenderContext *context, DRenderRect rect, int width,
                     DRenderColor color);
void d_render_draw_marker(DRenderContext *context, DRenderRect rect,
                          DRenderColor color, bool ordered, int index);
/* A filled circle, its centre and radius free to fall between pixels. */
void d_render_disc(DRenderContext *context, double cx, double cy, double radius,
                   DRenderColor color);
/* Another context's pixels -- a layer painted once and composited many
   times -- copied opaque with their top-left at (x, y). */
void d_render_blit(DRenderContext *context, int x, int y, const DRenderPixels *source);
void d_render_set_clip(DRenderContext *context, DRenderRect rect);
void d_render_del_clip(DRenderContext *context);

#ifdef __cplusplus
}
#endif

#endif
