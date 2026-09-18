#define _POSIX_C_SOURCE 200809L

#include "reader_walk.h"

#include "dom_util.h"

#include <lexbor/html/interfaces/document.h>

/* Where the walk is: counters that give links, controls and forms their
   document-order identities, and the form a control would submit with. */
typedef struct {
    const ReaderSink *sink;
    size_t next_link;
    size_t next_control;
    unsigned next_form;
    bool in_form;
    unsigned form_index;
    const char *action;
    const char *method;
} Walk;

#define EMIT(walk, name, ...) \
    do { if ((walk)->sink->name) (walk)->sink->name((walk)->sink->opaque, __VA_ARGS__); } while (0)
#define EMIT0(walk, name) \
    do { if ((walk)->sink->name) (walk)->sink->name((walk)->sink->opaque); } while (0)

static void walk_node(Walk *walk, lxb_dom_node_t *node);

static void walk_children(Walk *walk, lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
        walk_node(walk, child);
}

/* The template names its cards by class; say which card once, here. */
static const char *card_kind(const char *class_name)
{
    static const struct { const char *class_name; const char *card; } cards[] = {
        {"audio-card", "audio"}, {"video-card", "video"}, {"table-card", "table"},
        {"form-card", "form"}, {"result-list", "results"}, {"media-gallery", "gallery"},
    };
    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i)
        if (d_contains(class_name, cards[i].class_name)) return cards[i].card;
    return NULL;
}

static void walk_block(Walk *walk, lxb_dom_node_t *node, ReaderBlock block)
{
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    char *id = attr_value(element, "id");
    char *class_name = attr_value(element, "class");
    block.id = id;
    block.class_name = class_name;
    if (block.kind == READER_BLOCK_GENERIC) {
        const char *card = card_kind(class_name);
        if (card) { block.kind = READER_BLOCK_CARD; block.card = card; }
    }
    if (!block.card) block.card = "";
    EMIT(walk, block_open, &block);
    walk_children(walk, node);
    EMIT(walk, block_close, &block);
    free(id);
    free(class_name);
}

static void walk_table(Walk *walk, lxb_dom_node_t *table)
{
    EMIT0(walk, table_open);
    NodeVec rows = find_selector(table, "tr");
    for (size_t r = 0; r < rows.length; ++r) {
        size_t count = 0, capacity = 0;
        char **cells = NULL;
        bool *header = NULL;
        for (lxb_dom_node_t *cell = rows.items[r]->first_child; cell; cell = cell->next) {
            char *tag = node_tag(cell);
            bool is_header = d_eq(tag, "th");
            bool is_cell = is_header || d_eq(tag, "td");
            free(tag);
            if (!is_cell) continue;
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 8;
                cells = d_xrealloc(cells, capacity * sizeof(*cells));
                header = d_xrealloc(header, capacity * sizeof(*header));
            }
            cells[count] = compact_text(cell);
            header[count] = is_header;
            ++count;
        }
        EMIT(walk, table_row, (const char *const *)cells, header, count);
        for (size_t i = 0; i < count; ++i) free(cells[i]);
        free(cells);
        free(header);
    }
    free(rows.items);
    EMIT0(walk, table_close);
}

static void append_option_piece(DBuffer *out, const char *text)
{
    for (const char *at = text; *at; ++at) {
        char ch = (*at == 0x1e || *at == 0x1f) ? ' ' : *at;
        dbuf_append(out, &ch, 1);
    }
}

static void collect_options(lxb_dom_node_t *node, DBuffer *options, char **value)
{
    char *tag = node_tag(node);
    if (d_eq(tag, "option")) {
        lxb_dom_element_t *element = lxb_dom_interface_element(node);
        char *raw = attr_value(element, "value");
        char *label = compact_text(node);
        char *selected_attr = attr_value(element, "data-distill-selected");
        bool selected = d_eq(selected_attr, "1");
        const char *option_value = *raw ? raw : label;
        if (selected || !**value) { free(*value); *value = d_strdup(option_value); }
        append_option_piece(options, option_value);
        dbuf_puts(options, "\x1f");
        append_option_piece(options, label);
        dbuf_puts(options, "\x1f");
        dbuf_puts(options, selected ? "1" : "0");
        dbuf_puts(options, "\x1e");
        free(raw); free(label); free(selected_attr);
    }
    free(tag);
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
        collect_options(child, options, value);
}

static bool attr_is_one(lxb_dom_element_t *element, const char *name)
{
    char *value = attr_value(element, name);
    bool one = d_eq(value, "1");
    free(value);
    return one;
}

static void walk_control(Walk *walk, lxb_dom_node_t *node, const char *tag)
{
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    char *type_attr = attr_value(element, "type");
    char *distill_type = attr_value(element, "data-distill-type");
    char *id = attr_value(element, "id");
    char *name = attr_value(element, "name");
    char *value = attr_value(element, "value");
    char *value_attr = d_strdup(value);
    char *placeholder = attr_value(element, "placeholder");
    char *caption = d_strdup("");
    DBuffer options;
    dbuf_init(&options);
    if (d_eq(tag, "textarea")) { free(value); value = node_text(node); }
    if (d_eq(tag, "select")) collect_options(node, &options, &value);
    if (d_eq(tag, "button")) { free(caption); caption = compact_text(node); }
    char *options_text = dbuf_take(&options);
    ReaderControl control = {
        .in_form = walk->in_form,
        .form_index = walk->in_form ? walk->form_index : 0,
        .action = walk->in_form ? walk->action : "",
        .method = walk->in_form ? walk->method : "get",
        .tag = tag,
        .type_attr = type_attr,
        .type = *distill_type ? distill_type : (*type_attr ? type_attr : "text"),
        .id = id,
        .name = name,
        .value = value,
        .value_attr = value_attr,
        .placeholder = placeholder,
        .caption = caption,
        .options = options_text,
        .checked = attr_is_one(element, "data-distill-checked"),
        .disabled = attr_is_one(element, "data-distill-disabled"),
        .required = attr_is_one(element, "data-distill-required"),
        .index = walk->next_control++,
    };
    EMIT(walk, control, &control);
    free(type_attr); free(distill_type); free(id); free(name); free(value); free(value_attr);
    free(placeholder); free(caption); free(options_text);
}

static void walk_form(Walk *walk, lxb_dom_node_t *node)
{
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    char *action = attr_value(element, "action");
    char *method = attr_value(element, "method");
    Walk saved = *walk;
    walk->in_form = true;
    walk->form_index = walk->next_form++;
    walk->action = action;
    walk->method = *method ? method : "get";
    walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_GENERIC, .tag = "form"});
    walk->in_form = saved.in_form;
    walk->form_index = saved.form_index;
    walk->action = saved.action;
    walk->method = saved.method;
    free(action);
    free(method);
}

static void walk_node(Walk *walk, lxb_dom_node_t *node)
{
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const lxb_char_t *data = lxb_dom_node_text_content(node, &length);
        if (!data || !length) return;
        EMIT(walk, text, (const char *)data, length);
        return;
    }
    if (!is_element(node)) return;
    char *tag = node_tag(node);
    lxb_dom_element_t *element = lxb_dom_interface_element(node);

    if (d_eq(tag, "script") || d_eq(tag, "style") || d_eq(tag, "head")) { free(tag); return; }
    if (d_eq(tag, "a")) {
        char *class_name = attr_value(element, "class");
        bool anchor = d_contains(class_name, "section-anchor");
        free(class_name);
        if (anchor) {
            char *href = attr_value(element, "href");
            const char *id = d_starts(href, "distill-section:") ? href + strlen("distill-section:") : "";
            EMIT(walk, anchor, id);
            free(href);
            free(tag);
            return;
        }
    }

    if (d_eq(tag, "h1") || d_eq(tag, "h2") || d_eq(tag, "h3") ||
        d_eq(tag, "h4") || d_eq(tag, "h5") || d_eq(tag, "h6")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_HEADING, .tag = tag,
                                            .level = tag[1] - '0'});
    } else if (d_eq(tag, "form")) {
        walk_form(walk, node);
    } else if (d_eq(tag, "p") || d_eq(tag, "section") || d_eq(tag, "div") ||
               d_eq(tag, "main") || d_eq(tag, "article")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_GENERIC, .tag = tag});
    } else if (d_eq(tag, "br")) {
        EMIT0(walk, line_break);
    } else if (d_eq(tag, "hr")) {
        EMIT0(walk, rule);
    } else if (d_eq(tag, "ul") || d_eq(tag, "ol")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_LIST, .tag = tag,
                                            .ordered = d_eq(tag, "ol")});
    } else if (d_eq(tag, "li")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_ITEM, .tag = tag});
    } else if (d_eq(tag, "blockquote")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_QUOTE, .tag = tag});
    } else if (d_eq(tag, "pre")) {
        walk_block(walk, node, (ReaderBlock){.kind = READER_BLOCK_PRE, .tag = tag});
    } else if (d_eq(tag, "code") || d_eq(tag, "strong") || d_eq(tag, "b") ||
               d_eq(tag, "em") || d_eq(tag, "i")) {
        unsigned style = d_eq(tag, "code") ? READER_STYLE_CODE
                       : (d_eq(tag, "strong") || d_eq(tag, "b")) ? READER_STYLE_STRONG
                       : READER_STYLE_EM;
        EMIT(walk, style_open, style);
        walk_children(walk, node);
        EMIT(walk, style_close, style);
    } else if (d_eq(tag, "img")) {
        char *src = attr_value(element, "src");
        char *alt = attr_value(element, "alt");
        EMIT(walk, image, &(ReaderImage){.src = src, .alt = alt});
        free(src); free(alt);
    } else if (d_eq(tag, "a")) {
        char *class_name = attr_value(element, "class");
        char *href = attr_value(element, "href");
        ReaderLink link = {.href = href, .index = walk->next_link++,
                           .button = d_contains(class_name, "button")};
        EMIT(walk, link_open, &link);
        walk_children(walk, node);
        EMIT(walk, link_close, &link);
        free(class_name); free(href);
    } else if (d_eq(tag, "table")) {
        walk_table(walk, node);
    } else if (d_eq(tag, "input") || d_eq(tag, "textarea") || d_eq(tag, "select") ||
               d_eq(tag, "button")) {
        walk_control(walk, node, tag);
    } else {
        walk_children(walk, node);
    }
    free(tag);
}

bool reader_walk(const char *html, const ReaderSink *sink)
{
    if (!html || !sink) return false;
    lxb_html_document_t *document = lxb_html_document_create();
    if (!document) return false;
    if (lxb_html_document_parse(document, (const lxb_char_t *)html, strlen(html)) != LXB_STATUS_OK) {
        lxb_html_document_destroy(document);
        return false;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(document);

    NodeVec roots = find_selector(root, "html");
    NodeVec titles = find_selector(root, "title");
    lxb_dom_element_t *html_element = roots.length ? lxb_dom_interface_element(roots.items[0]) : NULL;
    char *dir = attr_value(html_element, "dir");
    char *lang = attr_value(html_element, "lang");
    char *title = titles.length ? compact_text(titles.items[0]) : d_strdup("");
    Walk walk = {.sink = sink, .action = "", .method = "get"};
    EMIT(&walk, page, &(ReaderPage){.title = title, .dir = dir, .lang = lang});
    free(roots.items); free(titles.items);
    free(dir); free(lang); free(title);

    NodeVec bodies = find_selector(root, "body");
    if (bodies.length) walk_node(&walk, bodies.items[0]);
    free(bodies.items);
    lxb_html_document_destroy(document);
    return true;
}
