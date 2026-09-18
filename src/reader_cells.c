#define _POSIX_C_SOURCE 200809L

#include "reader_cells.h"

#include "cell_width.h"
#include "cutil.h"
#include "reader_walk.h"
#include "shaper.h"

#include <graphemebreak.h>

/* ---- clusters ------------------------------------------------------------ */

static uint32_t utf8_decode(const unsigned char *s, size_t length, size_t *used)
{
    if (!length) { *used = 0; return 0; }
    unsigned char c = s[0];
    size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
    if (!need || need > length) { *used = 1; return 0xfffd; }
    uint32_t cp = need == 1 ? c : need == 2 ? (c & 0x1f) : need == 3 ? (c & 0x0f) : (c & 0x07);
    for (size_t i = 1; i < need; ++i) {
        if ((s[i] & 0xc0) != 0x80) { *used = 1; return 0xfffd; }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *used = need;
    return cp;
}

/* Cells one grapheme cluster takes. A wide base makes the cluster wide --
   CJK with its marks, emoji sequences, flags. Otherwise the code points' own
   widths add up, which is how terminals draw an Indic conjunct, and an emoji
   presentation selector widens a narrow base. 0 means the cluster is not
   drawn at all: a control character, or a zero-width one standing alone. */
static int cluster_width(const char *bytes, size_t length)
{
    size_t used = 0;
    uint32_t first = utf8_decode((const unsigned char *)bytes, length, &used);
    if (first < 0x20 || (first >= 0x7f && first < 0xa0)) return 0;
    int width = d_cell_width(first);
    if (width == 2) return 2;
    bool presentation = false;
    for (size_t at = used; at < length;) {
        uint32_t cp = utf8_decode((const unsigned char *)bytes + at, length - at, &used);
        if (cp == 0xfe0f) presentation = true;
        width += d_cell_width(cp);
        at += used;
    }
    if (presentation && width < 2) return 2;
    return width;
}

typedef struct {
    const char *text;
    size_t length;
    char *breaks;
    size_t at;
} Clusters;

static void clusters_init(Clusters *clusters, const char *text, size_t length)
{
    static bool initialised = false;
    if (!initialised) { init_graphemebreak(); initialised = true; }
    clusters->text = text;
    clusters->length = length;
    clusters->at = 0;
    clusters->breaks = d_xmalloc(length ? length : 1);
    if (length) set_graphemebreaks_utf8((const utf8_t *)text, length, NULL, clusters->breaks);
}

static bool clusters_next(Clusters *clusters, size_t *start, size_t *end)
{
    if (clusters->at >= clusters->length) return false;
    size_t i = clusters->at;
    while (i < clusters->length && clusters->breaks[i] != GRAPHEMEBREAK_BREAK) ++i;
    *start = clusters->at;
    *end = i < clusters->length ? i + 1 : clusters->length;
    clusters->at = *end;
    return true;
}

static void clusters_free(Clusters *clusters)
{
    free(clusters->breaks);
    clusters->breaks = NULL;
}

int reader_text_cells(const char *text, size_t length)
{
    Clusters clusters;
    clusters_init(&clusters, text, length);
    int width = 0;
    size_t start, end;
    while (clusters_next(&clusters, &start, &end))
        width += cluster_width(text + start, end - start);
    clusters_free(&clusters);
    return width;
}

void reader_text_clusters(const char *text, size_t length, ReaderClusterVisit visit, void *opaque)
{
    Clusters clusters;
    clusters_init(&clusters, text, length);
    size_t start, end;
    while (clusters_next(&clusters, &start, &end))
        visit(opaque, text + start, end - start, cluster_width(text + start, end - start));
    clusters_free(&clusters);
}

static int measure_cells(void *opaque, const char *text, size_t length)
{
    (void)opaque;
    return reader_text_cells(text, length);
}

/* ---- the grid ------------------------------------------------------------ */

typedef struct {
    uint8_t role;
    uint8_t attrs;
    int32_t link;
    int32_t control;
} Style;

static const Style plain = {READER_ROLE_TEXT, 0, -1, -1};

static uint32_t pool_add(ReaderLayout *layout, const char *bytes, size_t length)
{
    if (layout->pool_length + length + 1 > layout->pool_capacity) {
        size_t capacity = layout->pool_capacity ? layout->pool_capacity * 2 : 4096;
        while (capacity < layout->pool_length + length + 1) capacity *= 2;
        layout->pool = d_xrealloc(layout->pool, capacity);
        layout->pool_capacity = capacity;
    }
    uint32_t offset = (uint32_t)layout->pool_length;
    memcpy(layout->pool + offset, bytes, length);
    layout->pool_length += length;
    layout->pool[layout->pool_length] = '\0';
    return offset;
}

static ReaderCell blank_cell(Style style)
{
    return (ReaderCell){.width = 1, .role = style.role, .attrs = style.attrs,
                        .link = style.link, .control = style.control};
}

static void fill_blank(ReaderCell *cells, size_t count)
{
    for (size_t i = 0; i < count; ++i) cells[i] = blank_cell(plain);
}

static int new_row(ReaderLayout *layout)
{
    size_t row = (size_t)layout->rows;
    size_t columns = (size_t)layout->columns;
    layout->cells = d_xrealloc(layout->cells, (row + 1) * columns * sizeof(*layout->cells));
    fill_blank(layout->cells + row * columns, columns);
    return layout->rows++;
}

/* Write one cluster into a row of `width` cells. A cluster that would run
   past the edge is not drawn: half of a wide character is not a character. */
static int put_cluster(ReaderLayout *layout, ReaderCell *row, int row_width, int column,
                       const char *bytes, size_t length, int width, Style style)
{
    if (width <= 0 || column < 0 || column + width > row_width) return column;
    if (length > 255) length = 255;
    ReaderCell *cell = &row[column];
    *cell = blank_cell(style);
    cell->text = pool_add(layout, bytes, length);
    cell->length = (uint8_t)length;
    cell->width = (uint8_t)width;
    for (int i = 1; i < width; ++i) {
        row[column + i] = blank_cell(style);
        row[column + i].width = 0;
    }
    return column + width;
}

static int put_text(ReaderLayout *layout, ReaderCell *row, int row_width, int column,
                    const char *text, Style style)
{
    size_t length = strlen(text);
    Clusters clusters;
    clusters_init(&clusters, text, length);
    size_t start, end;
    while (clusters_next(&clusters, &start, &end)) {
        int width = cluster_width(text + start, end - start);
        if (width) column = put_cluster(layout, row, row_width, column, text + start,
                                        end - start, width, style);
    }
    clusters_free(&clusters);
    return column;
}

static ReaderCell *grid_row(ReaderLayout *layout, int row)
{
    return layout->cells + (size_t)row * (size_t)layout->columns;
}

/* ---- the layout state ---------------------------------------------------- */

/* A frame is what one enclosing block puts at the start of each line: a list
   item's marker then its hanging indent, a quote's or a card's bar. A bar
   continues through the blank lines inside its block; an indent does not
   need to. */
typedef struct {
    char *first;
    char *rest;
    Style style;
    bool bar;
    bool used;
} Frame;

typedef struct {
    ReaderBlockKind kind;
    bool frame;
    bool muted;
} Open;

typedef struct {
    char **cells;
    bool *header;
    size_t count;
} TableRow;

typedef struct {
    ReaderLayout *layout;
    ReaderLayoutOptions options;
    bool rtl;

    DBuffer text;
    Style *styles;
    size_t styles_capacity;
    /* Collapsed whitespace waits here until the next character arrives,
       because only then is it known whether it survives. */
    bool pending_space;
    bool pending_segment;      /* the whitespace held a line break */
    Style pending_style;
    bool text_is_pre;

    int strong, em, code, pre, muted, heading;
    int32_t link_stack[16];
    bool button_stack[16];
    int link_depth;

    Frame frames[32];
    int frame_count;
    Open open[64];
    int open_count;
    bool ordered[16];
    size_t counter[16];
    int list_depth;

    bool blank_pending;
    bool last_row_blank;
    char **pending_anchors;
    size_t pending_anchor_count;

    bool in_table;
    TableRow *table_rows;
    size_t table_row_count;
} Cells;

static Style current_style(const Cells *cells)
{
    Style style = plain;
    int32_t link = cells->link_depth ? cells->link_stack[cells->link_depth - 1] : -1;
    bool button = cells->link_depth && cells->button_stack[cells->link_depth - 1];
    if (cells->code) style.role = READER_ROLE_CODE;
    else if (link >= 0) style.role = button ? READER_ROLE_BUTTON : READER_ROLE_LINK;
    else if (cells->heading) style.role = READER_ROLE_HEADING;
    else if (cells->muted) style.role = READER_ROLE_MUTED;
    if (cells->strong || cells->heading) style.attrs |= READER_CELL_BOLD;
    if (cells->em) style.attrs |= READER_CELL_ITALIC;
    if (link >= 0 && !button) style.attrs |= READER_CELL_UNDERLINE;
    style.link = link;
    return style;
}

static void inline_raw(Cells *cells, const char *bytes, size_t length, Style style)
{
    size_t at = cells->text.length;
    dbuf_append(&cells->text, bytes, length);
    d_vec_reserve((void **)&cells->styles, &cells->styles_capacity, at,
                  sizeof(*cells->styles), at + length);
    for (size_t i = 0; i < length; ++i) cells->styles[at + i] = style;
}

static bool ends_with_space(const Cells *cells)
{
    return cells->text.length && cells->text.data[cells->text.length - 1] == ' ';
}

static uint32_t last_code_point(const Cells *cells)
{
    if (!cells->text.length) return 0;
    size_t at = cells->text.length - 1;
    while (at > 0 && ((unsigned char)cells->text.data[at] & 0xc0) == 0x80) --at;
    size_t used = 0;
    return utf8_decode((const unsigned char *)cells->text.data + at, cells->text.length - at, &used);
}

static bool is_hangul(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11ff) || (cp >= 0x3130 && cp <= 0x318f) ||
           (cp >= 0xa960 && cp <= 0xa97f) || (cp >= 0xac00 && cp <= 0xd7ff);
}

/* CSS Text 3, segment break transformation: a line break in the source
   between two East Asian wide characters is removed, not turned into a
   space. Chinese and Japanese put no spaces between words, and a source
   file wrapped for an editor must not grow them. Hangul is the exception:
   Korean spaces its words. */
static bool break_vanishes(uint32_t before, uint32_t after)
{
    return d_cell_width(before) == 2 && d_cell_width(after) == 2 &&
           !is_hangul(before) && !is_hangul(after);
}

/* Decide the pending space now that the next character, `next`, is known. */
static void resolve_space(Cells *cells, uint32_t next)
{
    if (!cells->pending_space) return;
    bool vanish = cells->pending_segment && break_vanishes(last_code_point(cells), next);
    if (!vanish && !ends_with_space(cells)) inline_raw(cells, " ", 1, cells->pending_style);
    cells->pending_space = false;
    cells->pending_segment = false;
}

/* Append content that is not whitespace from the page: a bracket, a box. */
static void inline_append(Cells *cells, const char *bytes, size_t length, Style style)
{
    resolve_space(cells, 0);
    inline_raw(cells, bytes, length, style);
}

static int frames_width(const Cells *cells)
{
    int width = 0;
    for (int i = 0; i < cells->frame_count; ++i) {
        const Frame *frame = &cells->frames[i];
        const char *prefix = frame->used ? frame->rest : frame->first;
        width += reader_text_cells(prefix, strlen(prefix));
    }
    return width;
}

static int available(const Cells *cells)
{
    int width = cells->layout->measure - frames_width(cells);
    return width < 1 ? 1 : width;
}

/* Put the frames at the start of a row. A content row consumes each frame's
   first-line prefix; a blank row shows only the bars. */
static int write_frames(Cells *cells, int row, bool content)
{
    ReaderLayout *layout = cells->layout;
    int column = layout->left;
    for (int i = 0; i < cells->frame_count; ++i) {
        Frame *frame = &cells->frames[i];
        const char *prefix = frame->used ? frame->rest : frame->first;
        if (content) frame->used = true;
        else if (!frame->bar) { column += reader_text_cells(prefix, strlen(prefix)); continue; }
        column = put_text(layout, grid_row(layout, row), layout->columns, column, prefix, frame->style);
    }
    return column;
}

static void settle_blank(Cells *cells);

static int begin_row(Cells *cells, int *column)
{
    ReaderLayout *layout = cells->layout;
    settle_blank(cells);
    int row = new_row(layout);
    *column = write_frames(cells, row, true);
    cells->last_row_blank = false;
    for (size_t i = 0; i < cells->pending_anchor_count; ++i) {
        layout->anchors = d_xrealloc(layout->anchors, (layout->anchor_count + 1) * sizeof(*layout->anchors));
        layout->anchors[layout->anchor_count++] = (ReaderLayoutAnchor){cells->pending_anchors[i], row};
    }
    free(cells->pending_anchors);
    cells->pending_anchors = NULL;
    cells->pending_anchor_count = 0;
    return row;
}

static void request_blank(Cells *cells)
{
    cells->blank_pending = true;
}

/* Write a pending blank line now, under the frames that are open now. A bar
   that is about to start must not reach back into the gap before its block. */
static void settle_blank(Cells *cells)
{
    ReaderLayout *layout = cells->layout;
    if (cells->blank_pending && layout->rows > 0 && !cells->last_row_blank) {
        int blank = new_row(layout);
        write_frames(cells, blank, false);
        cells->last_row_blank = true;
    }
    cells->blank_pending = false;
}

/* One line of the inline buffer, [begin, end), onto a new row. */
static void emit_line(Cells *cells, const char *text, const Style *styles, size_t begin, size_t end)
{
    ReaderLayout *layout = cells->layout;
    int column = 0;
    int row = begin_row(cells, &column);
    if (cells->rtl) {
        int width = reader_text_cells(text + begin, end - begin);
        int right = layout->left + layout->measure;
        if (right - width > column) column = right - width;
    }
    int limit = layout->left + layout->measure;
    Clusters clusters;
    clusters_init(&clusters, text + begin, end - begin);
    size_t start, stop;
    while (clusters_next(&clusters, &start, &stop)) {
        int width = cluster_width(text + begin + start, stop - start);
        if (!width) continue;
        if (column + width > limit) break;
        column = put_cluster(layout, grid_row(layout, row), layout->columns, column,
                             text + begin + start, stop - start, width, styles[begin + start]);
    }
    clusters_free(&clusters);
}

/* Split [begin, end) into pieces no wider than `width` at cluster boundaries:
   the terminal's overflow-wrap, for a word longer than the line. */
static void emit_hard_wrapped(Cells *cells, const char *text, const Style *styles,
                              size_t begin, size_t end, int width)
{
    Clusters clusters;
    clusters_init(&clusters, text + begin, end - begin);
    size_t start, stop, line = begin;
    int used = 0;
    while (clusters_next(&clusters, &start, &stop)) {
        int cluster = cluster_width(text + begin + start, stop - start);
        if (used + cluster > width && used > 0) {
            emit_line(cells, text, styles, line, begin + start);
            line = begin + start;
            used = 0;
        }
        used += cluster;
    }
    clusters_free(&clusters);
    if (line < end || line == begin) emit_line(cells, text, styles, line, end);
}

static bool is_blank_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void flush_pre(Cells *cells)
{
    const char *text = cells->text.data;
    size_t length = cells->text.length;
    int width = available(cells);
    size_t line = 0;
    while (line <= length) {
        size_t end = line;
        while (end < length && text[end] != '\n') ++end;
        if (end == length && line == length) break;
        /* Tabs become spaces to the next stop of eight: a cell holds a
           character, and a tab is an instruction. */
        DBuffer expanded;
        dbuf_init(&expanded);
        Style *styles = NULL;
        size_t styles_capacity = 0;
        int column = 0;
        for (size_t i = line; i < end; ++i) {
            if (text[i] == '\r') continue;
            int repeat = 1;
            char c = text[i];
            if (c == '\t') { repeat = 8 - (column % 8); c = ' '; }
            for (int r = 0; r < repeat; ++r) {
                d_vec_reserve((void **)&styles, &styles_capacity, expanded.length,
                              sizeof(*styles), expanded.length + 1);
                styles[expanded.length] = cells->styles[i];
                dbuf_append(&expanded, &c, 1);
            }
            column += repeat;
        }
        if (expanded.length) emit_hard_wrapped(cells, expanded.data, styles, 0, expanded.length, width);
        else { int ignored; begin_row(cells, &ignored); }
        dbuf_free(&expanded);
        free(styles);
        line = end + 1;
    }
}

static void flush_inline(Cells *cells)
{
    if (!cells->text.length) { cells->pending_space = false; cells->pending_segment = false; return; }
    if (cells->text_is_pre) {
        flush_pre(cells);
    } else {
        const char *text = cells->text.data;
        size_t length = cells->text.length;
        while (length && is_blank_byte(text[length - 1])) --length;
        int width = available(cells);
        if (length) {
            DTextLine *lines = d_xmalloc((length + 1) * sizeof(*lines));
            size_t count = d_text_wrap_utf8(text, length, width, measure_cells, NULL,
                                            lines, length + 1);
            for (size_t i = 0; i < count; ++i) {
                size_t begin = lines[i].begin, end = lines[i].end;
                while (end > begin && is_blank_byte(text[end - 1])) --end;
                while (begin < end && is_blank_byte(text[begin])) ++begin;
                if (begin == end) continue;
                if (reader_text_cells(text + begin, end - begin) > width)
                    emit_hard_wrapped(cells, text, cells->styles, begin, end, width);
                else
                    emit_line(cells, text, cells->styles, begin, end);
            }
            free(lines);
        }
    }
    cells->text.length = 0;
    if (cells->text.data) cells->text.data[0] = '\0';
    cells->pending_space = false;
    cells->pending_segment = false;
    cells->text_is_pre = false;
}

static void push_frame(Cells *cells, const char *first, const char *rest, Style style, bool bar)
{
    if (cells->frame_count >= (int)(sizeof(cells->frames) / sizeof(cells->frames[0]))) return;
    cells->frames[cells->frame_count++] = (Frame){d_strdup(first), d_strdup(rest), style, bar, false};
}

static void pop_frame(Cells *cells)
{
    if (!cells->frame_count) return;
    Frame *frame = &cells->frames[--cells->frame_count];
    free(frame->first);
    free(frame->rest);
}

/* ---- sink callbacks ------------------------------------------------------ */

static void on_page(void *opaque, const ReaderPage *page)
{
    Cells *cells = opaque;
    ReaderLayout *layout = cells->layout;
    layout->title = d_strdup(page->title);
    layout->dir = d_strdup(page->dir);
    layout->lang = d_strdup(page->lang);
    cells->rtl = d_ieq(page->dir, "rtl");
}

static bool class_is_muted(const char *class_name)
{
    return d_contains(class_name, "eyebrow") || d_contains(class_name, "media-meta") ||
           d_contains(class_name, "media-url");
}

static void on_block_open(void *opaque, const ReaderBlock *block)
{
    Cells *cells = opaque;
    flush_inline(cells);
    Open open = {.kind = block->kind};
    Style border = plain;
    border.role = READER_ROLE_BORDER;
    switch (block->kind) {
    case READER_BLOCK_GENERIC:
        request_blank(cells);
        break;
    case READER_BLOCK_CARD:
        request_blank(cells);
        settle_blank(cells);
        push_frame(cells, "\xe2\x96\x8e ", "\xe2\x96\x8e ", border, true);   /* ▎ */
        open.frame = true;
        break;
    case READER_BLOCK_HEADING:
        request_blank(cells);
        cells->heading = block->level;
        break;
    case READER_BLOCK_LIST:
        /* A list inside a list item is part of that item: spacing it from
           its parent spreads a short outline over twice the rows. */
        if (!cells->list_depth) request_blank(cells);
        if (cells->list_depth < 16) {
            cells->ordered[cells->list_depth] = block->ordered;
            cells->counter[cells->list_depth] = 0;
        }
        ++cells->list_depth;
        break;
    case READER_BLOCK_ITEM: {
        int depth = cells->list_depth > 0 ? cells->list_depth - 1 : 0;
        char marker[32];
        if (depth < 16 && cells->ordered[depth])
            snprintf(marker, sizeof(marker), "%zu. ", ++cells->counter[depth]);
        else
            snprintf(marker, sizeof(marker), "%s", depth % 2 ? "\xe2\x97\xa6 " : "\xe2\x80\xa2 ");  /* ◦ • */
        int width = reader_text_cells(marker, strlen(marker));
        char rest[32];
        memset(rest, ' ', (size_t)width);
        rest[width] = '\0';
        push_frame(cells, marker, rest, plain, false);
        open.frame = true;
        break;
    }
    case READER_BLOCK_QUOTE:
        request_blank(cells);
        settle_blank(cells);
        push_frame(cells, "\xe2\x94\x82 ", "\xe2\x94\x82 ", border, true);   /* │ */
        open.frame = true;
        open.muted = true;
        break;
    case READER_BLOCK_PRE:
        request_blank(cells);
        push_frame(cells, "  ", "  ", plain, false);
        open.frame = true;
        ++cells->pre;
        break;
    }
    if (class_is_muted(block->class_name)) open.muted = true;
    if (open.muted) ++cells->muted;
    if (cells->open_count < (int)(sizeof(cells->open) / sizeof(cells->open[0])))
        cells->open[cells->open_count++] = open;
}

static void on_block_close(void *opaque, const ReaderBlock *block)
{
    Cells *cells = opaque;
    flush_inline(cells);
    if (!cells->open_count) return;
    Open open = cells->open[--cells->open_count];
    if (open.frame) pop_frame(cells);
    if (open.muted && cells->muted) --cells->muted;
    switch (block->kind) {
    case READER_BLOCK_HEADING:
        cells->heading = 0;
        request_blank(cells);
        break;
    case READER_BLOCK_LIST:
        if (cells->list_depth) --cells->list_depth;
        if (!cells->list_depth) request_blank(cells);
        break;
    case READER_BLOCK_ITEM:
        break;
    case READER_BLOCK_PRE:
        if (cells->pre) --cells->pre;
        request_blank(cells);
        break;
    default:
        request_blank(cells);
        break;
    }
}

static void on_text(void *opaque, const char *data, size_t length)
{
    Cells *cells = opaque;
    if (cells->in_table) return;
    Style style = current_style(cells);
    if (cells->pre) {
        if (!cells->text_is_pre && cells->text.length) flush_inline(cells);
        cells->text_is_pre = true;
        inline_raw(cells, data, length, style);
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        if (is_blank_byte(data[i])) {
            if (!cells->text.length || ends_with_space(cells)) continue;
            if (!cells->pending_space) cells->pending_style = style;
            cells->pending_space = true;
            if (data[i] == '\n' || data[i] == '\r') cells->pending_segment = true;
            continue;
        }
        size_t run = i;
        while (run < length && !is_blank_byte(data[run])) ++run;
        size_t used = 0;
        resolve_space(cells, utf8_decode((const unsigned char *)data + i, run - i, &used));
        inline_raw(cells, data + i, run - i, style);
        i = run - 1;
    }
}

static void on_style_open(void *opaque, unsigned style)
{
    Cells *cells = opaque;
    if (style == READER_STYLE_STRONG) ++cells->strong;
    else if (style == READER_STYLE_EM) ++cells->em;
    else if (style == READER_STYLE_CODE) ++cells->code;
}

static void on_style_close(void *opaque, unsigned style)
{
    Cells *cells = opaque;
    if (style == READER_STYLE_STRONG && cells->strong) --cells->strong;
    else if (style == READER_STYLE_EM && cells->em) --cells->em;
    else if (style == READER_STYLE_CODE && cells->code) --cells->code;
}

static void on_link_open(void *opaque, const ReaderLink *link)
{
    Cells *cells = opaque;
    ReaderLayout *layout = cells->layout;
    layout->links = d_xrealloc(layout->links, (layout->link_count + 1) * sizeof(*layout->links));
    layout->links[layout->link_count] = (ReaderLayoutLink){d_strdup(link->href), link->button, -1, -1};
    int32_t index = (int32_t)layout->link_count++;
    if (cells->link_depth < 16) {
        cells->link_stack[cells->link_depth] = index;
        cells->button_stack[cells->link_depth] = link->button;
        ++cells->link_depth;
    }
    /* An action set against the words before it with no space between:
       the desktop's CSS spaces them, and cells must say so themselves. */
    if (link->button && cells->text.length && !cells->pending_space && !ends_with_space(cells))
        inline_raw(cells, " ", 1, plain);
    if (link->button) inline_append(cells, "[ ", 2, current_style(cells));
}

static void on_link_close(void *opaque, const ReaderLink *link)
{
    Cells *cells = opaque;
    if (link->button) {
        /* Whitespace at the end of the label becomes the bracket's own. */
        cells->pending_space = false;
        cells->pending_segment = false;
        inline_raw(cells, ends_with_space(cells) ? "]" : " ]",
                   ends_with_space(cells) ? 1 : 2, current_style(cells));
    }
    if (cells->link_depth) --cells->link_depth;
    if (link->button) inline_raw(cells, " ", 1, current_style(cells));
}

static void on_line_break(void *opaque)
{
    Cells *cells = opaque;
    if (cells->in_table) return;
    if (cells->text.length) flush_inline(cells);
    else { int column; begin_row(cells, &column); }
}

static void on_rule(void *opaque)
{
    Cells *cells = opaque;
    flush_inline(cells);
    request_blank(cells);
    int column = 0;
    int row = begin_row(cells, &column);
    Style rule = plain;
    rule.role = READER_ROLE_RULE;
    ReaderLayout *layout = cells->layout;
    int end = layout->left + layout->measure;
    while (column < end)
        column = put_cluster(layout, grid_row(layout, row), layout->columns, column,
                             "\xe2\x94\x80", 3, 1, rule);   /* ─ */
    request_blank(cells);
}

static void on_image(void *opaque, const ReaderImage *image)
{
    Cells *cells = opaque;
    if (cells->in_table) return;
    flush_inline(cells);
    ReaderLayout *layout = cells->layout;
    Style style = current_style(cells);
    if (style.role == READER_ROLE_TEXT) style.role = READER_ROLE_MUTED;
    int column = 0;
    int row = begin_row(cells, &column);
    char *label = *image->alt ? d_cat3("[image: ", image->alt, "]") : d_strdup("[image]");
    int start = column;
    int limit = layout->left + layout->measure;
    ReaderCell *line = grid_row(layout, row);
    /* A label wider than the column is cut rather than wrapped: it is a
       placeholder, and the image it stands for is one thing. */
    Clusters clusters;
    size_t length = strlen(label), begin, end;
    clusters_init(&clusters, label, length);
    while (clusters_next(&clusters, &begin, &end)) {
        int width = cluster_width(label + begin, end - begin);
        if (column + width > limit) break;
        column = put_cluster(layout, line, layout->columns, column, label + begin, end - begin, width, style);
    }
    clusters_free(&clusters);
    free(label);
    int rows = 1;
    for (int extra = 1; extra < cells->options.image_rows; ++extra) {
        int blank = new_row(layout);
        write_frames(cells, blank, false);
        ++rows;
    }
    layout->images = d_xrealloc(layout->images, (layout->image_count + 1) * sizeof(*layout->images));
    layout->images[layout->image_count++] = (ReaderLayoutImage){
        d_strdup(image->src), d_strdup(image->alt), style.link, row, start,
        limit - start, rows};
}

static bool control_is_button(const ReaderLayoutControl *control)
{
    return d_eq(control->tag, "button") || d_ieq(control->type, "submit") ||
           d_ieq(control->type, "reset") || d_ieq(control->type, "button") ||
           d_ieq(control->type, "image");
}

static bool control_is_choice(const ReaderLayoutControl *control)
{
    return d_ieq(control->type, "checkbox") || d_ieq(control->type, "radio");
}

static void on_control(void *opaque, const ReaderControl *source)
{
    Cells *cells = opaque;
    ReaderLayout *layout = cells->layout;
    layout->controls = d_xrealloc(layout->controls, (layout->control_count + 1) * sizeof(*layout->controls));
    ReaderLayoutControl *control = &layout->controls[layout->control_count];
    *control = (ReaderLayoutControl){
        .in_form = source->in_form, .form_index = source->form_index,
        .action = d_strdup(source->action), .method = d_strdup(source->method),
        .tag = d_strdup(source->tag), .type = d_strdup(source->type),
        .id = d_strdup(source->id), .name = d_strdup(source->name),
        .value = d_strdup(source->value), .placeholder = d_strdup(source->placeholder),
        .caption = d_strdup(source->caption), .options = d_strdup(source->options),
        .checked = source->checked, .disabled = source->disabled, .required = source->required,
        .row = -1, .column = -1, .width = 0,
    };
    int32_t index = (int32_t)layout->control_count++;
    if (cells->in_table || d_ieq(control->type, "hidden")) return;

    Style style = current_style(cells);
    style.control = index;
    style.link = -1;
    if (control_is_choice(control)) {
        style.role = READER_ROLE_FIELD;
        const char *box = d_ieq(control->type, "radio") ? "( )" : "[ ]";
        inline_append(cells, box, 3, style);
        inline_raw(cells, " ", 1, current_style(cells));
        return;
    }
    if (control_is_button(control)) {
        style.role = READER_ROLE_BUTTON;
        const char *label = *control->caption ? control->caption
                          : *control->value ? control->value : control->type;
        char *text = d_cat3("[ ", label, " ]");
        inline_append(cells, text, strlen(text), style);
        free(text);
        inline_raw(cells, " ", 1, current_style(cells));
        return;
    }
    /* A field is a line of its own, the way the reader lays it out. */
    flush_inline(cells);
    int column = 0;
    int row = begin_row(cells, &column);
    int width = available(cells);
    if (!d_eq(control->tag, "textarea") && width > 40) width = 40;
    control->row = row;
    control->column = column;
    control->width = width;
    reader_layout_paint_control(layout, (size_t)index, control->value, control->checked);
}

static void on_table_open(void *opaque)
{
    Cells *cells = opaque;
    flush_inline(cells);
    request_blank(cells);
    cells->in_table = true;
}

static void on_table_row(void *opaque, const char *const *texts, const bool *header, size_t count)
{
    Cells *cells = opaque;
    cells->table_rows = d_xrealloc(cells->table_rows, (cells->table_row_count + 1) * sizeof(*cells->table_rows));
    TableRow *row = &cells->table_rows[cells->table_row_count++];
    row->count = count;
    row->cells = d_xmalloc((count ? count : 1) * sizeof(*row->cells));
    row->header = d_xmalloc((count ? count : 1) * sizeof(*row->header));
    for (size_t i = 0; i < count; ++i) {
        row->cells[i] = d_strdup(texts[i]);
        row->header[i] = header[i];
    }
}

#define TABLE_COLUMN_CAP 40

/* Cells wrap inside their column; a column is as wide as its widest cell up
   to a cap. What does not fit the measure gets a horizontal window, not a
   reflow: squeezing columns until they fit turns a table into a word list. */
static void on_table_close(void *opaque)
{
    Cells *cells = opaque;
    ReaderLayout *layout = cells->layout;
    cells->in_table = false;
    size_t columns = 0;
    for (size_t r = 0; r < cells->table_row_count; ++r)
        if (cells->table_rows[r].count > columns) columns = cells->table_rows[r].count;
    if (!columns) goto done;

    int *widths = d_xcalloc(columns, sizeof(*widths));
    for (size_t r = 0; r < cells->table_row_count; ++r)
        for (size_t c = 0; c < cells->table_rows[r].count; ++c) {
            const char *text = cells->table_rows[r].cells[c];
            int width = reader_text_cells(text, strlen(text));
            if (width > TABLE_COLUMN_CAP) width = TABLE_COLUMN_CAP;
            if (width > widths[c]) widths[c] = width;
        }
    int full = 0;
    for (size_t c = 0; c < columns; ++c) {
        if (widths[c] < 1) widths[c] = 1;
        full += widths[c] + (c ? 3 : 0);
    }

    /* Lay every row out in the table's own grid first. */
    ReaderCell *grid = NULL;
    int grid_rows = 0;
    Style border = plain;
    border.role = READER_ROLE_BORDER;
    for (size_t r = 0; r < cells->table_row_count; ++r) {
        const TableRow *row = &cells->table_rows[r];
        DTextLine *lines[64] = {0};
        size_t line_counts[64] = {0};
        size_t height = 1;
        size_t used_columns = columns < 64 ? columns : 64;
        for (size_t c = 0; c < row->count && c < used_columns; ++c) {
            const char *text = row->cells[c];
            size_t length = strlen(text);
            if (!length) continue;
            lines[c] = d_xmalloc((length + 1) * sizeof(DTextLine));
            line_counts[c] = d_text_wrap_utf8(text, length, widths[c], measure_cells, NULL,
                                              lines[c], length + 1);
            if (line_counts[c] > height) height = line_counts[c];
        }
        grid = d_xrealloc(grid, (size_t)(grid_rows + (int)height) * (size_t)full * sizeof(*grid));
        fill_blank(grid + (size_t)grid_rows * (size_t)full, height * (size_t)full);
        for (size_t line = 0; line < height; ++line) {
            ReaderCell *out = grid + (size_t)(grid_rows + (int)line) * (size_t)full;
            int x = 0;
            for (size_t c = 0; c < used_columns; ++c) {
                if (c) x = put_text(layout, out, full, x, " \xe2\x94\x82 ", border);   /* │ */
                Style style = plain;
                if (c < row->count && row->header[c]) style.attrs |= READER_CELL_BOLD;
                if (c < row->count && lines[c] && line < line_counts[c]) {
                    const char *text = row->cells[c];
                    size_t begin = lines[c][line].begin, end = lines[c][line].end;
                    while (end > begin && is_blank_byte(text[end - 1])) --end;
                    int cx = x;
                    Clusters clusters;
                    clusters_init(&clusters, text + begin, end - begin);
                    size_t start, stop;
                    while (clusters_next(&clusters, &start, &stop)) {
                        int width = cluster_width(text + begin + start, stop - start);
                        if (!width) continue;
                        if (cx + width > x + widths[c]) break;
                        cx = put_cluster(layout, out, full, cx, text + begin + start,
                                         stop - start, width, style);
                    }
                    clusters_free(&clusters);
                }
                x += widths[c];
            }
        }
        grid_rows += (int)height;
        for (size_t c = 0; c < used_columns; ++c) free(lines[c]);

        /* A rule under a header row. */
        bool header_row = false;
        for (size_t c = 0; c < row->count; ++c) header_row |= row->header[c];
        if (r == 0 && header_row) {
            grid = d_xrealloc(grid, (size_t)(grid_rows + 1) * (size_t)full * sizeof(*grid));
            ReaderCell *out = grid + (size_t)grid_rows * (size_t)full;
            fill_blank(out, (size_t)full);
            int x = 0;
            for (size_t c = 0; c < columns; ++c) {
                if (c) x = put_text(layout, out, full, x, "\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80", border);  /* ─┼─ */
                for (int i = 0; i < widths[c]; ++i)
                    x = put_cluster(layout, out, full, x, "\xe2\x94\x80", 3, 1, border);
            }
            ++grid_rows;
        }
    }
    free(widths);

    /* Then show it in the page: whole, or through a window with its own
       horizontal scroll. */
    int visible = available(cells);
    bool overflow = full > visible;
    if (overflow && visible > 1) --visible;   /* room for the scroll mark */
    int first_row = -1, column = 0;
    for (int r = 0; r < grid_rows; ++r) {
        int row = begin_row(cells, &column);
        if (first_row < 0) first_row = row;
    }
    layout->tables = d_xrealloc(layout->tables, (layout->table_count + 1) * sizeof(*layout->tables));
    layout->tables[layout->table_count] = (ReaderLayoutTable){
        .row = first_row, .rows = grid_rows, .column = column,
        .visible = overflow ? visible : full, .full_width = full, .offset = 0, .cells = grid};
    ++layout->table_count;
    reader_layout_scroll_table(layout, layout->table_count - 1, 0);
    request_blank(cells);

done:
    for (size_t r = 0; r < cells->table_row_count; ++r) {
        for (size_t c = 0; c < cells->table_rows[r].count; ++c) free(cells->table_rows[r].cells[c]);
        free(cells->table_rows[r].cells);
        free(cells->table_rows[r].header);
    }
    free(cells->table_rows);
    cells->table_rows = NULL;
    cells->table_row_count = 0;
}

static void on_anchor(void *opaque, const char *id)
{
    Cells *cells = opaque;
    cells->pending_anchors = d_xrealloc(cells->pending_anchors,
                                        (cells->pending_anchor_count + 1) * sizeof(*cells->pending_anchors));
    cells->pending_anchors[cells->pending_anchor_count++] = d_strdup(id);
}

/* ---- public -------------------------------------------------------------- */

void reader_layout_scroll_table(ReaderLayout *layout, size_t index, int offset)
{
    if (!layout || index >= layout->table_count) return;
    ReaderLayoutTable *table = &layout->tables[index];
    int most = table->full_width - table->visible;
    if (offset > most) offset = most;
    if (offset < 0) offset = 0;
    table->offset = offset;
    bool overflow = table->full_width > table->visible;
    for (int r = 0; r < table->rows; ++r) {
        ReaderCell *out = grid_row(layout, table->row + r) + table->column;
        const ReaderCell *in = table->cells + (size_t)r * (size_t)table->full_width + (size_t)offset;
        for (int x = 0; x < table->visible && table->column + x < layout->columns; ++x) {
            out[x] = in[x];
            /* A wide character cut by either edge of the window. */
            if ((x == 0 && in[x].width == 0) ||
                (in[x].width > 1 && x + in[x].width > table->visible))
                out[x] = blank_cell(plain);
        }
        if (overflow && table->column + table->visible < layout->columns) {
            Style mark = plain;
            mark.role = READER_ROLE_MUTED;
            ReaderCell *edge = &out[table->visible];
            *edge = blank_cell(plain);
            const char *glyph = offset + table->visible < table->full_width ? "\xe2\x80\xba"  /* › */
                              : offset > 0 ? "\xe2\x80\xb9" : NULL;                        /* ‹ */
            if (glyph) put_cluster(layout, out, layout->columns - table->column, table->visible,
                                   glyph, 3, 1, mark);
        }
    }
}

static const char *select_label(const char *options, const char *value, char **owned)
{
    *owned = NULL;
    const char *record = options;
    while (record && *record) {
        const char *unit = strchr(record, '\x1f');
        const char *end = strchr(record, '\x1e');
        if (!unit || !end || unit > end) break;
        const char *label = unit + 1;
        const char *label_end = strchr(label, '\x1f');
        if (!label_end || label_end > end) label_end = end;
        if ((size_t)(unit - record) == strlen(value) && !strncmp(record, value, (size_t)(unit - record))) {
            *owned = d_strndup(label, (size_t)(label_end - label));
            return *owned;
        }
        record = end + 1;
    }
    return value;
}

void reader_layout_paint_control(ReaderLayout *layout, size_t index, const char *value, bool checked)
{
    if (!layout || index >= layout->control_count) return;
    ReaderLayoutControl *control = &layout->controls[index];
    if (control->row < 0 || control->width <= 0) return;
    ReaderCell *row = grid_row(layout, control->row);
    Style style = plain;
    style.role = READER_ROLE_FIELD;
    style.control = (int32_t)index;
    if (control_is_choice(control)) {
        const char *mark = checked ? (d_ieq(control->type, "radio") ? "\xe2\x80\xa2" : "x") : " ";
        put_text(layout, row, layout->columns, control->column + 1, mark, style);
        return;
    }
    if (control_is_button(control)) return;

    int end = control->column + control->width;
    if (end > layout->columns) end = layout->columns;
    style.attrs = READER_CELL_UNDERLINE;
    for (int x = control->column; x < end; ++x) row[x] = blank_cell(style);

    bool select = d_eq(control->tag, "select");
    char *owned = NULL;
    const char *shown = value ? value : "";
    Style text = style;
    if (select) {
        shown = select_label(control->options, shown, &owned);
    } else if (!*shown && *control->placeholder) {
        shown = control->placeholder;
        text.attrs |= READER_CELL_ITALIC;
    }
    DBuffer display;
    dbuf_init(&display);
    if (d_ieq(control->type, "password") && shown == value) {
        for (int i = reader_text_cells(shown, strlen(shown)); i > 0; --i) dbuf_puts(&display, "\xe2\x80\xa2");
    } else {
        /* One line of it: a textarea's first line stands for the rest. */
        const char *newline = strchr(shown, '\n');
        dbuf_append(&display, shown, newline ? (size_t)(newline - shown) : strlen(shown));
    }
    int limit = end - (select ? 2 : 0);
    int x = control->column + 1;
    Clusters clusters;
    const char *data = display.data ? display.data : "";
    clusters_init(&clusters, data, display.length);
    size_t start, stop;
    while (clusters_next(&clusters, &start, &stop)) {
        int width = cluster_width(data + start, stop - start);
        if (!width) continue;
        if (x + width > limit) break;
        x = put_cluster(layout, row, end, x, data + start, stop - start, width, text);
    }
    clusters_free(&clusters);
    if (select && end - 1 > control->column) put_text(layout, row, end, end - 1, "\xe2\x96\xbe", style);  /* ▾ */
    dbuf_free(&display);
    free(owned);
}

/* Where each link and inline control ended up, read back off the grid: the
   first cell that carries it. A field placed itself when it was laid out. */
static void record_positions(ReaderLayout *layout)
{
    bool *inline_control = d_xcalloc(layout->control_count ? layout->control_count : 1, sizeof(bool));
    for (size_t i = 0; i < layout->control_count; ++i)
        inline_control[i] = layout->controls[i].row < 0;
    for (int r = 0; r < layout->rows; ++r) {
        ReaderCell *row = grid_row(layout, r);
        for (int c = 0; c < layout->columns; ++c) {
            ReaderCell *cell = &row[c];
            if (cell->link >= 0 && (size_t)cell->link < layout->link_count &&
                layout->links[cell->link].row < 0) {
                layout->links[cell->link].row = r;
                layout->links[cell->link].column = c;
            }
            if (cell->control >= 0 && (size_t)cell->control < layout->control_count &&
                inline_control[cell->control]) {
                ReaderLayoutControl *control = &layout->controls[cell->control];
                if (control->row < 0) { control->row = r; control->column = c; }
                if (control->row == r) ++control->width;
            }
        }
    }
    free(inline_control);
}

ReaderLayout *reader_layout(const char *html, const ReaderLayoutOptions *options)
{
    if (!html || !options || options->columns < 8) return NULL;
    ReaderLayout *layout = d_xcalloc(1, sizeof(*layout));
    layout->columns = options->columns;
    int measure = options->measure > 0 ? options->measure : options->columns - 2;
    if (!options->measure && measure > 80) measure = 80;
    if (measure > options->columns) measure = options->columns;
    if (measure < 1) measure = 1;
    layout->measure = measure;
    layout->left = (options->columns - measure) / 2;

    Cells cells = {.layout = layout, .options = *options};
    dbuf_init(&cells.text);
    ReaderSink sink = {
        .opaque = &cells,
        .page = on_page,
        .block_open = on_block_open,
        .block_close = on_block_close,
        .text = on_text,
        .style_open = on_style_open,
        .style_close = on_style_close,
        .link_open = on_link_open,
        .link_close = on_link_close,
        .line_break = on_line_break,
        .rule = on_rule,
        .image = on_image,
        .control = on_control,
        .table_open = on_table_open,
        .table_row = on_table_row,
        .table_close = on_table_close,
        .anchor = on_anchor,
    };
    bool ok = reader_walk(html, &sink);
    flush_inline(&cells);
    /* An anchor at the very end of the page still has somewhere to land. */
    if (cells.pending_anchor_count) { int column; begin_row(&cells, &column); }
    while (cells.frame_count) pop_frame(&cells);
    dbuf_free(&cells.text);
    free(cells.styles);
    if (!layout->title) layout->title = d_strdup("");
    if (!layout->dir) layout->dir = d_strdup("");
    if (!layout->lang) layout->lang = d_strdup("");
    if (!ok) { reader_layout_free(layout); return NULL; }
    record_positions(layout);
    for (size_t i = 0; i < layout->control_count; ++i)
        if (control_is_choice(&layout->controls[i]) && layout->controls[i].checked)
            reader_layout_paint_control(layout, i, layout->controls[i].value, true);
    return layout;
}

void reader_layout_free(ReaderLayout *layout)
{
    if (!layout) return;
    free(layout->cells);
    free(layout->pool);
    free(layout->title); free(layout->dir); free(layout->lang);
    for (size_t i = 0; i < layout->link_count; ++i) free(layout->links[i].href);
    free(layout->links);
    for (size_t i = 0; i < layout->control_count; ++i) {
        ReaderLayoutControl *c = &layout->controls[i];
        free(c->action); free(c->method); free(c->tag); free(c->type); free(c->id);
        free(c->name); free(c->value); free(c->placeholder); free(c->caption); free(c->options);
    }
    free(layout->controls);
    for (size_t i = 0; i < layout->anchor_count; ++i) free(layout->anchors[i].id);
    free(layout->anchors);
    for (size_t i = 0; i < layout->image_count; ++i) { free(layout->images[i].src); free(layout->images[i].alt); }
    free(layout->images);
    for (size_t i = 0; i < layout->table_count; ++i) free(layout->tables[i].cells);
    free(layout->tables);
    free(layout);
}

const char *reader_cell_text(const ReaderLayout *layout, const ReaderCell *cell, size_t *length)
{
    if (!cell->length) { if (length) *length = 0; return ""; }
    if (length) *length = cell->length;
    return layout->pool + cell->text;
}

char *reader_layout_dump(const ReaderLayout *layout, int first, int count)
{
    DBuffer out;
    dbuf_init(&out);
    if (first < 0) first = 0;
    int last = count > 0 ? first + count : layout->rows;
    if (last > layout->rows) last = layout->rows;
    for (int r = first; r < last; ++r) {
        size_t line_start = out.length;
        const ReaderCell *row = layout->cells + (size_t)r * (size_t)layout->columns;
        for (int c = 0; c < layout->columns; ++c) {
            const ReaderCell *cell = &row[c];
            if (!cell->width) continue;
            if (!cell->length) dbuf_puts(&out, " ");
            else dbuf_append(&out, layout->pool + cell->text, cell->length);
        }
        while (out.length > line_start && out.data[out.length - 1] == ' ') --out.length;
        dbuf_puts(&out, "\n");
    }
    if (!out.data) dbuf_puts(&out, "");
    return dbuf_take(&out);
}
