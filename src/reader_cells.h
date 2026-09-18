#ifndef DISTILL_READER_CELLS_H
#define DISTILL_READER_CELLS_H

/* The reader page laid out in character cells.

   A pure function: a reader page and a width in, a grid out. It does not open
   a terminal, ask one anything or know which library will draw it. That is
   what lets --dump-cells assert on terminal rendering with no tty present,
   and what lets a framebuffer console reuse this whole rather than beside a
   second I/O layer. The terminal shell is input and output around it.

   The grid is as tall as the page. A shell shows a window onto it; the rows
   and columns recorded for links, controls, anchors, images and tables are
   grid coordinates, so scrolling is an offset and never a relayout. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    READER_ROLE_TEXT,
    READER_ROLE_HEADING,
    READER_ROLE_LINK,
    READER_ROLE_BUTTON,
    READER_ROLE_MUTED,
    READER_ROLE_CODE,
    READER_ROLE_RULE,
    READER_ROLE_FIELD,
    READER_ROLE_BORDER,
} ReaderRole;

enum {
    READER_CELL_BOLD      = 1u << 0,
    READER_CELL_ITALIC    = 1u << 1,
    READER_CELL_UNDERLINE = 1u << 2,
};

/* One cell. A grapheme cluster occupies `width` cells: the first holds the
   cluster, the rest are continuations with width 0. A blank cell has width 1
   and no text. */
typedef struct {
    uint32_t text;       /* offset into the layout's pool */
    uint8_t length;      /* bytes of the cluster; 0 for blank or continuation */
    uint8_t width;
    uint8_t role;        /* ReaderRole */
    uint8_t attrs;       /* READER_CELL_* */
    int32_t link;        /* index into links, or -1 */
    int32_t control;     /* index into controls, or -1 */
} ReaderCell;

typedef struct {
    char *href;
    bool button;
    int row, column;     /* first cell */
} ReaderLayoutLink;

/* A form control, owning copies of the fields the walker reported, plus
   where it was drawn. `width` is 0 for a control with no cells (hidden). */
typedef struct {
    bool in_form;
    unsigned form_index;
    char *action, *method, *tag, *type, *id, *name, *value, *placeholder, *caption, *options;
    bool checked, disabled, required;
    int row, column, width;
} ReaderLayoutControl;

typedef struct {
    char *id;
    int row;
} ReaderLayoutAnchor;

typedef struct {
    char *src, *alt;
    int link;                  /* the link it sits in, or -1 */
    int row, column, width, rows;
} ReaderLayoutImage;

/* A table wider than the measure keeps its full grid here and shows a
   window of it; reader_layout_scroll_table moves the window. */
typedef struct {
    int row, rows;             /* where it sits in the page grid */
    int column, visible;       /* its left edge and visible width there */
    int full_width;
    int offset;
    ReaderCell *cells;         /* rows * full_width */
} ReaderLayoutTable;

typedef struct {
    int columns;               /* the terminal's width */
    /* The reading measure. 0 means min(columns - 2, 80): a reader that lets
       prose run the width of a wide terminal has misunderstood the job. */
    int measure;
    /* Rows an image is given for drawing. 0 draws a one-line alt-text
       placeholder, which is what a terminal without graphics shows. */
    int image_rows;
} ReaderLayoutOptions;

typedef struct {
    int columns, rows;
    int left, measure;         /* the reading column */
    ReaderCell *cells;         /* rows * columns */
    char *pool;
    size_t pool_length, pool_capacity;
    char *title, *dir, *lang;
    ReaderLayoutLink *links;       size_t link_count;
    ReaderLayoutControl *controls; size_t control_count;
    ReaderLayoutAnchor *anchors;   size_t anchor_count;
    ReaderLayoutImage *images;     size_t image_count;
    ReaderLayoutTable *tables;     size_t table_count;
} ReaderLayout;

/* NULL only when the page cannot be parsed or `columns` is below 8. */
ReaderLayout *reader_layout(const char *html, const ReaderLayoutOptions *options);
void reader_layout_free(ReaderLayout *layout);

static inline ReaderCell *reader_layout_cell(ReaderLayout *layout, int row, int column)
{
    return &layout->cells[(size_t)row * (size_t)layout->columns + (size_t)column];
}

/* The cluster a cell holds, or "" for a blank or a continuation. */
const char *reader_cell_text(const ReaderLayout *layout, const ReaderCell *cell, size_t *length);

/* Rows [first, first + count) as plain text, one line per row, trailing
   blanks trimmed. count <= 0 means to the end of the page. */
char *reader_layout_dump(const ReaderLayout *layout, int first, int count);

void reader_layout_scroll_table(ReaderLayout *layout, size_t table, int offset);

/* Redraw a control in place with a new state -- what the user typed, what
   they ticked -- without laying the page out again. */
void reader_layout_paint_control(ReaderLayout *layout, size_t control,
                                 const char *value, bool checked);

/* Cells a UTF-8 string occupies, cluster by cluster: the measure the line
   breaker is given. Exposed for the shell's own lines (status, prompts). */
int reader_text_cells(const char *text, size_t length);

/* The same text one grapheme cluster at a time, with the cells each takes:
   0 for a cluster that is not drawn at all. What the shell uses for text it
   writes itself, so it measures exactly as the page does. */
typedef void (*ReaderClusterVisit)(void *opaque, const char *bytes, size_t length, int width);
void reader_text_clusters(const char *text, size_t length, ReaderClusterVisit visit, void *opaque);

#endif
