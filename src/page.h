#ifndef DISTILL_PAGE_H
#define DISTILL_PAGE_H

/* A reader page laid out and painted: the layout interface.

   The reader page is Distill's own HTML -- the template the engine writes,
   the media viewer -- never a source site's. This is what the engine's
   --render and --viewer, the desktop shell and the paint tools lay it out
   with. One implementation is linked, chosen at link time, as the painter is
   (paint.h). */

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DPage DPage;

/* Upper bound on an auto-sized reader surface, in pixels. */
#define DISTILL_MAX_RENDER_HEIGHT 20000

/* Laid-out height of the document at this width, or 0 if it cannot be laid
   out. Passing height <= 0 to d_page_render_png uses this, bounded. */
int d_page_measure_height(const char *html, int width, const char *font_dir);

/* The whole page painted and written as a PNG. Height <= 0 sizes the surface
   to the content, bounded by DISTILL_MAX_RENDER_HEIGHT. */
int d_page_render_png(const char *html, const char *output_path, int width, int height,
                      const char *font_dir);

/* A laid-out reader document and the fixed-size paint surface used to draw
   visible bands from it. The document owns the returned surface. */
DPage *d_page_create(const char *html, int width,
                                                     int surface_height,
                                                     const char *font_dir);
void d_page_destroy(DPage *document);
int d_page_height(const DPage *document);
int d_page_paint(DPage *document, int scroll_y);
bool d_page_pixels(DPage *document, DRenderPixels *pixels);
size_t d_page_form_boxes(const DPage *document,
                                            DFormBox *boxes, size_t box_capacity);
/* Anchor URL under a document-space point, written to href_out; non-zero when
   the point is over a link. Answers for inline anchors, which have no box. */
int d_page_link_at(DPage *document, int x, int y,
                                      char *href_out, size_t href_capacity);
/* Returns the native input ID when a point belongs to a checkbox/radio label.
   The GTK shell uses this to give the rendered label its normal click target. */
int d_page_choice_id_at(DPage *document, int x, int y,
                                           char *id_out, size_t id_capacity);

size_t d_page_link_boxes(const DPage *document,
                                            DLinkBox *boxes, size_t box_capacity);

/* The text runs of the whole document, in the order they were drawn, which
   for a reader page in normal flow is reading order. Collected once when the
   document is laid out: the shell paints in bands, and a selection that could
   only reach painted text would stop at the edge of the screen. */
size_t d_page_text_boxes(DPage *document,
                                            DTextBox *boxes, size_t box_capacity);
/* Byte offset in run `index` for a point `x` pixels into its box. */
size_t d_page_text_offset_at(DPage *document,
                                                size_t index, int x);
/* The x offset, within run `index`'s box, of the boundary at byte `offset`. */
int d_page_text_x_at(DPage *document,
                                        size_t index, size_t offset);

/* One-shot region rendering for callers that do not need to retain a
   document between paints. */
int d_page_render_region_png(const char *html, const char *output_path,
                                   int width, int viewport, int scroll_y,
                                   const char *font_dir);

size_t d_page_measure_form_boxes(const char *html, int width, int height,
                                   const char *font_dir, DFormBox *boxes,
                                   size_t box_capacity);
size_t d_page_measure_link_boxes(const char *html, int width, int height,
                                   const char *font_dir, DLinkBox *boxes,
                                   size_t box_capacity);

#ifdef __cplusplus
}
#endif

#endif
