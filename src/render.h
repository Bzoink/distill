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
typedef struct DLiteHtmlDocument DLiteHtmlDocument;

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

/* Geometry and attributes for native controls placed over a reader surface. */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    unsigned int form_index;
    char tag[16];
    char type[32];
    char name[128];
    char value[512];
    char action[2048];
    char method[16];
} DFormBox;

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
void *d_render_surface(DRenderContext *context);

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

/* Draw decoded pixels scaled into rect. The pixels are 0xAARRGGBB with the
   colour channels already multiplied by alpha, which is what d_image_decode
   produces; the caller keeps ownership and may free them on return. */
void d_render_draw_image(DRenderContext *context, DRenderRect rect,
                         const uint32_t *pixels, int width, int height);
void d_render_stroke(DRenderContext *context, DRenderRect rect, int width,
                     DRenderColor color);
void d_render_draw_marker(DRenderContext *context, DRenderRect rect,
                          DRenderColor color, bool ordered, int index);
void d_render_set_clip(DRenderContext *context, DRenderRect rect);
void d_render_del_clip(DRenderContext *context);

/* Upper bound on an auto-sized reader surface, in pixels. */
#define DISTILL_MAX_RENDER_HEIGHT 20000

/* Laid-out height of the document at this width, or 0 if it cannot be laid
   out. Passing height <= 0 to distill_litehtml_render uses this, bounded. */
int distill_litehtml_content_height(const char *html, int width, const char *font_dir);

/* A laid-out reader document and the fixed-size paint surface used to draw
   visible bands from it. The document owns the returned surface. */
DLiteHtmlDocument *distill_litehtml_document_create(const char *html, int width,
                                                     int surface_height,
                                                     const char *font_dir);
void distill_litehtml_document_destroy(DLiteHtmlDocument *document);
int distill_litehtml_document_content_height(const DLiteHtmlDocument *document);
int distill_litehtml_document_render_region(DLiteHtmlDocument *document, int scroll_y);
void *distill_litehtml_document_surface(DLiteHtmlDocument *document);
size_t distill_litehtml_document_form_boxes(const DLiteHtmlDocument *document,
                                            DFormBox *boxes, size_t box_capacity);
/* Anchor URL under a document-space point, written to href_out; non-zero when
   the point is over a link. Answers for inline anchors, which have no box. */
int distill_litehtml_document_link_at(DLiteHtmlDocument *document, int x, int y,
                                      char *href_out, size_t href_capacity);

size_t distill_litehtml_document_link_boxes(const DLiteHtmlDocument *document,
                                            DLinkBox *boxes, size_t box_capacity);

/* One-shot region rendering for callers that do not need to retain a
   document between paints. */
int distill_litehtml_render_region(const char *html, const char *output_path,
                                   int width, int viewport, int scroll_y,
                                   const char *font_dir);

size_t distill_litehtml_form_boxes(const char *html, int width, int height,
                                   const char *font_dir, DFormBox *boxes,
                                   size_t box_capacity);
size_t distill_litehtml_link_boxes(const char *html, int width, int height,
                                   const char *font_dir, DLinkBox *boxes,
                                   size_t box_capacity);

#ifdef __cplusplus
}
#endif

#endif
