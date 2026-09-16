#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/serialize.h>

#include <dirent.h>

typedef struct {
    lxb_dom_node_t **items;
    size_t length;
    size_t capacity;
} NodeVec;

typedef struct {
    char **items;
    size_t length;
    size_t capacity;
} StringVec;

static void string_vec_push(StringVec *vec, char *value)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = value;
}

static void string_vec_free(StringVec *vec)
{
    for (size_t i = 0; i < vec->length; ++i) free(vec->items[i]);
    free(vec->items);
    *vec = (StringVec){0};
}

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
    size_t length = strlen(raw);
    for (size_t i = 0; i < length;) {
        size_t whitespace = 0;
        const unsigned char *current = (const unsigned char *)raw + i;
        if (d_isspace((char)*current)) whitespace = 1;
        else if (i + 1 < length && current[0] == 0xc2 && current[1] == 0xa0) whitespace = 2;
        else if (i + 2 < length && current[0] == 0xe2 && current[1] == 0x80 &&
                 ((current[2] >= 0x80 && current[2] <= 0x8a) || current[2] == 0xaf)) whitespace = 3;
        else if (i + 2 < length && current[0] == 0xe3 && current[1] == 0x80 && current[2] == 0x80) whitespace = 3;
        if (whitespace) { if (out.length) space = true; i += whitespace; continue; }
        if (space && out.length) dbuf_puts(&out, " ");
        space = false;
        dbuf_append(&out, raw + i, 1);
        ++i;
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

/* Some publishers put a branded or currently-updated headline in og:title,
   while the document's h1 names the article region that should be kept. A
   profile can opt into the visible headline without teaching the matcher a
   site-specific rule. */
static char *profile_title_value(lxb_dom_node_t *root, const char *source)
{
    if (source && d_ieq(source, "document-h1")) {
        /* Prefer the heading inside <main>; many publishers use a short site
           masthead h1 before the article's actual h1. */
        NodeVec mains = find_selector(root, "main");
        for (size_t i = 0; i < mains.length; ++i) {
            NodeVec main_headings = find_selector(mains.items[i], "h1");
            for (size_t h = 0; h < main_headings.length; ++h) {
                char *text = compact_text(main_headings.items[h]);
                if (*text) {
                    free(main_headings.items); free(mains.items); return text;
                }
                free(text);
            }
            free(main_headings.items);
        }
        free(mains.items);
        NodeVec headings = find_selector(root, "h1");
        for (size_t i = 0; i < headings.length; ++i) {
            char *text = compact_text(headings.items[i]);
            if (*text) { free(headings.items); return text; }
            free(text);
        }
        free(headings.items);
        return d_strdup("");
    }
    return meta_value(root, source && *source ? source : "og:title");
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

/* The tags that carry a block of readable text. Shared so that the page-level
   list and the main-content list cannot drift apart on what counts as one. */
static const char *const kBlockTags[] = {
    "h1", "h2", "h3", "h4", "h5", "h6", "p", "pre", "blockquote", "li", "dt", "dd"
};

static bool block_tag(const char *tag)
{
    for (size_t i = 0; i < sizeof(kBlockTags) / sizeof(kBlockTags[0]); ++i)
        if (d_eq(tag, kBlockTags[i])) return true;
    return false;
}

/* The page's own statement about which part of itself the request asked for.

   A liveblog permalink, a forum thread anchored on one post, an item inside a
   catalogue: the URL names one thing and the document ships the whole stream
   around it. Readability is no help, because it scores the densest container
   and the densest container is the stream -- a DN Direkt permalink extracts
   all fifty posts, of which the requested one is 2.5% of the text. schema.org
   has a vocabulary for exactly this case and publishers that use it hand over
   a CSS selector for the requested item, so read that first. */
typedef struct {
    NodeVec nodes;    /* every element the locator matched, in document order */
    char *source;     /* how it was located */
    char *selector;   /* the selector that matched, when there was one */
    char *title;      /* the item's own headline */
    char *profile;    /* the profile that supplied the locator, when any */
} MainContent;

typedef struct {
    char *name;
    StringVec hosts;
    StringVec paths;
    StringVec containers;
    StringVec headings;
    char *title_source;
} MainContentProfile;

static void free_main_content(MainContent *item)
{
    free(item->nodes.items); free(item->source); free(item->selector); free(item->title);
    free(item->profile);
    *item = (MainContent){0};
}

static void free_main_content_profile(MainContentProfile *profile)
{
    free(profile->name);
    string_vec_free(&profile->hosts);
    string_vec_free(&profile->paths);
    string_vec_free(&profile->containers);
    string_vec_free(&profile->headings);
    free(profile->title_source);
    *profile = (MainContentProfile){0};
}

static const char *after_scheme(const char *url)
{
    if (d_starts(url, "https://")) return url + 8;
    if (d_starts(url, "http://")) return url + 7;
    return url;
}

static bool equal_ignoring_case(const char *a, size_t la, const char *b, size_t lb)
{
    if (la != lb) return false;
    for (size_t i = 0; i < la; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

/* Two spellings of one document. The record carries the canonical URL and the
   reader may have arrived by any equivalent spelling, so the scheme and a
   trailing slash are ignored: a record written http: for a page served https:
   is not a different page.

   The fragment counts only when the record uses one. A stream that gives each
   of its items an anchor is naming different items and the anchor is the whole
   difference between them; a request that merely arrived at some anchor of a
   record which claims no fragment is still at that record. */
static bool same_document_url(const char *record, const char *request)
{
    if (!record || !request) return false;
    const char *a = after_scheme(record), *b = after_scheme(request);
    size_t path_a = strcspn(a, "#"), path_b = strcspn(b, "#");
    size_t trimmed_a = path_a, trimmed_b = path_b;
    while (trimmed_a && a[trimmed_a - 1] == '/') --trimmed_a;
    while (trimmed_b && b[trimmed_b - 1] == '/') --trimmed_b;
    if (!trimmed_a || !equal_ignoring_case(a, trimmed_a, b, trimmed_b)) return false;
    if (!a[path_a]) return true;
    return equal_ignoring_case(a + path_a, strlen(a + path_a), b + path_b, strlen(b + path_b));
}

static char *json_ld_string(Engine *engine, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    char *out = JS_IsString(value) ? js_string(engine->context, value) : d_strdup("");
    JS_FreeValue(engine->context, value);
    return out;
}

/* mainEntityOfPage is written either as a bare URL or as a WebPage object
   carrying @id; url is the older spelling of the same claim. */
static char *json_ld_page_url(Engine *engine, JSValueConst record)
{
    JSValue value = JS_GetPropertyStr(engine->context, record, "mainEntityOfPage");
    char *out = d_strdup("");
    if (JS_IsString(value)) { free(out); out = js_string(engine->context, value); }
    else if (JS_IsObject(value)) { free(out); out = json_ld_string(engine, value, "@id"); }
    JS_FreeValue(engine->context, value);
    if (!*out) { free(out); out = json_ld_string(engine, record, "url"); }
    return out;
}

/* hasPart names the region of the page a record occupies, as one object or an
   array of them. It is the same field publishers use to mark which part of a
   page is outside a paywall, so both uses arrive here. */
static char *json_ld_selector(Engine *engine, JSValueConst record)
{
    JSValue parts = JS_GetPropertyStr(engine->context, record, "hasPart");
    char *out = d_strdup("");
    if (JS_IsArray(parts)) {
        int64_t count = json_array_length_for_context(engine->context, parts);
        for (int64_t i = 0; i < count && !*out; ++i) {
            JSValue part = JS_GetPropertyUint32(engine->context, parts, (uint32_t)i);
            if (JS_IsObject(part)) { free(out); out = json_ld_string(engine, part, "cssSelector"); }
            JS_FreeValue(engine->context, part);
        }
    } else if (JS_IsObject(parts)) {
        free(out); out = json_ld_string(engine, parts, "cssSelector");
    }
    JS_FreeValue(engine->context, parts);
    return out;
}

static bool json_ld_record_matches(Engine *engine, JSValueConst record,
                                   char **selector, char **title)
{
    char *page = json_ld_page_url(engine, record);
    bool match = *page && (same_document_url(page, engine->base_url) ||
                           same_document_url(page, engine->source_url));
    free(page);
    if (!match) return false;
    char *found = json_ld_selector(engine, record);
    if (!*found) { free(found); return false; }
    free(*selector); *selector = found;
    free(*title); *title = json_ld_string(engine, record, "headline");
    if (!**title) { free(*title); *title = json_ld_string(engine, record, "name"); }
    return true;
}

/* Records nest: a @graph holds them, a LiveBlogPosting holds its updates, an
   ItemList holds its entries. Descend through the containers that are known to
   hold records rather than over every key, and cap the depth -- the block is
   page-controlled text. */
static bool json_ld_find_selector(Engine *engine, JSValueConst value,
                                  char **selector, char **title, int depth)
{
    if (depth > 4 || !JS_IsObject(value)) return false;
    if (JS_IsArray(value)) {
        int64_t count = json_array_length_for_context(engine->context, value);
        for (int64_t i = 0; i < count; ++i) {
            JSValue item = JS_GetPropertyUint32(engine->context, value, (uint32_t)i);
            bool found = json_ld_find_selector(engine, item, selector, title, depth + 1);
            JS_FreeValue(engine->context, item);
            if (found) return true;
        }
        return false;
    }
    if (json_ld_record_matches(engine, value, selector, title)) return true;
    const char *containers[] = {"@graph", "liveBlogUpdate", "itemListElement", "mainEntity"};
    for (size_t c = 0; c < sizeof(containers) / sizeof(containers[0]); ++c) {
        JSValue child = JS_GetPropertyStr(engine->context, value, containers[c]);
        bool found = json_ld_find_selector(engine, child, selector, title, depth + 1);
        JS_FreeValue(engine->context, child);
        if (found) return true;
    }
    return false;
}

static void profile_strings_from_json(Engine *engine, JSValueConst object,
                                      const char *name, StringVec *out)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    if (JS_IsString(value)) {
        string_vec_push(out, js_string(engine->context, value));
    } else if (JS_IsArray(value)) {
        int64_t count = json_array_length_for_context(engine->context, value);
        for (int64_t i = 0; i < count; ++i) {
            JSValue item = JS_GetPropertyUint32(engine->context, value, (uint32_t)i);
            if (JS_IsString(item)) string_vec_push(out, js_string(engine->context, item));
            JS_FreeValue(engine->context, item);
        }
    }
    JS_FreeValue(engine->context, value);
}

static bool profile_glob_matches(const char *value, const char *pattern, bool fold_case)
{
    if (!value || !pattern) return false;
    while (*pattern) {
        if (*pattern == '*') {
            ++pattern;
            if (!*pattern) return true;
            for (const char *cursor = value;; ++cursor) {
                if (profile_glob_matches(cursor, pattern, fold_case)) return true;
                if (!*cursor) break;
            }
            return false;
        }
        char actual = *value++, wanted = *pattern++;
        if (fold_case) {
            if (actual >= 'A' && actual <= 'Z') actual = (char)(actual - 'A' + 'a');
            if (wanted >= 'A' && wanted <= 'Z') wanted = (char)(wanted - 'A' + 'a');
        }
        if (actual != wanted) return false;
        if (!actual) return true;
    }
    return *value == 0;
}

static bool profile_value_matches(const char *value, const StringVec *patterns,
                                  bool fold_case)
{
    if (!patterns->length) return true;
    for (size_t i = 0; i < patterns->length; ++i)
        if (profile_glob_matches(value, patterns->items[i], fold_case)) return true;
    return false;
}

static bool profile_matches_url(const MainContentProfile *profile, const char *url)
{
    char *host = d_lower(url_cookie_domain(url));
    char *path = url_path(url);
    bool matches = profile_value_matches(host, &profile->hosts, true) &&
                   profile_value_matches(path, &profile->paths, false);
    free(host);
    free(path);
    return matches;
}

static char *profile_file_stem(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && d_ieq(dot, ".json") ? d_strndup(name, (size_t)(dot - name)) : d_strdup(name);
}

static bool profile_from_json(Engine *engine, JSValueConst value, const char *filename,
                              const char *url, MainContentProfile *out)
{
    if (!JS_IsObject(value)) return false;
    MainContentProfile candidate = {0};
    candidate.name = json_ld_string(engine, value, "name");
    if (!*candidate.name) { free(candidate.name); candidate.name = profile_file_stem(filename); }

    JSValue match = JS_GetPropertyStr(engine->context, value, "match");
    if (JS_IsObject(match)) {
        profile_strings_from_json(engine, match, "hosts", &candidate.hosts);
        profile_strings_from_json(engine, match, "paths", &candidate.paths);
    }
    bool matches = profile_matches_url(&candidate, url);
    JS_FreeValue(engine->context, match);
    if (!matches) { free_main_content_profile(&candidate); return false; }

    JSValue main = JS_GetPropertyStr(engine->context, value, "main_content");
    if (JS_IsObject(main)) {
        profile_strings_from_json(engine, main, "containers", &candidate.containers);
        profile_strings_from_json(engine, main, "headings", &candidate.headings);
        candidate.title_source = json_ld_string(engine, main, "title");
    }
    JS_FreeValue(engine->context, main);
    if (!candidate.containers.length) {
        free_main_content_profile(&candidate);
        return false;
    }
    *out = candidate;
    return true;
}

static char *main_content_profile_directory(void)
{
    const char *configured = getenv("DISTILL_PROFILE_DIR");
    if (configured && *configured) return d_strdup(configured);
    char *source = d_cat2(DISTILL_SOURCE_DIR, "/profiles");
    if (access(source, R_OK) == 0) return source;
    free(source);
#ifdef DISTILL_INSTALL_PROFILE_DIR
    char *installed = d_strdup(DISTILL_INSTALL_PROFILE_DIR);
    if (access(installed, R_OK) == 0) return installed;
    free(installed);
#endif
    return d_strdup("");
}

static bool main_content_profile_load(Engine *engine, const char *url,
                                      MainContentProfile *out)
{
    char *directory = main_content_profile_directory();
    if (!*directory) { free(directory); return false; }
    DIR *handle = opendir(directory);
    if (!handle) { free(directory); return false; }
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(handle))) {
        const char *name = entry->d_name;
        size_t length = strlen(name);
        if (length < 6 || !d_ieq(name + length - 5, ".json")) continue;
        char *path = d_cat3(directory, "/", name);
        size_t body_length = 0;
        char *body = d_read_file(path, &body_length);
        free(path);
        if (!body || !body_length) { free(body); continue; }
        JSValue parsed = JS_ParseJSON(engine->context, body, body_length, name);
        if (JS_IsException(parsed)) {
            JS_FreeValue(engine->context, JS_GetException(engine->context));
        } else {
            found = profile_from_json(engine, parsed, name, url, out);
        }
        JS_FreeValue(engine->context, parsed);
        free(body);
    }
    closedir(handle);
    free(directory);
    return found;
}

static bool main_content_candidate_matches(lxb_dom_node_t *root, lxb_dom_node_t *candidate,
                                            const char *wanted, const StringVec *headings)
{
    if (hidden_node(root, candidate)) return false;
    const char *defaults[] = {"h1", "h2", "h3"};
    const char *const *selectors = headings && headings->length ? (const char *const *)headings->items : defaults;
    size_t count = headings && headings->length ? headings->length : sizeof(defaults) / sizeof(defaults[0]);
    for (size_t h = 0; h < count; ++h) {
        NodeVec found = find_selector(candidate, selectors[h]);
        for (size_t i = 0; i < found.length; ++i) {
            char *text = compact_text(found.items[i]);
            bool match = *text && d_eq(text, wanted);
            free(text);
            if (match) { free(found.items); return true; }
        }
        free(found.items);
    }
    return false;
}

static bool main_content_try_headline(lxb_dom_node_t *root, const StringVec *profile_containers,
                                       const char *const *generic_containers,
                                       size_t generic_count, bool require_multiple,
                                       const char *wanted, const StringVec *headings,
                                       MainContent *item, const char *source,
                                       const char *profile_name)
{
    size_t count = profile_containers ? profile_containers->length : generic_count;
    for (size_t s = 0; s < count; ++s) {
        const char *selector = profile_containers ? profile_containers->items[s] : generic_containers[s];
        NodeVec candidates = find_selector(root, selector);
        if (require_multiple && candidates.length < 2) {
            free(candidates.items);
            continue;
        }
        for (size_t i = 0; i < candidates.length; ++i)
            if (main_content_candidate_matches(root, candidates.items[i], wanted, headings))
                node_vec_push(&item->nodes, candidates.items[i]);
        free(candidates.items);
        if (item->nodes.length) {
            item->source = d_strdup(source);
            item->profile = profile_name ? d_strdup(profile_name) : d_strdup("");
            return true;
        }
    }
    return false;
}

/* The page still says what it is about even when it provides no usable
   selector. og:title or the document h1 names the requested item, and a
   semantic region containing that visible heading is strong evidence for the
   main content. Streams without such a heading fall through to JSON-LD. */
static MainContent main_content_by_headline(lxb_dom_node_t *root,
                                            const MainContentProfile *profile)
{
    MainContent item = {0};
    const StringVec *profile_containers = profile && profile->containers.length ? &profile->containers : NULL;
    if (profile_containers) {
        const char *title_source = profile->title_source && *profile->title_source
                                       ? profile->title_source : "og:title";
        char *wanted = profile_title_value(root, title_source);
        if (*wanted)
            main_content_try_headline(root, profile_containers, NULL, 0, false, wanted,
                                      &profile->headings, &item,
                                      "profile-headline", profile->name);
        if (item.nodes.length) {
            item.selector = d_strdup("");
            item.title = wanted;
            return item;
        }
        free(wanted);
    }

    /* Most article pages expose one semantic region, not a stream of sibling
       candidates. A matching visible headline is strong evidence there, so
       these generic candidates do not need the old multiple-candidate guard. */
    const char *generic_containers[] = {
        "article", "*[role=\"article\"]", "main", "*[role=\"main\"]",
        "section", "*[itemprop=\"articleBody\"]",
        "*[data-selector=\"content-wrapper\"]"
    };
    const char *title_sources[] = {"og:title", "document-h1"};
    for (size_t t = 0; t < sizeof(title_sources) / sizeof(title_sources[0]); ++t) {
        char *wanted = profile_title_value(root, title_sources[t]);
        if (!*wanted) { free(wanted); continue; }
        main_content_try_headline(root, NULL, generic_containers,
                                  sizeof(generic_containers) / sizeof(generic_containers[0]),
                                  false, wanted, NULL, &item, "article-headline", NULL);
        if (item.nodes.length) {
            item.selector = d_strdup("");
            item.title = wanted;
            return item;
        }
        free(wanted);
    }
    return item;
}

static MainContent find_main_content(Engine *engine, lxb_dom_node_t *root,
                                     const MainContentProfile *profile)
{
    MainContent item = {0};
    /* An explicit site profile identifies the requested article region. Give
       that authorisation precedence over page-controlled JSON-LD hasPart
       selectors, which often point only at a paywall or ad wrapper. */
    if (profile && profile->containers.length) {
        item = main_content_by_headline(root, profile);
        if (item.nodes.length) return item;
        free_main_content(&item);
    }
    /* Prefer a visible semantic headline over page-controlled JSON-LD
       hasPart selectors. The latter often point only at a paywall or ad
       wrapper; retaining it as a fallback preserves liveblog extraction. */
    item = main_content_by_headline(root, NULL);
    if (item.nodes.length) return item;
    free_main_content(&item);
    char *selector = d_strdup(""), *title = d_strdup("");
    NodeVec scripts = find_selector(root, "script");
    for (size_t i = 0; i < scripts.length && !*selector; ++i) {
        lxb_dom_element_t *element = lxb_dom_interface_element(scripts.items[i]);
        char *type_raw = attr_value(element, "type");
        char *type = d_lower(type_raw);
        free(type_raw);
        bool ld = d_contains(type, "ld+json");
        free(type);
        if (!ld) continue;
        char *text = node_text(scripts.items[i]);
        char *body = d_trim(text);
        free(text);
        if (*body) {
            JSValue parsed = JS_ParseJSON(engine->context, body, strlen(body), "distill-ld.json");
            if (JS_IsException(parsed)) JS_FreeValue(engine->context, JS_GetException(engine->context));
            else json_ld_find_selector(engine, parsed, &selector, &title, 0);
            JS_FreeValue(engine->context, parsed);
        }
        free(body);
    }
    free(scripts.items);
    /* The selector is page-controlled and may name a region this document does
       not have, or use syntax the matcher does not implement. Either way the
       result is no match. */
    if (*selector) {
        NodeVec matched = find_selector(root, selector);
        for (size_t i = 0; i < matched.length; ++i)
            if (!hidden_node(root, matched.items[i])) node_vec_push(&item.nodes, matched.items[i]);
        free(matched.items);
        if (item.nodes.length) {
            item.source = d_strdup("json-ld-haspart");
            item.selector = selector;
            item.title = title;
            return item;
        }
        free(item.nodes.items);
        item.nodes = (NodeVec){0};
    }
    free(selector); free(title);
    return (MainContent){0};
}

/* A permalinked item commonly appears twice -- pinned at the top of the stream
   and again in its chronological place -- so the locator keeps every match and
   the whole set counts as the main content. That also settles the duplicate:
   both copies leave the remainder together. */
static bool main_content_holds(const MainContent *item, lxb_dom_node_t *node)
{
    if (!item) return false;
    for (lxb_dom_node_t *up = node; up; up = up->parent)
        for (size_t i = 0; i < item->nodes.length; ++i)
            if (item->nodes.items[i] == up) return true;
    return false;
}

/* The page-level block list is grouped by tag, which is harmless for a stream
   of headlines but would separate this item's headline from its paragraphs.
   Walk the subtree in document order instead, and stop at the first block
   element on each branch so that a list item containing a paragraph is one
   block rather than the same words twice. Returns the characters emitted. */
static size_t append_main_blocks(Engine *engine, DBuffer *out, lxb_dom_node_t *root,
                                 lxb_dom_node_t *node, size_t *rejected, bool *first,
                                 bool at_item_root)
{
    if (!node) return 0;
    if (is_element(node)) {
        if (hidden_node(root, node)) return 0;
        char *tag = node_tag(node);
        /* An entry wrapper can itself be a <li>. Do not collapse that wrapper
           into one reader-list-item; its heading and paragraphs are the
           readable blocks the promoted item is meant to preserve. */
        bool entry_list_wrapper = at_item_root && d_eq(tag, "li");
        if (block_tag(tag) && !entry_list_wrapper) {
            size_t chars = 0;
            char *text = compact_text(node);
            if (*text) {
                char *href = block_href(engine, root, node, rejected);
                if (!*first) dbuf_puts(out, ",");
                *first = false;
                char *qt = json_quote(tag), *qx = json_quote(text), *qh = json_quote(href);
                dbuf_printf(out, "{\"type\":%s,\"text\":%s,\"href\":%s}", qt, qx, qh);
                free(qt); free(qx); free(qh); free(href);
                chars = strlen(text);
            }
            free(text); free(tag);
            return chars;
        }
        free(tag);
    }
    size_t chars = 0;
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
        chars += append_main_blocks(engine, out, root, child, rejected, first, false);
    return chars;
}

/* Enough text that the located region is worth promoting. A selector that
   matched a share button or an empty wrapper is a selector we misread, and
   hoisting it would bury the page under a heading with nothing beneath it. */
#define DISTILL_MAIN_CONTENT_FLOOR 160

/* Promotion is for an item inside something larger, and that is a question of
   proportion rather than of whether a locator matched. A region holding most
   of the page's prose IS the page: there is nothing to lift it above, and
   lifting it would take an ordinary article away from Readability, which
   cleans up a body far better than walking its blocks does. Publishers mark
   the free part of a paywalled article with the same hasPart selector, so
   without this an article page would arrive here as often as a stream does. */
static bool main_content_is_a_minority(lxb_dom_node_t *root, const MainContent *item)
{
    if (!item->nodes.length) return false;
    size_t page_prose = visible_text_chars(root, "p");
    size_t item_prose = visible_text_chars(item->nodes.items[0], "p");
    /* The item's repeat appearances are the same item, so they are taken out
       of the page total rather than counted against it. A post that is pinned
       at the top and shown again in place would otherwise look twice as large
       a share of its own stream as it is. */
    for (size_t i = 1; i < item->nodes.length; ++i) {
        size_t copy = visible_text_chars(item->nodes.items[i], "p");
        page_prose = copy < page_prose ? page_prose - copy : 0;
    }
    return item_prose && item_prose * 2 <= page_prose;
}

static void append_blocks(Engine *engine, DBuffer *out, lxb_dom_node_t *root,
                          const MainContent *item, size_t *rejected)
{
    bool first = true; dbuf_puts(out, "[");
    for (size_t t = 0; t < sizeof(kBlockTags) / sizeof(kBlockTags[0]); ++t) {
        NodeVec nodes = find_selector(root, kBlockTags[t]);
        for (size_t i = 0; i < nodes.length; ++i) {
            if (hidden_node(root, nodes.items[i])) continue;
            char *text = compact_text(nodes.items[i]); if (!*text) { free(text); continue; }
            char *href = block_href(engine, root, nodes.items[i], rejected);
            if (!first) dbuf_puts(out, ","); first = false;
            char *qt = json_quote(kBlockTags[t]), *qx = json_quote(text), *qh = json_quote(href);
            /* The page list stays complete -- the shell walks it for keyboard
               navigation -- so a block that was promoted is marked rather than
               removed, and the reader is what decides not to show it twice. */
            dbuf_printf(out, "{\"type\":%s,\"text\":%s,\"href\":%s,\"main\":%s}", qt, qx, qh,
                        main_content_holds(item, nodes.items[i]) ? "true" : "false");
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
                for (size_t l = 0; l < labels.length; ++l) { char *for_id = attr_value(lxb_dom_interface_element(labels.items[l]), "for"); if ((*id && d_eq(id, for_id)) || labels.items[l] == fields.items[f]->parent) { free(label); label = compact_text(labels.items[l]); free(for_id); break; } free(for_id); }
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
    MainContentProfile profile = {0};
    main_content_profile_load(engine, engine->source_url, &profile);
    /* Locate the requested item before anything renders, so the block list can
       say which of its entries have already been shown at the top. */
    MainContent item = find_main_content(engine, root, &profile);
    DBuffer main_blocks; dbuf_init(&main_blocks); dbuf_puts(&main_blocks, "[");
    bool main_first = true; size_t main_chars = 0;
    if (item.nodes.length)
        main_chars = append_main_blocks(engine, &main_blocks, root, item.nodes.items[0], &rejected, &main_first, true);
    dbuf_puts(&main_blocks, "]");
    char *main_blocks_json = dbuf_take(&main_blocks);
    /* An explicit profile authorises promotion of the named region, including
       a full article page. A generic headline match is also strong enough to
       identify a short article or video, while ordinary selector matches keep
       the existing prose-floor and minority-region safeguards. */
    bool profile_selected = profile.name && *profile.name && item.source && d_eq(item.source, "profile-headline");
    /* A profile or matching visible headline may identify a short video or
       paywalled article whose text is below the normal prose floor. */
    /* Image-heavy pages often have a perfectly valid <main><h1>, but their
       media registry is the content the reader must render. Do not let the
       headline promotion hide that shape-specific output. */
    bool headline_selected = item.source && d_eq(item.source, "article-headline") &&
                             !d_eq(shape.kind, "image-heavy");
    bool main_found = item.nodes.length &&
                      ((profile_selected || headline_selected) ? main_chars > 0 :
                       main_chars >= DISTILL_MAIN_CONTENT_FLOOR &&
                       main_content_is_a_minority(root, &item));
    char *content = sanitize_content(readability.content); char *tables = table_json(root); char *search = search_json(engine, root, &rejected); char *forms = forms_json(engine, root, &rejected); char *videos = videos_json(engine, root, &rejected); char *media = media_json(engine, root, &rejected); char *links = links_json(engine, root, &rejected); char *structured = structured_data_json(engine, root);
    const char *selected = "structured-text"; if (d_eq(shape.kind, "article")) selected = readability.ok ? "readability" : "structured-text"; else if (d_eq(shape.kind, "table")) selected = "table"; else if (d_eq(shape.kind, "search-results")) selected = "search-results"; else if (d_eq(shape.kind, "form")) selected = "form"; else if (d_eq(shape.kind, "video")) selected = "video-metadata"; else if (d_eq(shape.kind, "image-heavy")) selected = "media-reference";
    DBuffer out; dbuf_init(&out); char *page_title = document_title(root);
    char *qurl = json_quote(engine->source_url), *qbase = json_quote(engine->base_url), *qkind = json_quote(shape.kind), *qevidence = json_quote(shape.evidence), *qselected = json_quote(selected), *qtitle = json_quote(page_title);
    dbuf_printf(&out, "{\"schema\":1,\"url\":%s,\"base_url\":%s,\"title\":%s,\"shape\":{\"kind\":%s,\"confidence\":%.2f,\"evidence\":%s},\"extractor\":%s,\"extractor_registry\":[\"readability\",\"structured-text\",\"table\",\"search-results\",\"form\",\"video-metadata\",\"media-reference\"],\"readability\":", qurl, qbase, qtitle, qkind, shape.confidence, qevidence, qselected);
    if (!readability.ok) dbuf_puts(&out, "null"); else { char *qt = json_quote(readability.title), *qb = json_quote(readability.byline), *qd = json_quote(readability.dir), *ql = json_quote(readability.lang), *qc = json_quote(content), *qtc = json_quote(readability.text_content), *qe = json_quote(readability.excerpt), *qs = json_quote(readability.site_name), *qp = json_quote(readability.published_time); dbuf_printf(&out, "{\"title\":%s,\"byline\":%s,\"dir\":%s,\"lang\":%s,\"content\":%s,\"textContent\":%s,\"excerpt\":%s,\"siteName\":%s,\"publishedTime\":%s}", qt, readability.byline_null ? "null" : qb, readability.dir_null ? "null" : qd, readability.lang_null ? "null" : ql, qc, qtc, readability.excerpt_null ? "null" : qe, readability.site_name_null ? "null" : qs, readability.published_time_null ? "null" : qp); free(qt); free(qb); free(qd); free(ql); free(qc); free(qtc); free(qe); free(qs); free(qp); }
    char *qsource = json_quote(main_found ? item.source : "");
    char *qselector = json_quote(main_found ? item.selector : "");
    char *qmain_title = json_quote(main_found ? item.title : "");
    char *qprofile = json_quote(main_found ? item.profile : "");
    dbuf_printf(&out, ",\"main_content\":{\"found\":%s,\"source\":%s,\"profile\":%s,\"selector\":%s,\"title\":%s,\"matches\":%zu,\"blocks\":%s}",
                main_found ? "true" : "false", qsource, qprofile, qselector, qmain_title,
                main_found ? item.nodes.length : (size_t)0, main_found ? main_blocks_json : "[]");
    free(qsource); free(qprofile); free(qselector); free(qmain_title);
    dbuf_puts(&out, ",\"blocks\":"); append_blocks(engine, &out, root, main_found ? &item : NULL, &rejected); dbuf_printf(&out, ",\"tables\":%s,\"search_results\":%s,\"forms\":%s,\"videos\":%s,\"media\":%s,\"links\":%s,\"structured_data\":%s,\"sanitization\":{\"allowed_schemes\":[\"http\",\"https\",\"mailto\",\"tel\"],\"rejected_uris\":%zu}}", tables, search, forms, videos, media, links, structured, rejected);
    free(qurl); free(qbase); free(qtitle); free(page_title); free(qkind); free(qevidence); free(qselected); free(content); free(tables); free(search); free(forms); free(videos); free(media); free(links); free(structured); free(shape.kind); free(shape.evidence); free(main_blocks_json); free_main_content(&item); free_main_content_profile(&profile); free_readability(&readability); lxb_html_document_destroy(document); return dbuf_take(&out);
}
