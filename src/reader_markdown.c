/* Markdown export: the reader walk, written as a document.

   The conversion reads the reader page rather than the extraction JSON, so
   what comes out is what the reader shows: the same shape decision, the same
   cards, the same order. A template change carries into the export without
   anything here being told about it, which is the point -- an export that
   drifts from the page it claims to be is worse than no export.

   Internal destinations do not survive: distill-play: and its siblings are
   things for the shell to do, meaningless in a file. The media URL is written
   as an ordinary link instead, which is what a reader of the exported
   document can actually use. */

#include "reader_walk.h"

#include "cutil.h"

typedef struct {
    DBuffer out;
    int list_depth;
    bool ordered[8];
    size_t counter[8];
    bool at_line_start;
    bool in_pre;
    /* A code span inside a code span restores what it found; pre does not
       nest in a way that needs one. */
    bool *saved_pre;
    size_t saved_length;
    size_t saved_capacity;
    bool table_header_done;
} Markdown;

static void md_text(Markdown *md, const char *text)
{
    if (!text || !*text) return;
    dbuf_puts(&md->out, text);
    md->at_line_start = false;
}

static void md_newline(Markdown *md)
{
    if (!md->at_line_start) { dbuf_puts(&md->out, "\n"); md->at_line_start = true; }
}

static void md_blank(Markdown *md)
{
    md_newline(md);
    size_t length = md->out.length;
    const char *data = md->out.data;
    if (length >= 2 && data[length - 1] == '\n' && data[length - 2] == '\n') return;
    if (length == 0) return;
    dbuf_puts(&md->out, "\n");
}

/* Collapse runs of whitespace the way the reader lays text out, and keep
   Markdown's own punctuation from being read as markup. */
static void md_inline_text(Markdown *md, const char *text)
{
    bool space = md->at_line_start;
    for (const char *at = text; *at; ++at) {
        unsigned char ch = (unsigned char)*at;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!space) { dbuf_puts(&md->out, " "); space = true; md->at_line_start = false; }
            continue;
        }
        space = false;
        if (!md->in_pre && strchr("\\`*_[]<>", ch)) dbuf_printf(&md->out, "\\%c", ch);
        else dbuf_printf(&md->out, "%c", ch);
        md->at_line_start = false;
    }
}

/* An internal action carries the media URL after its scheme; that is the part
   worth keeping. A section jump or a close has nothing behind it. */
static const char *md_link_target(const char *href)
{
    if (!d_starts(href, "distill-")) return href;
    const char *schemes[] = {"distill-play:", "distill-download:", "distill-open:", "distill-media:"};
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); ++i)
        if (d_starts(href, schemes[i])) return href + strlen(schemes[i]);
    return NULL;
}

static void md_list_item(Markdown *md)
{
    int depth = md->list_depth > 0 ? md->list_depth - 1 : 0;
    for (int i = 0; i < depth; ++i) dbuf_puts(&md->out, "  ");
    if (depth < 8 && md->ordered[depth])
        dbuf_printf(&md->out, "%zu. ", ++md->counter[depth]);
    else
        dbuf_puts(&md->out, "- ");
    md->at_line_start = false;
}

static void on_block_open(void *opaque, const ReaderBlock *block)
{
    Markdown *md = opaque;
    switch (block->kind) {
    case READER_BLOCK_HEADING:
        md_blank(md);
        for (int i = 0; i < block->level; ++i) dbuf_puts(&md->out, "#");
        dbuf_puts(&md->out, " ");
        md->at_line_start = false;
        break;
    case READER_BLOCK_GENERIC:
    case READER_BLOCK_CARD:
        md_blank(md);
        break;
    case READER_BLOCK_LIST:
        md_blank(md);
        if (md->list_depth < 8) {
            md->ordered[md->list_depth] = block->ordered;
            md->counter[md->list_depth] = 0;
        }
        ++md->list_depth;
        break;
    case READER_BLOCK_ITEM:
        md_newline(md);
        md_list_item(md);
        break;
    case READER_BLOCK_QUOTE:
        md_blank(md);
        dbuf_puts(&md->out, "> ");
        md->at_line_start = false;
        break;
    case READER_BLOCK_PRE:
        md_blank(md);
        dbuf_puts(&md->out, "```\n");
        md->at_line_start = true;
        md->in_pre = true;
        break;
    }
}

static void on_block_close(void *opaque, const ReaderBlock *block)
{
    Markdown *md = opaque;
    switch (block->kind) {
    case READER_BLOCK_HEADING:
    case READER_BLOCK_GENERIC:
    case READER_BLOCK_CARD:
    case READER_BLOCK_QUOTE:
        md_blank(md);
        break;
    case READER_BLOCK_LIST:
        --md->list_depth;
        md_blank(md);
        break;
    case READER_BLOCK_ITEM:
        md_newline(md);
        break;
    case READER_BLOCK_PRE:
        md->in_pre = false;
        md_newline(md);
        dbuf_puts(&md->out, "```");
        md->at_line_start = false;
        md_blank(md);
        break;
    }
}

static void on_text(void *opaque, const char *data, size_t length)
{
    Markdown *md = opaque;
    char *text = d_strndup(data, length);
    if (md->in_pre) md_text(md, text);
    else md_inline_text(md, text);
    free(text);
}

static void on_style_open(void *opaque, unsigned style)
{
    Markdown *md = opaque;
    if (style == READER_STYLE_CODE) {
        dbuf_puts(&md->out, "`");
        d_vec_reserve((void **)&md->saved_pre, &md->saved_capacity, md->saved_length,
                      sizeof(*md->saved_pre), md->saved_length + 1);
        md->saved_pre[md->saved_length++] = md->in_pre;
        md->in_pre = true;
    } else {
        dbuf_puts(&md->out, style == READER_STYLE_STRONG ? "**" : "*");
    }
}

static void on_style_close(void *opaque, unsigned style)
{
    Markdown *md = opaque;
    if (style == READER_STYLE_CODE) {
        md->in_pre = md->saved_length ? md->saved_pre[--md->saved_length] : false;
        dbuf_puts(&md->out, "`");
    } else {
        dbuf_puts(&md->out, style == READER_STYLE_STRONG ? "**" : "*");
    }
    md->at_line_start = false;
}

static void on_link_open(void *opaque, const ReaderLink *link)
{
    Markdown *md = opaque;
    const char *target = md_link_target(link->href);
    /* A destination only the shell understands keeps its words and loses the
       link, because nothing outside Distill could follow it. */
    if (target && *target) dbuf_puts(&md->out, "[");
}

static void on_link_close(void *opaque, const ReaderLink *link)
{
    Markdown *md = opaque;
    const char *target = md_link_target(link->href);
    if (target && *target) dbuf_printf(&md->out, "](%s)", target);
    /* Actions sit side by side on the card; in a document they would run
       into one another without a gap. */
    if (link->button) dbuf_puts(&md->out, " ");
    md->at_line_start = false;
}

static void on_line_break(void *opaque)
{
    Markdown *md = opaque;
    dbuf_puts(&md->out, "  ");
    md_newline(md);
}

static void on_rule(void *opaque)
{
    Markdown *md = opaque;
    md_blank(md);
    dbuf_puts(&md->out, "---");
    md->at_line_start = false;
    md_blank(md);
}

static void on_image(void *opaque, const ReaderImage *image)
{
    Markdown *md = opaque;
    if (*image->src) dbuf_printf(&md->out, "![%s](%s)", image->alt, image->src);
    md->at_line_start = false;
}

/* A control cannot be exported as a control. Name it, so the document still
   says the page asked for something here. */
static void on_control(void *opaque, const ReaderControl *control)
{
    Markdown *md = opaque;
    if (d_ieq(control->type_attr, "hidden")) return;
    /* What the control declares, not the text a textarea holds or the option
       a select settled on. */
    const char *label = *control->placeholder ? control->placeholder
                      : *control->value_attr ? control->value_attr : control->name;
    md_newline(md);
    dbuf_printf(&md->out, "`[%s%s%s]`", *control->type_attr ? control->type_attr : control->tag,
                *label ? ": " : "", label);
    md->at_line_start = false;
    md_newline(md);
}

static void on_table_open(void *opaque)
{
    Markdown *md = opaque;
    md->table_header_done = false;
}

static void on_table_row(void *opaque, const char *const *cells, const bool *header, size_t count)
{
    Markdown *md = opaque;
    (void)header;
    md_newline(md);
    dbuf_puts(&md->out, "|");
    for (size_t i = 0; i < count; ++i) {
        char *text = d_strdup(cells[i]);
        /* A pipe inside a cell would end the column early. */
        for (char *at = text; *at; ++at) if (*at == '|') *at = '/';
        dbuf_printf(&md->out, " %s |", text);
        free(text);
    }
    md->at_line_start = false;
    md_newline(md);
    if (!md->table_header_done && count) {
        dbuf_puts(&md->out, "|");
        for (size_t c = 0; c < count; ++c) dbuf_puts(&md->out, " --- |");
        md->at_line_start = false;
        md_newline(md);
        md->table_header_done = true;
    }
}

static void on_table_close(void *opaque)
{
    md_blank(opaque);
}

char *reader_markdown(const char *html, const char *url)
{
    Markdown md = {0};
    dbuf_init(&md.out);
    md.at_line_start = true;
    ReaderSink sink = {
        .opaque = &md,
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
    };
    if (!reader_walk(html, &sink)) {
        free(md.saved_pre);
        return dbuf_take(&md.out);
    }
    free(md.saved_pre);

    /* Where it came from, recorded once at the end: an exported document that
       cannot say what it is a reading of is hard to trust later. */
    if (url && *url) {
        md_blank(&md);
        dbuf_puts(&md.out, "---\n\nSource: ");
        dbuf_puts(&md.out, url);
        dbuf_puts(&md.out, "\n");
    }
    return dbuf_take(&md.out);
}
