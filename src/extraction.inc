#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/serialize.h>

typedef struct {
    lxb_dom_node_t **items;
    size_t length;
    size_t capacity;
} NodeVec;

typedef struct { char *type; char *text; } ExtractBlock;
typedef struct { char *caption; NodeVec rows; } ExtractTable;
typedef struct { char *title; char *url; char *snippet; } ExtractSearch;
typedef struct { char *label; char *name; char *type; char *value; } ExtractField;
typedef struct { char *action; char *method; char *enctype; ExtractField *fields; size_t field_count; size_t field_capacity; } ExtractForm;
typedef struct { char *kind; char *url; char *poster; char *title; } ExtractVideo;
typedef struct { char *kind; char *url; char *alt; } ExtractMedia;
typedef struct { char *text; char *url; } ExtractLink;

typedef struct {
    char *kind;
    double confidence;
    char *evidence;
} ExtractShape;

typedef struct {
    bool ok;
    char *title;
    char *byline; bool byline_null;
    char *dir; bool dir_null;
    char *lang; bool lang_null;
    char *content;
    char *text_content;
    char *excerpt; bool excerpt_null;
    char *site_name; bool site_name_null;
    char *published_time; bool published_time_null;
} ReadabilityFields;

static void node_vec_push(NodeVec *vec, lxb_dom_node_t *node)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = node;
}

static bool is_element(lxb_dom_node_t *node)
{
    return node && node->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

static char *node_text(lxb_dom_node_t *node)
{
    size_t length = 0;
    lxb_char_t *value = lxb_dom_node_text_content(node, &length);
    return value ? d_strndup((const char *)value, length) : d_strdup("");
}

static char *node_tag(lxb_dom_node_t *node)
{
    if (!is_element(node)) return d_strdup("");
    const lxb_char_t *name = lxb_dom_element_local_name(lxb_dom_interface_element(node), NULL);
    return d_lower(name ? (const char *)name : "");
}

static char *attr_value(lxb_dom_element_t *element, const char *name)
{
    if (!element) return d_strdup("");
    size_t length = 0;
    const lxb_char_t *value = lxb_dom_element_get_attribute(element,
        (const lxb_char_t *)name, strlen(name), &length);
    return value ? d_strndup((const char *)value, length) : d_strdup("");
}

static bool has_attr(lxb_dom_element_t *element, const char *name)
{
    return lxb_dom_element_has_attribute(element, (const lxb_char_t *)name, strlen(name));
}

static bool class_has(lxb_dom_element_t *element, const char *name)
{
    char *classes = attr_value(element, "class");
    bool found = false;
    char *cursor = classes;
    while (*cursor) {
        while (*cursor && d_isspace(*cursor)) ++cursor;
        char *end = cursor;
        while (*end && !d_isspace(*end)) ++end;
        char saved = *end;
        *end = 0;
        if (d_eq(cursor, name)) found = true;
        *end = saved;
        cursor = end;
    }
    free(classes);
    return found;
}

static bool selector_match(lxb_dom_node_t *node, const char *raw_selector)
{
    if (!is_element(node)) return false;
    char *selector = d_trim(raw_selector);
    if (!*selector) { free(selector); return false; }
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    char *tag = node_tag(node);
    const char *hash = strchr(selector, '#');
    const char *dot = strchr(selector, '.');
    const char *bracket = strchr(selector, '[');
    const char *end = selector + strlen(selector);
    if (hash && hash < end) end = hash;
    if (dot && dot < end) end = dot;
    if (bracket && bracket < end) end = bracket;
    char *wanted_tag = d_strndup(selector, (size_t)(end - selector));
    bool ok = !*wanted_tag || d_eq(wanted_tag, "*") || d_ieq(wanted_tag, tag);
    free(wanted_tag);
    if (ok && hash) {
        const char *hash_end = dot && dot > hash ? dot : (bracket && bracket > hash ? bracket : selector + strlen(selector));
        char *wanted = d_strndup(hash + 1, (size_t)(hash_end - hash - 1));
        char *actual = attr_value(element, "id");
        ok = d_eq(wanted, actual);
        free(wanted); free(actual);
    }
    if (ok && dot) {
        const char *class_end = bracket && bracket > dot ? bracket : selector + strlen(selector);
        char *wanted = d_strndup(dot + 1, (size_t)(class_end - dot - 1));
        ok = class_has(element, wanted);
        free(wanted);
    }
    if (ok && bracket) {
        const char *close = strchr(bracket, ']');
        size_t length = close ? (size_t)(close - bracket - 1) : strlen(bracket + 1);
        char *expression = d_strndup(bracket + 1, length);
        char *equals = strchr(expression, '=');
        if (equals) {
            *equals = 0;
            char *name = d_trim(expression);
            char *value = d_trim(equals + 1);
            size_t vl = strlen(value);
            if (vl >= 2 && ((value[0] == '"' && value[vl - 1] == '"') || (value[0] == '\'' && value[vl - 1] == '\''))) {
                value[vl - 1] = 0;
                memmove(value, value + 1, vl - 1);
            }
            char *actual = attr_value(element, name);
            ok = d_eq(actual, value);
            free(name); free(value); free(actual);
        } else {
            char *name = d_trim(expression);
            ok = has_attr(element, name);
            free(name);
        }
        free(expression);
    }
    free(selector); free(tag);
    return ok;
}

typedef struct { const char *selector; NodeVec *out; } FindContext;
typedef void (*NodeVisit)(lxb_dom_node_t *, void *);

static void walk_nodes(lxb_dom_node_t *node, NodeVisit visit, void *opaque)
{
    if (!node) return;
    visit(node, opaque);
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
        walk_nodes(child, visit, opaque);
}

static void find_walk(lxb_dom_node_t *node, void *opaque)
{
    FindContext *context = opaque;
    if (selector_match(node, context->selector)) node_vec_push(context->out, node);
}

static NodeVec find_selector(lxb_dom_node_t *root, const char *selector)
{
    NodeVec out = {0};
    FindContext context = {selector, &out};
    walk_nodes(root, find_walk, &context);
    return out;
}

static char *compact_text(lxb_dom_node_t *node)
{
    char *raw = node_text(node);
    DBuffer out; dbuf_init(&out);
    bool space = false;
    for (size_t i = 0; raw[i]; ++i) {
        if (d_isspace(raw[i])) { if (out.length) space = true; continue; }
        if (space && out.length) dbuf_puts(&out, " ");
        space = false;
        dbuf_append(&out, raw + i, 1);
    }
    free(raw);
    return dbuf_take(&out);
}

static bool declaration_has(const char *declarations, const char *property, const char *value)
{
    char *lower = d_lower(declarations);
    DBuffer compact; dbuf_init(&compact);
    for (size_t i = 0; lower[i]; ++i) if (!d_isspace(lower[i])) dbuf_append(&compact, lower + i, 1);
    char *needle = d_cat3(property, ":", value);
    bool found = strstr(compact.data ? compact.data : "", needle) != NULL;
    free(lower); free(needle); dbuf_free(&compact);
    return found;
}

static bool css_hides(lxb_dom_node_t *root, lxb_dom_node_t *target)
{
    NodeVec styles = find_selector(root, "style");
    for (size_t i = 0; i < styles.length; ++i) {
        char *css = node_text(styles.items[i]);
        char *cursor = css;
        while (*cursor) {
            char *open = strchr(cursor, '{');
            if (!open) break;
            char *close = strchr(open + 1, '}');
            if (!close) break;
            char *selectors = d_strndup(cursor, (size_t)(open - cursor));
            char *declarations = d_strndup(open + 1, (size_t)(close - open - 1));
            bool hides = declaration_has(declarations, "display", "none") || declaration_has(declarations, "visibility", "hidden");
            if (hides) {
                char *part = selectors;
                while (part) {
                    char *comma = strchr(part, ',');
                    if (comma) *comma = 0;
                    char *clean = d_trim(part);
                    if (selector_match(target, clean)) { free(clean); free(selectors); free(declarations); free(css); free(styles.items); return true; }
                    free(clean);
                    part = comma ? comma + 1 : NULL;
                }
            }
            free(selectors); free(declarations);
            cursor = close + 1;
        }
        free(css);
    }
    free(styles.items);
    return false;
}

static bool hidden_node(lxb_dom_node_t *root, lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *current = node; current; current = current->parent) {
        if (!is_element(current)) continue;
        lxb_dom_element_t *element = lxb_dom_interface_element(current);
        if (has_attr(element, "hidden")) return true;
        char *aria = attr_value(element, "aria-hidden");
        bool hidden = d_ieq(aria, "true"); free(aria);
        if (hidden) return true;
        char *style = attr_value(element, "style");
        hidden = declaration_has(style, "display", "none") || declaration_has(style, "visibility", "hidden");
        free(style);
        if (hidden || css_hides(root, current)) return true;
    }
    return false;
}

static bool ancestor_class(lxb_dom_node_t *node, const char *name)
{
    for (lxb_dom_node_t *current = node; current; current = current->parent)
        if (is_element(current) && class_has(lxb_dom_interface_element(current), name)) return true;
    return false;
}

static bool class_contains(lxb_dom_element_t *element, const char *needle)
{
    char *classes = attr_value(element, "class");
    bool found = d_contains(classes, needle);
    free(classes);
    return found;
}

static char *safe_uri(Engine *engine, const char *candidate, size_t *rejected)
{
    char *raw = d_trim(candidate);
    if (!*raw) { free(raw); return d_strdup(""); }
    char *resolved = resolve_url(engine->base_url, raw);
    free(raw);
    bool allowed = d_starts(resolved, "http:") || d_starts(resolved, "https:") || d_starts(resolved, "mailto:") || d_starts(resolved, "tel:");
    if (!allowed) { if (rejected) ++*rejected; free(resolved); return d_strdup(""); }
    return resolved;
}

/* Text carried by visible elements of one kind. The shape decision is a
   question about proportion -- what is most of this page? -- and answering it
   from element counts alone gets it wrong: a site-wide search box, one
   tracking iframe, or a single layout table is present on pages that are
   overwhelmingly prose. */
static size_t visible_text_chars(lxb_dom_node_t *root, const char *selector)
{
    NodeVec nodes = find_selector(root, selector);
    size_t total = 0;
    for (size_t i = 0; i < nodes.length; ++i) {
        if (hidden_node(root, nodes.items[i])) continue;
        char *text = compact_text(nodes.items[i]);
        total += strlen(text);
        free(text);
    }
    free(nodes.items);
    return total;
}

/* Enough prose that the page is about its text, whatever else it also
   contains. Calibrated against the corpus: the isolating fixtures for form,
   table, search, video, and gallery shapes carry 0-151 characters of
   paragraph text, while a real article carries thousands. */
#define DISTILL_PROSE_FLOOR 600

static ExtractShape shape_for(lxb_dom_node_t *root)
{
    NodeVec articles = find_selector(root, "article");
    NodeVec mains = find_selector(root, "main");
    NodeVec tables = find_selector(root, "table");
    NodeVec forms = find_selector(root, "form");
    NodeVec images = find_selector(root, "img");
    NodeVec videos = find_selector(root, "video");
    NodeVec iframes = find_selector(root, "iframe");
    NodeVec links = find_selector(root, "a");
    size_t result_links = 0, visible_images = 0;
    for (size_t i = 0; i < links.length; ++i) if (!hidden_node(root, links.items[i]) && ancestor_class(links.items[i], "result")) ++result_links;
    for (size_t i = 0; i < images.length; ++i) if (!hidden_node(root, images.items[i])) ++visible_images;
    size_t prose_chars = visible_text_chars(root, "p");
    size_t table_chars = tables.length ? visible_text_chars(root, "table") : 0;
    /* A page whose text is mostly prose is an article even when it also
       carries a search box, an embed, or a handful of images. Without this
       the first matching element decided the shape, so every encyclopaedia
       article rendered as its own site-search form. */
    bool prose_dominates = prose_chars >= DISTILL_PROSE_FLOOR;
    ExtractShape shape = {d_strdup("structured-text"), .55, d_strdup("fallback structured text")};
    if (!prose_dominates && forms.length && ((is_element(forms.items[0]) && class_contains(lxb_dom_interface_element(forms.items[0]), "search")) || ancestor_class(forms.items[0], "login"))) { free(shape.kind); free(shape.evidence); shape = (ExtractShape){d_strdup("form"), .95, d_strdup("native form controls without a prose body")}; }
    else if (!prose_dominates && result_links >= 2) { free(shape.kind); free(shape.evidence); double confidence = .65 + result_links * .06; shape = (ExtractShape){d_strdup("search-results"), confidence > .99 ? .99 : confidence, d_strdup("repeated result links")}; }
    else if (!prose_dominates && (videos.length || iframes.length || find_selector(root, "embed").length || find_selector(root, "object").length)) { free(shape.kind); free(shape.evidence); shape = (ExtractShape){d_strdup("video"), .92, d_strdup("video or embed element without a prose body")}; }
    /* A table wins only on a page that is not otherwise prose. An
       encyclopaedia article carries infoboxes and navigation boxes whose
       combined text exceeds its paragraphs, which does not make it a table. */
    else if (!prose_dominates && tables.length && table_chars >= prose_chars) { free(shape.kind); free(shape.evidence); shape = (ExtractShape){d_strdup("table"), .94, d_strdup("table holding most of the page text")}; }
    else {
        NodeVec paragraphs = find_selector(root, "p");
        NodeVec headings = find_selector(root, "h1");
        NodeVec h2 = find_selector(root, "h2");
        NodeVec h3 = find_selector(root, "h3");
        NodeVec code = find_selector(root, "pre");
        NodeVec code_inline = find_selector(root, "code");
        if (!prose_dominates && visible_images >= 3 && visible_images > articles.length + mains.length) { free(shape.kind); free(shape.evidence); double confidence = .70 + visible_images * .04; shape = (ExtractShape){d_strdup("image-heavy"), confidence > .98 ? .98 : confidence, d_strdup("multiple visible images without a prose body")}; }
        /* Reference documentation is headings plus code blocks. Inline
           <code> alone does not qualify: prose mentioning a function name is
           still prose, which is why this tests <pre> rather than counting
           every inline span. */
        else if (!articles.length && headings.length + h2.length + h3.length && code.length) { free(shape.kind); free(shape.evidence); shape = (ExtractShape){d_strdup("docs"), .88, d_strdup("headings with code blocks")}; }
        else if (articles.length || mains.length || paragraphs.length >= 2) { free(shape.kind); free(shape.evidence); double confidence = .72 + paragraphs.length * .03; shape = (ExtractShape){d_strdup("article"), confidence > .98 ? .98 : confidence, d_strdup("article/main or paragraph body")}; }
        free(paragraphs.items); free(headings.items); free(h2.items); free(h3.items); free(code.items); free(code_inline.items);
    }
    free(articles.items); free(mains.items); free(tables.items); free(forms.items); free(images.items); free(videos.items); free(iframes.items); free(links.items);
    return shape;
}

static char *meta_value(lxb_dom_node_t *root, const char *property)
{
    NodeVec metas = find_selector(root, "meta");
    for (size_t i = 0; i < metas.length; ++i) {
        char *value = attr_value(lxb_dom_interface_element(metas.items[i]), "property");
        bool match = d_ieq(value, property); free(value);
        if (match) { char *content = attr_value(lxb_dom_interface_element(metas.items[i]), "content"); free(metas.items); return content; }
    }
    free(metas.items); return d_strdup("");
}

static char *json_quote(const char *value)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "\"");
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
        switch (*p) {
            case '"': dbuf_puts(&out, "\\\""); break;
            case '\\': dbuf_puts(&out, "\\\\"); break;
            case '\n': dbuf_puts(&out, "\\n"); break;
            case '\r': dbuf_puts(&out, "\\r"); break;
            case '\t': dbuf_puts(&out, "\\t"); break;
            default: if (*p < 0x20) dbuf_printf(&out, "\\u%04x", *p); else dbuf_append(&out, (const char *)p, 1); break;
        }
    }
    dbuf_puts(&out, "\""); return dbuf_take(&out);
}

static void free_readability(ReadabilityFields *fields)
{
    free(fields->title); free(fields->byline); free(fields->dir); free(fields->lang); free(fields->content); free(fields->text_content);
    free(fields->excerpt); free(fields->site_name); free(fields->published_time); *fields = (ReadabilityFields){0};
}

static char *readability_property(Engine *engine, JSValueConst object, const char *name, bool *nullish)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    bool is_null = JS_IsNull(value) || JS_IsUndefined(value);
    if (nullish) *nullish = is_null;
    char *out = is_null ? d_strdup("") : js_string(engine->context, value);
    JS_FreeValue(engine->context, value); return out;
}

static bool readability_void_tag(const char *name)
{
    const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) if (d_eq(name, tags[i])) return true;
    return false;
}

static char *readability_snapshot(const char *html)
{
    DBuffer out; dbuf_init(&out);
    char *lower_html = d_lower(html ? html : "");
    bool wrapped = !d_contains(lower_html, "<html");
    free(lower_html);
    if (wrapped) dbuf_puts(&out, "<!DOCTYPE html><html><head></head><body>");
    for (size_t i = 0; html && html[i];) {
        if (html[i] != '<') { dbuf_append(&out, html + i, 1); ++i; continue; }
        const char *end = strchr(html + i, '>');
        if (!end) { dbuf_puts(&out, html + i); break; }
        size_t length = (size_t)(end - html - i + 1);
        char *tag = d_strndup(html + i, length);
        char *tag_lower = d_lower(tag);
        if (d_starts(tag_lower, "<!doctype")) {
            if (!wrapped) dbuf_puts(&out, "<!DOCTYPE html>");
            free(tag_lower); free(tag); i += length; continue;
        }
        free(tag_lower);
        size_t name_start = 1;
        while (name_start + 1 < length && d_isspace(tag[name_start])) ++name_start;
        if (name_start < length && tag[name_start] != '/' && tag[name_start] != '!' && tag[name_start] != '?') {
            size_t name_end = name_start;
            while (name_end < length && !d_isspace(tag[name_end]) && tag[name_end] != '/' && tag[name_end] != '>') ++name_end;
            char *raw_name = d_strndup(tag + name_start, name_end - name_start);
            char *name = d_lower(raw_name); free(raw_name);
            bool closed = length >= 3 && tag[length - 2] == '/';
            if (readability_void_tag(name) && !closed) {
                dbuf_append(&out, tag, length - 1); dbuf_puts(&out, "/> ");
                --out.length; out.data[out.length] = 0;
            } else dbuf_append(&out, tag, length);
            free(name);
        } else dbuf_append(&out, tag, length);
        free(tag); i += length;
    }
    if (wrapped) dbuf_puts(&out, "</body></html>");
    return dbuf_take(&out);
}

static ReadabilityFields run_readability(Engine *engine, const char *serialized)
{
    ReadabilityFields fields = {0};
    char *parser_path = d_cat3(DISTILL_SOURCE_DIR, "/external/src/readability/JSDOMParser.js", "");
    char *readability_path = d_cat3(DISTILL_SOURCE_DIR, "/external/src/readability/Readability.js", "");
    size_t parser_length = 0, readability_length = 0;
    char *parser = d_read_file(parser_path, &parser_length);
    char *readability = d_read_file(readability_path, &readability_length);
    free(parser_path); free(readability_path);
    if (!parser || !readability) { free(parser); free(readability); return fields; }
    JSValue result = JS_Eval(engine->context, parser, parser_length, "JSDOMParser.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) { note_exception(engine, "Readability parser"); JS_FreeValue(engine->context, result); free(parser); free(readability); return fields; }
    JS_FreeValue(engine->context, result);
    result = JS_Eval(engine->context, readability, readability_length, "Readability.js", JS_EVAL_TYPE_GLOBAL);
    free(parser); free(readability);
    if (JS_IsException(result)) { note_exception(engine, "Readability"); JS_FreeValue(engine->context, result); return fields; }
    JS_FreeValue(engine->context, result);
    char *snapshot = readability_snapshot(serialized);
    JS_SetPropertyStr(engine->context, engine->global, "__distill_readability_html", js_str(engine->context, snapshot));
    JS_SetPropertyStr(engine->context, engine->global, "__distill_readability_url", js_str(engine->context, engine->base_url));
    const char *run = "(function(){var doc=new JSDOMParser().parse(__distill_readability_html,__distill_readability_url);return new Readability(doc,{disableJSONLD:false}).parse();})()";
    result = JS_Eval(engine->context, run, strlen(run), "distill-readability.js", JS_EVAL_TYPE_GLOBAL);
    free(snapshot);
    if (JS_IsException(result) || JS_IsNull(result) || JS_IsUndefined(result)) { if (JS_IsException(result)) note_exception(engine, "Readability.parse"); JS_FreeValue(engine->context, result); return fields; }
    fields.ok = true;
    fields.title = readability_property(engine, result, "title", NULL);
    fields.byline = readability_property(engine, result, "byline", &fields.byline_null);
    fields.dir = readability_property(engine, result, "dir", &fields.dir_null);
    fields.lang = readability_property(engine, result, "lang", &fields.lang_null);
    fields.content = readability_property(engine, result, "content", NULL);
    fields.text_content = readability_property(engine, result, "textContent", NULL);
    fields.excerpt = readability_property(engine, result, "excerpt", &fields.excerpt_null);
    fields.site_name = readability_property(engine, result, "siteName", &fields.site_name_null);
    fields.published_time = readability_property(engine, result, "publishedTime", &fields.published_time_null);
    JS_FreeValue(engine->context, result); return fields;
}

static char *sanitize_content(const char *html)
{
    DBuffer out; dbuf_init(&out);
    for (const char *p = html ? html : ""; *p;) {
        if (*p != '<') { dbuf_append(&out, p, 1); ++p; continue; }
        const char *end = strchr(p, '>'); if (!end) break;
        size_t length = (size_t)(end - p + 1); char *tag = d_strndup(p, length); char *lower = d_lower(tag);
        if (!d_contains(lower, "javascript:") && !d_contains(lower, "data:")) dbuf_append(&out, tag, length);
        free(tag); free(lower); p = end + 1;
    }
    return dbuf_take(&out);
}

/* The destination a block of text points at, if any. A front page is a list
   of headlines that are themselves links, and a headline stripped of its link
   is not usable information -- the reader shows the words but gives the reader
   no way to follow them. Look outwards first, because the common shape is an
   anchor wrapping the heading, then inwards for a heading containing a single
   link. */
static char *block_href(Engine *engine, lxb_dom_node_t *root, lxb_dom_node_t *node,
                        size_t *rejected)
{
    for (lxb_dom_node_t *up = node->parent; up; up = up->parent) {
        if (!is_element(up)) continue;
        char *tag = node_tag(up);
        bool anchor = d_eq(tag, "a");
        free(tag);
        if (!anchor) continue;
        char *raw = attr_value(lxb_dom_interface_element(up), "href");
        char *url = safe_uri(engine, raw, rejected);
        free(raw);
        if (*url) return url;
        free(url);
        break;
    }
    NodeVec inner = find_selector(node, "a");
    char *found = d_strdup("");
    for (size_t i = 0; i < inner.length; ++i) {
        char *raw = attr_value(lxb_dom_interface_element(inner.items[i]), "href");
        char *url = safe_uri(engine, raw, rejected);
        free(raw);
        if (*url) { free(found); found = url; break; }
        free(url);
    }
    free(inner.items);
    (void)root;
    return found;
}

static void append_blocks(Engine *engine, DBuffer *out, lxb_dom_node_t *root, size_t *rejected)
{
    const char *tags[] = {"h1","h2","h3","h4","h5","h6","p","pre","blockquote","li","dt","dd"};
    bool first = true; dbuf_puts(out, "[");
    for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); ++t) {
        NodeVec nodes = find_selector(root, tags[t]);
        for (size_t i = 0; i < nodes.length; ++i) {
            if (hidden_node(root, nodes.items[i])) continue;
            char *text = compact_text(nodes.items[i]); if (!*text) { free(text); continue; }
            char *href = block_href(engine, root, nodes.items[i], rejected);
            if (!first) dbuf_puts(out, ","); first = false;
            char *qt = json_quote(tags[t]), *qx = json_quote(text), *qh = json_quote(href);
            dbuf_printf(out, "{\"type\":%s,\"text\":%s,\"href\":%s}", qt, qx, qh);
            free(qt); free(qx); free(qh); free(text); free(href);
        }
        free(nodes.items);
    }
    dbuf_puts(out, "]");
}

/* The document's own title. Readability supplies one when it runs, but it
   returns nothing at all on pages it cannot parse -- which are exactly the
   pages where the reader has least else to show, and where labelling the tab
   "Untitled page" is least helpful. */
static char *document_title(lxb_dom_node_t *root)
{
    NodeVec titles = find_selector(root, "title");
    char *text = d_strdup("");
    for (size_t i = 0; i < titles.length; ++i) {
        free(text);
        text = compact_text(titles.items[i]);
        if (*text) break;
    }
    free(titles.items);
    if (!*text) { free(text); text = meta_value(root, "og:title"); }
    return text;
}

static char *table_json(lxb_dom_node_t *root)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first_table = true;
    NodeVec tables = find_selector(root, "table");
    for (size_t i = 0; i < tables.length; ++i) {
        NodeVec captions = find_selector(tables.items[i], "caption"); NodeVec rows = find_selector(tables.items[i], "tr");
        if (!first_table) dbuf_puts(&out, ","); first_table = false;
        char *caption = captions.length ? compact_text(captions.items[0]) : d_strdup(""); char *qc = json_quote(caption);
        dbuf_printf(&out, "{\"caption\":%s,\"rows\":[", qc); free(qc); free(caption);
        for (size_t r = 0; r < rows.length; ++r) {
            NodeVec cells = find_selector(rows.items[r], "th"); NodeVec td = find_selector(rows.items[r], "td");
            for (size_t j = 0; j < td.length; ++j) node_vec_push(&cells, td.items[j]);
            if (r) dbuf_puts(&out, ","); dbuf_puts(&out, "[");
            for (size_t c = 0; c < cells.length; ++c) { if (c) dbuf_puts(&out, ","); char *text = compact_text(cells.items[c]); char *q = json_quote(text); dbuf_puts(&out, q); free(q); free(text); }
            dbuf_puts(&out, "]"); free(cells.items); free(td.items);
        }
        dbuf_puts(&out, "]}"); free(captions.items); free(rows.items);
    }
    free(tables.items); dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *search_json(Engine *engine, lxb_dom_node_t *root, size_t *rejected)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first = true; NodeVec links = find_selector(root, "a");
    for (size_t i = 0; i < links.length; ++i) {
        if (hidden_node(root, links.items[i]) || !ancestor_class(links.items[i], "result")) continue;
        char *raw = attr_value(lxb_dom_interface_element(links.items[i]), "href"); char *url = safe_uri(engine, raw, rejected); free(raw);
        char *title = compact_text(links.items[i]); char *snippet = compact_text(links.items[i]->parent); if (d_eq(snippet, title)) { free(snippet); snippet = d_strdup(""); }
        if (!*url || !*title) { free(url); free(title); free(snippet); continue; }
        if (!first) dbuf_puts(&out, ","); first = false;
        char *qt = json_quote(title), *qu = json_quote(url), *qs = json_quote(snippet);
        dbuf_printf(&out, "{\"title\":%s,\"url\":%s,\"snippet\":%s}", qt, qu, qs); free(qt); free(qu); free(qs); free(url); free(title); free(snippet);
    }
    free(links.items); dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *forms_json(Engine *engine, lxb_dom_node_t *root, size_t *rejected)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first_form = true; NodeVec forms = find_selector(root, "form");
    for (size_t i = 0; i < forms.length; ++i) {
        if (hidden_node(root, forms.items[i])) continue;
        lxb_dom_element_t *form = lxb_dom_interface_element(forms.items[i]); char *raw = attr_value(form, "action"); char *action = safe_uri(engine, raw, rejected); free(raw); if (!*action) { free(action); action = d_strdup(engine->base_url); }
        char *method_raw = attr_value(form, "method"); char *method = d_lower(method_raw); free(method_raw); if (!*method) { free(method); method = d_strdup("get"); }
        char *enctype = attr_value(form, "enctype"); if (!*enctype) { free(enctype); enctype = d_strdup("application/x-www-form-urlencoded"); }
        if (!first_form) dbuf_puts(&out, ","); first_form = false;
        char *q = json_quote(action); dbuf_printf(&out, "{\"action\":%s,\"method\":", q); free(q); q = json_quote(method); dbuf_puts(&out, q); free(q); q = json_quote(enctype); dbuf_printf(&out, ",\"enctype\":%s,\"fields\":[", q); free(q);
        bool first_field = true; const char *tags[] = {"input", "textarea", "select", "button"};
        for (size_t t = 0; t < 4; ++t) {
            NodeVec fields = find_selector(forms.items[i], tags[t]);
            for (size_t f = 0; f < fields.length; ++f) {
                if (hidden_node(root, fields.items[f])) continue;
                lxb_dom_element_t *field = lxb_dom_interface_element(fields.items[f]); char *name = attr_value(field, "name"); char *type_raw = attr_value(field, "type"); char *type = d_lower(type_raw); free(type_raw); if (!*type) { free(type); type = d_strdup(d_eq(tags[t], "button") ? "submit" : tags[t]); }
                char *value = attr_value(field, "value"); if (!*value) { free(value); value = compact_text(fields.items[f]); }
                char *id = attr_value(field, "id"); char *label = d_strdup(""); NodeVec labels = find_selector(root, "label");
                for (size_t l = 0; l < labels.length; ++l) { char *for_id = attr_value(lxb_dom_interface_element(labels.items[l]), "for"); if ((*id && d_eq(id, for_id)) || labels.items[l]->parent == fields.items[f]->parent) { free(label); label = compact_text(labels.items[l]); free(for_id); break; } free(for_id); }
                free(labels.items); free(id); if (!first_field) dbuf_puts(&out, ","); first_field = false;
                char *ql = json_quote(label), *qn = json_quote(name), *qt = json_quote(type), *qv = json_quote(value); dbuf_printf(&out, "{\"label\":%s,\"name\":%s,\"type\":%s,\"value\":%s}", ql, qn, qt, qv); free(ql); free(qn); free(qt); free(qv); free(label); free(name); free(type); free(value);
            }
            free(fields.items);
        }
        dbuf_puts(&out, "]}"); free(action); free(method); free(enctype);
    }
    free(forms.items); dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *videos_json(Engine *engine, lxb_dom_node_t *root, size_t *rejected)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first = true; const char *tags[] = {"video", "iframe", "embed", "object"};
    for (size_t t = 0; t < 4; ++t) {
        NodeVec nodes = find_selector(root, tags[t]);
        for (size_t i = 0; i < nodes.length; ++i) {
            lxb_dom_element_t *element = lxb_dom_interface_element(nodes.items[i]); char *raw = attr_value(element, "src"); char *url = safe_uri(engine, raw, rejected); free(raw); char *poster_raw = attr_value(element, "poster"); char *poster = safe_uri(engine, poster_raw, rejected); free(poster_raw); char *title = attr_value(element, "title"); if (!*title) { free(title); title = meta_value(root, "og:title"); }
            if (!first) dbuf_puts(&out, ","); first = false; char *qk = json_quote(tags[t]), *qu = json_quote(url), *qp = json_quote(poster), *qt = json_quote(title); dbuf_printf(&out, "{\"kind\":%s,\"url\":%s,\"poster\":%s,\"title\":%s}", qk, qu, qp, qt); free(qk); free(qu); free(qp); free(qt); free(url); free(poster); free(title);
        }
        free(nodes.items);
    }
    if (first) { char *raw = meta_value(root, "og:video"); if (*raw) { char *url = safe_uri(engine, raw, rejected); char *poster_raw = meta_value(root, "og:image"); char *poster = safe_uri(engine, poster_raw, rejected); free(poster_raw); char *title = meta_value(root, "og:title"); char *qk = json_quote("metadata"), *qu = json_quote(url), *qp = json_quote(poster), *qt = json_quote(title); dbuf_printf(&out, "{\"kind\":%s,\"url\":%s,\"poster\":%s,\"title\":%s}", qk, qu, qp, qt); free(qk); free(qu); free(qp); free(qt); free(url); free(poster); free(title); } free(raw); }
    dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *media_json(Engine *engine, lxb_dom_node_t *root, size_t *rejected)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first = true; NodeVec images = find_selector(root, "img");
    for (size_t i = 0; i < images.length; ++i) { if (hidden_node(root, images.items[i])) continue; lxb_dom_element_t *element = lxb_dom_interface_element(images.items[i]); char *raw = attr_value(element, "src"); if (!*raw) { free(raw); raw = attr_value(element, "data-src"); } char *url = safe_uri(engine, raw, rejected); free(raw); if (!*url) { free(url); continue; } char *alt = attr_value(element, "alt"); char *classes_raw = attr_value(element, "class"); char *classes = d_lower(classes_raw); free(classes_raw); bool decorative = !*alt || d_contains(classes, "icon") || d_contains(classes, "spacer"); free(classes); if (!first) dbuf_puts(&out, ","); first = false; char *qk = json_quote(decorative ? "decorative" : "content"), *qu = json_quote(url), *qa = json_quote(alt); dbuf_printf(&out, "{\"kind\":%s,\"url\":%s,\"alt\":%s}", qk, qu, qa); free(qk); free(qu); free(qa); free(url); free(alt); }
    free(images.items); dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *links_json(Engine *engine, lxb_dom_node_t *root, size_t *rejected)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first = true; NodeVec links = find_selector(root, "a");
    for (size_t i = 0; i < links.length; ++i) { if (hidden_node(root, links.items[i])) continue; char *raw = attr_value(lxb_dom_interface_element(links.items[i]), "href"); char *url = safe_uri(engine, raw, rejected); free(raw); if (!*url) { free(url); continue; } char *text = compact_text(links.items[i]); if (!first) dbuf_puts(&out, ","); first = false; char *qt = json_quote(text), *qu = json_quote(url); dbuf_printf(&out, "{\"text\":%s,\"url\":%s}", qt, qu); free(qt); free(qu); free(text); free(url); }
    free(links.items); dbuf_puts(&out, "]"); return dbuf_take(&out);
}

/* Pages that never finish hydrating still ship their record in a JSON-LD
   block, and the script discovery pass deliberately skips those blocks
   instead of executing them. Reading them here recovers the page's own
   description of itself without depending on the application starting. */
static char *structured_data_json(Engine *engine, lxb_dom_node_t *root)
{
    DBuffer out; dbuf_init(&out); dbuf_puts(&out, "["); bool first = true;
    NodeVec scripts = find_selector(root, "script");
    for (size_t i = 0; i < scripts.length; ++i) {
        lxb_dom_element_t *element = lxb_dom_interface_element(scripts.items[i]);
        char *type_raw = attr_value(element, "type");
        char *type = d_lower(type_raw);
        free(type_raw);
        if (!d_contains(type, "ld+json")) { free(type); continue; }
        free(type);
        char *text = node_text(scripts.items[i]);
        char *body = d_trim(text);
        free(text);
        if (!*body) { free(body); continue; }
        /* The block is page-controlled text. It is parsed to confirm it is
           well formed and to lift a few common fields; malformed blocks are
           reported rather than dropped, and the raw text is always emitted as
           a JSON string so one bad block cannot corrupt our own output. */
        JSValue parsed = JS_ParseJSON(engine->context, body, strlen(body),
                                      "distill-ld.json");
        bool valid = !JS_IsException(parsed);
        char *kind = d_strdup(""), *name = d_strdup(""), *description = d_strdup("");
        if (valid) {
            JSValue object = parsed;
            /* A top-level array or @graph wrapper holds the real records. */
            if (JS_IsArray(object)) {
                JSValue zero = JS_GetPropertyUint32(engine->context, object, 0);
                if (JS_IsObject(zero)) { JS_FreeValue(engine->context, object); object = zero; }
                else JS_FreeValue(engine->context, zero);
            }
            const char *fields[] = {"@type", "name", "description"};
            char **targets[] = {&kind, &name, &description};
            for (size_t f = 0; f < 3; ++f) {
                JSValue value = JS_GetPropertyStr(engine->context, object, fields[f]);
                if (JS_IsString(value)) {
                    const char *raw = JS_ToCString(engine->context, value);
                    if (raw) { free(*targets[f]); *targets[f] = d_strdup(raw); JS_FreeCString(engine->context, raw); }
                }
                JS_FreeValue(engine->context, value);
            }
            if (!*name) {
                JSValue value = JS_GetPropertyStr(engine->context, object, "headline");
                if (JS_IsString(value)) {
                    const char *raw = JS_ToCString(engine->context, value);
                    if (raw) { free(name); name = d_strdup(raw); JS_FreeCString(engine->context, raw); }
                }
                JS_FreeValue(engine->context, value);
            }
            JS_FreeValue(engine->context, object);
        } else {
            JS_FreeValue(engine->context, JS_GetException(engine->context));
        }
        if (!first) dbuf_puts(&out, ","); first = false;
        char *qk = json_quote(kind), *qn = json_quote(name), *qd = json_quote(description), *qc = json_quote(body);
        dbuf_printf(&out, "{\"format\":\"ld+json\",\"valid\":%s,\"type\":%s,\"name\":%s,\"description\":%s,\"content\":%s}",
                    valid ? "true" : "false", qk, qn, qd, qc);
        free(qk); free(qn); free(qd); free(qc);
        free(kind); free(name); free(description); free(body);
    }
    free(scripts.items);
    dbuf_puts(&out, "]"); return dbuf_take(&out);
}

static char *run_extraction(Engine *engine, const char *serialized)
{
    lxb_html_document_t *document = lxb_html_document_create();
    if (!document || lxb_html_document_parse(document, (const lxb_char_t *)serialized, strlen(serialized)) != LXB_STATUS_OK) { if (document) lxb_html_document_destroy(document); return d_strdup("{\"schema\":1,\"readability\":null}"); }
    lxb_dom_node_t *root = lxb_dom_interface_node(document);
    NodeVec all_elements = find_selector(root, "*");
    for (size_t i = 0; i < all_elements.length; ++i) {
        if (!hidden_node(root, all_elements.items[i])) continue;
        lxb_dom_element_set_attribute(lxb_dom_interface_element(all_elements.items[i]),
                                      (const lxb_char_t *)"hidden", 6,
                                      (const lxb_char_t *)"true", 4);
    }
    free(all_elements.items);
    lexbor_str_t filtered = {0};
    lxb_html_serialize_tree_str(root, &filtered);
    const char *readability_input = filtered.data ? (const char *)filtered.data : serialized;
    ExtractShape shape = shape_for(root); ReadabilityFields readability = run_readability(engine, readability_input); size_t rejected = 0;
    char *content = sanitize_content(readability.content); char *tables = table_json(root); char *search = search_json(engine, root, &rejected); char *forms = forms_json(engine, root, &rejected); char *videos = videos_json(engine, root, &rejected); char *media = media_json(engine, root, &rejected); char *links = links_json(engine, root, &rejected); char *structured = structured_data_json(engine, root);
    const char *selected = "structured-text"; if (d_eq(shape.kind, "article")) selected = readability.ok ? "readability" : "structured-text"; else if (d_eq(shape.kind, "table")) selected = "table"; else if (d_eq(shape.kind, "search-results")) selected = "search-results"; else if (d_eq(shape.kind, "form")) selected = "form"; else if (d_eq(shape.kind, "video")) selected = "video-metadata"; else if (d_eq(shape.kind, "image-heavy")) selected = "media-reference";
    DBuffer out; dbuf_init(&out); char *page_title = document_title(root);
    char *qurl = json_quote(engine->source_url), *qbase = json_quote(engine->base_url), *qkind = json_quote(shape.kind), *qevidence = json_quote(shape.evidence), *qselected = json_quote(selected), *qtitle = json_quote(page_title);
    dbuf_printf(&out, "{\"schema\":1,\"url\":%s,\"base_url\":%s,\"title\":%s,\"shape\":{\"kind\":%s,\"confidence\":%.2f,\"evidence\":%s},\"extractor\":%s,\"extractor_registry\":[\"readability\",\"structured-text\",\"table\",\"search-results\",\"form\",\"video-metadata\",\"media-reference\"],\"readability\":", qurl, qbase, qtitle, qkind, shape.confidence, qevidence, qselected);
    if (!readability.ok) dbuf_puts(&out, "null"); else { char *qt = json_quote(readability.title), *qb = json_quote(readability.byline), *qd = json_quote(readability.dir), *ql = json_quote(readability.lang), *qc = json_quote(content), *qtc = json_quote(readability.text_content), *qe = json_quote(readability.excerpt), *qs = json_quote(readability.site_name), *qp = json_quote(readability.published_time); dbuf_printf(&out, "{\"title\":%s,\"byline\":%s,\"dir\":%s,\"lang\":%s,\"content\":%s,\"textContent\":%s,\"excerpt\":%s,\"siteName\":%s,\"publishedTime\":%s}", qt, readability.byline_null ? "null" : qb, readability.dir_null ? "null" : qd, readability.lang_null ? "null" : ql, qc, qtc, readability.excerpt_null ? "null" : qe, readability.site_name_null ? "null" : qs, readability.published_time_null ? "null" : qp); free(qt); free(qb); free(qd); free(ql); free(qc); free(qtc); free(qe); free(qs); free(qp); }
    dbuf_puts(&out, ",\"blocks\":"); append_blocks(engine, &out, root, &rejected); dbuf_printf(&out, ",\"tables\":%s,\"search_results\":%s,\"forms\":%s,\"videos\":%s,\"media\":%s,\"links\":%s,\"structured_data\":%s,\"sanitization\":{\"allowed_schemes\":[\"http\",\"https\",\"mailto\",\"tel\"],\"rejected_uris\":%zu}}", tables, search, forms, videos, media, links, structured, rejected);
    free(qurl); free(qbase); free(qtitle); free(page_title); free(qkind); free(qevidence); free(qselected); free(content); free(tables); free(search); free(forms); free(videos); free(media); free(links); free(structured); free(shape.kind); free(shape.evidence); free_readability(&readability); lxb_html_document_destroy(document); return dbuf_take(&out);
}
