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
    char id[64];
    char name[128];
    char value[512];
    char caption[512];
    char placeholder[256];
    /* Each select choice is value, label, selectedness, separated by ASCII
       unit/record separators. The renderer owns the markup; this compact C
       ABI avoids exposing litehtml's C++ tree to the GTK shell. */
    char options[4096];
    char action[2048];
    char method[16];
    bool checked;
    bool disabled;
    bool required;
} DFormBox;

/* One run of text as the renderer drew it: the string, the box it filled, and
   the block it belongs to. Selection is built from these -- litehtml hands the
   container each run already broken to a line, which is the granularity a
   reader selects at. */
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
void *d_render_surface(DRenderContext *context);

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
/* Returns the native input ID when a point belongs to a checkbox/radio label.
   The GTK shell uses this to give the rendered label its normal click target. */
int distill_litehtml_document_choice_id_at(DLiteHtmlDocument *document, int x, int y,
                                           char *id_out, size_t id_capacity);

size_t distill_litehtml_document_link_boxes(const DLiteHtmlDocument *document,
                                            DLinkBox *boxes, size_t box_capacity);

/* The text runs of the whole document, in the order they were drawn, which
   for a reader page in normal flow is reading order. Collected once when the
   document is laid out: the shell paints in bands, and a selection that could
   only reach painted text would stop at the edge of the screen. */
size_t distill_litehtml_document_text_boxes(DLiteHtmlDocument *document,
                                            DTextBox *boxes, size_t box_capacity);
/* Byte offset in run `index` for a point `x` pixels into its box. */
size_t distill_litehtml_document_text_offset_at(DLiteHtmlDocument *document,
                                                size_t index, int x);
/* The x offset, within run `index`'s box, of the boundary at byte `offset`. */
int distill_litehtml_document_text_x_at(DLiteHtmlDocument *document,
                                        size_t index, size_t offset);

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
