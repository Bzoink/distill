/* The reader page laid out and painted through the page interface.

   Distill lays out only its own pages -- the reader template and the media
   viewer -- so this is not a browser's layout engine. It is the part of one
   those pages use, read from the same place a browser would read it: the
   page's own <style>, cascaded over a default stylesheet, so the page, its
   HTML export and its rendering cannot disagree about how it looks.

   What it implements, and nothing more:
     cascade   type, class, id and attribute selectors, :root and :link,
               descendant and child combinators, specificity and order,
               inheritance, var() from :root, px, em and % lengths
     boxes     block, inline, inline-block, list-item, table and its parts,
               replaced images, <br>; margins, borders, padding, box-sizing,
               width, height, min- and max-width, auto margins
     flow      block layout with margin collapsing; inline layout a word at a
               time, with the reference's measured breaks;
               white-space normal, pre and pre-wrap; text-align; baseline and
               middle alignment
     tables    automatic column widths, collapsed borders
     paint     backgrounds, borders, text with its decoration, images, list
               markers, overflow clipping -- through render.h
   It keeps the reference conventions where they decide where a pixel goes:
   whole-pixel lengths, ems rounded to the nearest pixel, a word's width
   measured whole, a line's height from its text and half-leading.

   The layout runs in the long-lived shell, which on Muroxi may rely on
   64 KiB of stack only. Recursion follows the document's nesting and stops
   at LAYOUT_DEPTH_MAX; deeper content is not laid out, and is refused
   rather than overflowing. No frame holds a large buffer. */

#include "page.h"

#include "cutil.h"
#include "image.h"

#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/element.h>

#include <math.h>

#define LAYOUT_DEPTH_MAX 48
#define TEXT_RUNS_MAX 16384
#define DEFAULT_FONT_SIZE 18
#define DEFAULT_FONT_FAMILY "sans"

/* ---- values ------------------------------------------------------------- */

typedef enum { U_NONE, U_AUTO, U_PX, U_EM, U_PERCENT, U_NUMBER } Unit;

typedef struct {
    float value;
    uint8_t unit;
} Len;

static const Len LEN_NONE = {0, U_NONE};
static const Len LEN_AUTO = {0, U_AUTO};

static inline int round_float(float value)
{
    return (int)lroundf(value);
}

/* A length in pixels: ems against the font size, percentages against
   `base`, as the reference resolved them. */
static int len_px(Len len, int font_size, int base)
{
    switch (len.unit) {
    case U_PX: return (int)len.value;
    case U_EM: return round_float(len.value * (float)font_size);
    case U_PERCENT: return (int)((float)base * len.value / 100.0f);
    case U_NUMBER: return (int)len.value;
    default: return 0;
    }
}

typedef enum {
    DISPLAY_NONE, DISPLAY_INLINE, DISPLAY_BLOCK, DISPLAY_INLINE_BLOCK, DISPLAY_LIST_ITEM,
    DISPLAY_TABLE, DISPLAY_ROW_GROUP, DISPLAY_ROW, DISPLAY_CELL, DISPLAY_CAPTION,
} Display;

typedef enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE } WhiteSpace;
typedef enum { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_JUSTIFY } TextAlign;
typedef enum { VA_BASELINE, VA_MIDDLE, VA_SUB, VA_SUPER, VA_TOP, VA_BOTTOM, VA_TEXT_TOP, VA_TEXT_BOTTOM } VerticalAlign;
typedef enum { LIST_NONE, LIST_DISC, LIST_CIRCLE, LIST_SQUARE, LIST_DECIMAL } ListStyle;
enum { EDGE_TOP, EDGE_RIGHT, EDGE_BOTTOM, EDGE_LEFT };
enum { DECORATION_UNDERLINE = 0x01u, DECORATION_LINE_THROUGH = 0x02u, DECORATION_OVERLINE = 0x04u };

typedef struct {
    int height, ascent, descent, x_height;
} Metrics;

typedef struct Style Style;
struct Style {
    uint8_t display, white_space, text_align, vertical_align, list_style;
    uint8_t font_style_italic, decoration, overflow_hidden, border_box, collapse;
    uint8_t visible, text_transform, background_repeat;
    int font_weight;
    int font_size;
    Len line_height_css;           /* as specified, inherited as specified */
    int line_height;               /* computed, px */
    Len margin[4], padding[4];
    Len width, height, min_width, max_width, min_height, max_height;
    Len border_width_css[4];
    int border_width[4];           /* px; 0 when the style is none */
    bool border_solid[4];
    DRenderColor border_color[4];
    bool border_color_set[4];
    DRenderColor color;
    DRenderColor background;
    const char *background_image;  /* arena */
    Len background_size[2];
    int border_spacing;
    const char *font_family;       /* arena */
    DRenderFont *font;
    Metrics metrics;
};

/* ---- the arena: everything a page allocates lives until the page goes ---- */

typedef struct Chunk {
    struct Chunk *next;
    size_t used, size;
    alignas(max_align_t) unsigned char data[];
} Chunk;

typedef struct {
    Chunk *head;
} Arena;

static void *arena_alloc(Arena *arena, size_t size)
{
    size = (size + alignof(max_align_t) - 1) & ~(alignof(max_align_t) - 1);
    if (!arena->head || arena->head->used + size > arena->head->size) {
        size_t chunk = size > 65536 ? size : 65536;
        Chunk *next = d_xmalloc(sizeof(Chunk) + chunk);
        next->next = arena->head;
        next->used = 0;
        next->size = chunk;
        arena->head = next;
    }
    void *out = arena->head->data + arena->head->used;
    arena->head->used += size;
    memset(out, 0, size);
    return out;
}

static char *arena_strndup(Arena *arena, const char *text, size_t length)
{
    char *out = arena_alloc(arena, length + 1);
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}

static void arena_free(Arena *arena)
{
    for (Chunk *chunk = arena->head; chunk;) {
        Chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
}

/* ---- the default stylesheet ----------------------------------------------

   What a page gets before its own <style>: the defaults captured from the
   reference stylesheet, reduced to what the cascade here understands. The
   reader page overrides most of it;
   what it does not override -- list indents, table font size, heading
   weights -- still decides how the page looks, so it must stay as it was. */

static const char DEFAULT_CSS[] =
    "html{display:block;}head,meta,title,link,style,script,template,option,datalist{display:none;}"
    "body{display:block;margin:8px;}"
    "p{display:block;margin-top:1em;margin-bottom:1em;}"
    "b,strong{display:inline;font-weight:bold;}i,em,cite,var,dfn{display:inline;font-style:italic;}"
    "ins,u{text-decoration:underline;}del,s,strike{text-decoration:line-through;}"
    "center{text-align:center;display:block;}"
    /* The default-sheet link rule is deliberately no more specific than an
       authored `a` rule. The reference treats the UA colour as a fallback,
       so component classes such as `.button` can override it. */
    "a{text-decoration:underline;color:#00f;}"
    "h1,h2,h3,h4,h5,h6,div{display:block;}"
    "h1{font-weight:bold;margin-top:.67em;margin-bottom:.67em;font-size:2em;}"
    "h2{font-weight:bold;margin-top:.83em;margin-bottom:.83em;font-size:1.5em;}"
    "h3{font-weight:bold;margin-top:1em;margin-bottom:1em;font-size:1.17em;}"
    "h4{font-weight:bold;margin-top:1.33em;margin-bottom:1.33em;}"
    "h5{font-weight:bold;margin-top:1.67em;margin-bottom:1.67em;font-size:.83em;}"
    "h6{font-weight:bold;margin-top:2.33em;margin-bottom:2.33em;font-size:.67em;}"
    "br{display:inline-block;}span{display:inline;}img{display:inline-block;}"
    "hr{display:block;margin-top:.5em;margin-bottom:.5em;margin-left:auto;margin-right:auto;"
    "border-style:solid;border-width:1px;border-color:gray;}"
    "table{display:table;border-spacing:2px;font-size:medium;font-weight:normal;font-style:normal;}"
    "tbody,tfoot,thead{display:table-row-group;}tr{display:table-row;}"
    "td,th{display:table-cell;border-width:1px;padding:1px;}th{font-weight:bold;}"
    "caption{display:table-caption;}"
    "tt,code,kbd,samp{font-family:monospace;}"
    "pre,xmp,plaintext,listing{display:block;font-family:monospace;white-space:pre;margin:1em 0;}"
    "ul,menu,dir{display:block;list-style-type:disc;margin-top:1em;margin-bottom:1em;margin-left:0;"
    "margin-right:0;padding-left:40px;}"
    "ol{display:block;list-style-type:decimal;margin-top:1em;margin-bottom:1em;margin-left:0;"
    "margin-right:0;padding-left:40px;}"
    "li{display:list-item;}"
    "ul ul,ol ul{list-style-type:circle;}"
    "ol ol ul,ol ul ul,ul ol ul,ul ul ul{list-style-type:square;}"
    "dd{display:block;margin-left:40px;}dl{display:block;margin-top:1em;margin-bottom:1em;}"
    "dt{display:block;}"
    "ol ul,ul ol,ul ul,ol ol{margin-top:0;margin-bottom:0;}"
    "blockquote{display:block;margin-top:1em;margin-bottom:1em;margin-left:40px;margin-right:40px;}"
    "form{display:block;margin-top:0;}"
    "input,textarea,select,button{margin:0;color:initial;line-height:normal;display:inline-block;}"
    "input[type=\"hidden\"]{display:none;}"
    "article,aside,footer,header,hgroup,nav,section,main,figcaption,address,details,summary,fieldset,legend{display:block;}"
    "sub{vertical-align:sub;font-size:smaller;}sup{vertical-align:super;font-size:smaller;}"
    "figure{display:block;margin-top:1em;margin-bottom:1em;margin-left:40px;margin-right:40px;}";

/* ---- selectors and rules ------------------------------------------------ */

typedef enum { SEL_TAG, SEL_CLASS, SEL_ID, SEL_ATTR, SEL_ATTR_EQUALS, SEL_ROOT, SEL_LINK } SelectorPart;

typedef struct {
    uint8_t kind;
    const char *name;     /* tag, class, id, attribute */
    const char *value;    /* attribute value for SEL_ATTR_EQUALS */
} SimplePart;

/* A compound selector, and how it relates to the one before it (to its
   left): 0 for the rightmost, ' ' descendant, '>' child. */
typedef struct {
    SimplePart parts[6];
    int part_count;
    char combinator;
} Compound;

typedef struct {
    Compound compounds[8];
    int compound_count;
    int specificity;
} Selector;

typedef struct {
    const char *name;     /* property, lowercase */
    const char *value;    /* as written, trimmed */
} Declaration;

typedef struct {
    Selector selector;
    Declaration *declarations;
    int declaration_count;
    int order;
} Rule;

typedef struct {
    Rule *rules;
    size_t count, capacity;
    Declaration *variables;   /* custom properties from :root */
    size_t variable_count, variable_capacity;
    int order;
} Sheet;

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || (unsigned char)c >= 0x80;
}

static char *lower_dup(Arena *arena, const char *text, size_t length)
{
    char *out = arena_strndup(arena, text, length);
    for (char *p = out; *p; ++p)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return out;
}

/* One selector from [begin, end). False for anything this cascade does not
   understand, which then matches nothing -- the safe failure. */
static bool parse_selector(Arena *arena, const char *begin, const char *end, Selector *out)
{
    *out = (Selector){0};
    const char *p = begin;
    char pending = 0;
    int ids = 0, classes = 0, tags = 0;
    while (p < end) {
        while (p < end && d_isspace(*p)) { ++p; if (out->compound_count) pending = pending ? pending : ' '; }
        if (p >= end) break;
        if (*p == '>') { pending = '>'; ++p; continue; }
        if (out->compound_count == 8) return false;
        Compound *compound = &out->compounds[out->compound_count++];
        compound->combinator = out->compound_count == 1 ? 0 : (pending ? pending : ' ');
        pending = 0;
        while (p < end && !d_isspace(*p) && *p != '>') {
            if (compound->part_count == 6) return false;
            SimplePart *part = &compound->parts[compound->part_count];
            if (*p == '*') { ++p; continue; }
            if (*p == '.' || *p == '#') {
                char kind = *p++;
                const char *start = p;
                while (p < end && is_name_char(*p)) ++p;
                if (p == start) return false;
                *part = (SimplePart){kind == '.' ? SEL_CLASS : SEL_ID, arena_strndup(arena, start, (size_t)(p - start)), NULL};
                if (kind == '.') ++classes; else ++ids;
            } else if (*p == '[') {
                const char *close = memchr(p, ']', (size_t)(end - p));
                if (!close) return false;
                const char *name = p + 1, *equals = memchr(name, '=', (size_t)(close - name));
                if (equals && equals > name && (equals[-1] == '|' || equals[-1] == '~' || equals[-1] == '^' ||
                                                equals[-1] == '$' || equals[-1] == '*'))
                    return false;
                const char *name_end = equals ? equals : close;
                const char *value = equals ? equals + 1 : NULL;
                size_t value_length = value ? (size_t)(close - value) : 0;
                if (value && value_length >= 2 && (value[0] == '"' || value[0] == '\'')) { ++value; value_length -= 2; }
                *part = (SimplePart){equals ? SEL_ATTR_EQUALS : SEL_ATTR,
                                     lower_dup(arena, name, (size_t)(name_end - name)),
                                     value ? arena_strndup(arena, value, value_length) : NULL};
                ++classes;
                p = close + 1;
            } else if (*p == ':') {
                const char *start = ++p;
                while (p < end && is_name_char(*p)) ++p;
                size_t length = (size_t)(p - start);
                if (length == 4 && !d_ncasecmp(start, "root", 4)) *part = (SimplePart){SEL_ROOT, NULL, NULL};
                else if (length == 4 && !d_ncasecmp(start, "link", 4)) *part = (SimplePart){SEL_LINK, NULL, NULL};
                else return false;
                ++classes;
            } else if (is_name_char(*p)) {
                const char *start = p;
                while (p < end && is_name_char(*p)) ++p;
                *part = (SimplePart){SEL_TAG, lower_dup(arena, start, (size_t)(p - start)), NULL};
                ++tags;
            } else {
                return false;
            }
            ++compound->part_count;
        }
    }
    if (!out->compound_count) return false;
    out->specificity = ids * 10000 + classes * 100 + tags;
    return true;
}

static const char *trim_span(const char **begin, const char *end)
{
    while (*begin < end && d_isspace(**begin)) ++*begin;
    while (end > *begin && d_isspace(end[-1])) --end;
    return end;
}

static void push_rule(Sheet *sheet, Rule rule)
{
    d_vec_reserve((void **)&sheet->rules, &sheet->capacity, sheet->count, sizeof(*sheet->rules),
                  sheet->count + 1);
    sheet->rules[sheet->count++] = rule;
}

static void push_variable(Sheet *sheet, Declaration declaration)
{
    for (size_t i = 0; i < sheet->variable_count; ++i)
        if (!strcmp(sheet->variables[i].name, declaration.name)) { sheet->variables[i] = declaration; return; }
    d_vec_reserve((void **)&sheet->variables, &sheet->variable_capacity, sheet->variable_count,
                  sizeof(*sheet->variables), sheet->variable_count + 1);
    sheet->variables[sheet->variable_count++] = declaration;
}

/* A stylesheet into rules. Comments, @-rules and anything unparsable are
   skipped: this reads Distill's own stylesheet and its defaults, and a rule
   it cannot read must cost that rule only. */
static void parse_css(Arena *arena, Sheet *sheet, const char *css)
{
    const char *p = css;
    while (*p) {
        while (*p && d_isspace(*p)) ++p;
        if (!*p) break;
        if (p[0] == '/' && p[1] == '*') {
            const char *close = strstr(p + 2, "*/");
            p = close ? close + 2 : p + strlen(p);
            continue;
        }
        const char *open = strchr(p, '{');
        if (!open) break;
        if (*p == '@') {
            /* Skip the block, nested braces included. */
            int depth = 0;
            const char *q = open;
            for (; *q; ++q) {
                if (*q == '{') ++depth;
                else if (*q == '}' && --depth == 0) break;
            }
            p = *q ? q + 1 : q;
            continue;
        }
        const char *close = strchr(open, '}');
        if (!close) break;
        /* Declarations, split on ';' outside parentheses and quotes. */
        Declaration *declarations = NULL;
        size_t count = 0, capacity = 0;
        const char *d = open + 1;
        while (d < close) {
            const char *stop = d;
            int paren = 0;
            char quote = 0;
            for (; stop < close; ++stop) {
                if (quote) { if (*stop == quote) quote = 0; continue; }
                if (*stop == '"' || *stop == '\'') quote = *stop;
                else if (*stop == '(') ++paren;
                else if (*stop == ')') --paren;
                else if (*stop == ';' && paren <= 0) break;
            }
            const char *colon = memchr(d, ':', (size_t)(stop - d));
            if (colon) {
                const char *name = d, *name_end = trim_span(&name, colon);
                const char *value = colon + 1, *value_end = trim_span(&value, stop);
                const char *important = value_end - value >= 10 ? value_end - 10 : NULL;
                if (important && !d_ncasecmp(important, "!important", 10)) {
                    value_end = important;
                    value_end = trim_span(&value, value_end);
                }
                if (name_end > name && value_end >= value) {
                    d_vec_reserve((void **)&declarations, &capacity, count, sizeof(*declarations), count + 1);
                    declarations[count++] = (Declaration){lower_dup(arena, name, (size_t)(name_end - name)),
                                                          arena_strndup(arena, value, (size_t)(value_end - value))};
                }
            }
            d = stop + 1;
        }
        Declaration *kept = count ? arena_alloc(arena, count * sizeof(*kept)) : NULL;
        if (count) memcpy(kept, declarations, count * sizeof(*kept));
        free(declarations);
        /* One rule per selector in the list. */
        const char *s = p;
        while (s < open) {
            const char *comma = s;
            while (comma < open && *comma != ',') ++comma;
            const char *begin = s, *end = trim_span(&begin, comma);
            Rule rule = {.declarations = kept, .declaration_count = (int)count, .order = sheet->order++};
            if (end > begin && parse_selector(arena, begin, end, &rule.selector)) {
                bool root = rule.selector.compound_count == 1 && rule.selector.compounds[0].part_count == 1 &&
                            rule.selector.compounds[0].parts[0].kind == SEL_ROOT;
                if (root)
                    for (size_t i = 0; i < count; ++i)
                        if (kept[i].name[0] == '-' && kept[i].name[1] == '-') push_variable(sheet, kept[i]);
                push_rule(sheet, rule);
            }
            s = comma + 1;
        }
        p = close + 1;
    }
}

/* ---- matching ----------------------------------------------------------- */

static const char *element_tag(lxb_dom_element_t *element)
{
    size_t length = 0;
    const lxb_char_t *name = lxb_dom_element_local_name(element, &length);
    return name ? (const char *)name : "";
}

static const char *element_attr(lxb_dom_element_t *element, const char *name)
{
    size_t length = 0;
    const lxb_char_t *value = lxb_dom_element_get_attribute(element, (const lxb_char_t *)name,
                                                             strlen(name), &length);
    return value ? (const char *)value : NULL;
}

static bool has_class(lxb_dom_element_t *element, const char *name)
{
    const char *classes = element_attr(element, "class");
    if (!classes) return false;
    size_t length = strlen(name);
    for (const char *p = classes; *p;) {
        while (*p && d_isspace(*p)) ++p;
        const char *start = p;
        while (*p && !d_isspace(*p)) ++p;
        if ((size_t)(p - start) == length && !memcmp(start, name, length)) return true;
    }
    return false;
}

static lxb_dom_element_t *parent_element(lxb_dom_element_t *element)
{
    lxb_dom_node_t *parent = lxb_dom_interface_node(element)->parent;
    return parent && parent->type == LXB_DOM_NODE_TYPE_ELEMENT ? lxb_dom_interface_element(parent) : NULL;
}

static bool match_compound(lxb_dom_element_t *element, const Compound *compound)
{
    for (int i = 0; i < compound->part_count; ++i) {
        const SimplePart *part = &compound->parts[i];
        switch (part->kind) {
        case SEL_TAG:
            if (d_casecmp(element_tag(element), part->name)) return false;
            break;
        case SEL_CLASS:
            if (!has_class(element, part->name)) return false;
            break;
        case SEL_ID: {
            const char *id = element_attr(element, "id");
            if (!id || strcmp(id, part->name)) return false;
            break;
        }
        case SEL_ATTR:
            if (!element_attr(element, part->name)) return false;
            break;
        case SEL_ATTR_EQUALS: {
            const char *value = element_attr(element, part->name);
            if (!value || d_casecmp(value, part->value)) return false;
            break;
        }
        case SEL_ROOT:
            if (parent_element(element)) return false;
            break;
        case SEL_LINK:
            if (d_casecmp(element_tag(element), "a") || !element_attr(element, "href")) return false;
            break;
        }
    }
    return true;
}

/* Right to left: compound `index` against `element`, then what lies to its
   left against ancestors. Bounded by the selector's length (8). */
static bool match_from(lxb_dom_element_t *element, const Selector *selector, int index)
{
    if (!match_compound(element, &selector->compounds[index])) return false;
    if (index == 0) return true;
    char combinator = selector->compounds[index].combinator;
    for (lxb_dom_element_t *up = parent_element(element); up; up = parent_element(up)) {
        if (match_from(up, selector, index - 1)) return true;
        if (combinator == '>') return false;
    }
    return false;
}

static bool selector_matches(lxb_dom_element_t *element, const Selector *selector)
{
    return match_from(element, selector, selector->compound_count - 1);
}

/* ---- values from text --------------------------------------------------- */

static bool word_is(const char *value, const char *word)
{
    return !d_casecmp(value, word);
}

static bool parse_len(const char *value, Len *out)
{
    while (d_isspace(*value)) ++value;
    if (word_is(value, "auto")) { *out = LEN_AUTO; return true; }
    if (word_is(value, "none")) { *out = LEN_NONE; return true; }
    char *end = NULL;
    float number = strtof(value, &end);
    if (end == value) return false;
    while (d_isspace(*end)) ++end;
    if (!*end) *out = (Len){number, number == 0 ? U_PX : U_NUMBER};
    else if (!d_ncasecmp(end, "px", 2)) *out = (Len){number, U_PX};
    else if (!d_ncasecmp(end, "em", 2)) *out = (Len){number, U_EM};
    else if (!d_ncasecmp(end, "rem", 3)) *out = (Len){number * DEFAULT_FONT_SIZE, U_PX};
    else if (*end == '%') *out = (Len){number, U_PERCENT};
    else if (!d_ncasecmp(end, "pt", 2)) *out = (Len){number, U_PX};
    else return false;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *value, DRenderColor *out)
{
    while (d_isspace(*value)) ++value;
    if (value[0] == '#') {
        size_t length = 0;
        while (hex_digit(value[1 + length]) >= 0) ++length;
        if (length == 3 || length == 4) {
            int c[4] = {15, 15, 15, 15};
            for (size_t i = 0; i < length; ++i) c[i] = hex_digit(value[1 + i]);
            *out = (DRenderColor){(uint8_t)(c[0] * 17), (uint8_t)(c[1] * 17), (uint8_t)(c[2] * 17), (uint8_t)(c[3] * 17)};
            return true;
        }
        if (length == 6 || length == 8) {
            int c[4] = {0, 0, 0, 255};
            for (size_t i = 0; i < length / 2; ++i)
                c[i] = hex_digit(value[1 + i * 2]) * 16 + hex_digit(value[2 + i * 2]);
            *out = (DRenderColor){(uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], (uint8_t)c[3]};
            return true;
        }
        return false;
    }
    if (!d_ncasecmp(value, "rgb", 3)) {
        const char *open = strchr(value, '(');
        if (!open) return false;
        float c[4] = {0, 0, 0, 1};
        const char *p = open + 1;
        for (int i = 0; i < 4; ++i) {
            char *end = NULL;
            c[i] = strtof(p, &end);
            if (end == p) { if (i < 3) return false; c[3] = 1; break; }
            p = end;
            if (*p == '%') { c[i] = i < 3 ? c[i] * 2.55f : c[i] / 100.0f; ++p; }
            while (*p == ' ' || *p == ',' || *p == '/') ++p;
            if (*p == ')') { if (i < 3) return false; break; }
        }
        *out = (DRenderColor){(uint8_t)lroundf(fminf(fmaxf(c[0], 0), 255)), (uint8_t)lroundf(fminf(fmaxf(c[1], 0), 255)),
                              (uint8_t)lroundf(fminf(fmaxf(c[2], 0), 255)), (uint8_t)lroundf(fminf(fmaxf(c[3], 0), 1) * 255)};
        return true;
    }
    static const struct { const char *name; uint32_t rgb; } named[] = {
        {"black", 0x000000}, {"white", 0xffffff}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xc0c0c0}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"navy", 0x000080}, {"maroon", 0x800000}, {"purple", 0x800080},
        {"teal", 0x008080}, {"olive", 0x808000}, {"lime", 0x00ff00}, {"aqua", 0x00ffff},
        {"fuchsia", 0xff00ff}, {"orange", 0xffa500},
    };
    if (word_is(value, "transparent")) { *out = (DRenderColor){0, 0, 0, 0}; return true; }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i)
        if (word_is(value, named[i].name)) {
            *out = (DRenderColor){(uint8_t)(named[i].rgb >> 16), (uint8_t)(named[i].rgb >> 8), (uint8_t)named[i].rgb, 255};
            return true;
        }
    return false;
}

/* The next whitespace-separated token of a shorthand, parentheses kept
   whole: "1px solid rgba(0, 0, 0, .5)" is three. */
static bool next_token(const char **cursor, char *out, size_t capacity)
{
    const char *p = *cursor;
    while (d_isspace(*p)) ++p;
    if (!*p) return false;
    size_t length = 0;
    int paren = 0;
    while (*p && (paren || !d_isspace(*p))) {
        if (*p == '(') ++paren;
        else if (*p == ')') --paren;
        if (length + 1 < capacity) out[length++] = *p;
        ++p;
    }
    out[length] = '\0';
    *cursor = p;
    return true;
}

/* ---- the cascade -------------------------------------------------------- */

typedef struct {
    Arena arena;
    Sheet sheet;
    DRenderContext *renderer;      /* for fonts and measurement */
    int viewport_width;
    /* Fonts by what makes them different. */
    struct FontEntry { const char *family; int size, weight; bool italic; unsigned decoration;
                       DRenderFont *font; Metrics metrics; } *fonts;
    size_t font_count, font_capacity;
} Cascade;

/* var() references resolved from :root, into `out`. */
static void substitute_vars(const Sheet *sheet, const char *value, char *out, size_t capacity)
{
    size_t used = 0;
    for (const char *p = value; *p && used + 1 < capacity;) {
        if (!d_ncasecmp(p, "var(", 4)) {
            const char *name = p + 4;
            while (d_isspace(*name)) ++name;
            const char *end = name;
            while (*end && *end != ')' && *end != ',' && !d_isspace(*end)) ++end;
            const char *close = strchr(end, ')');
            const char *found = NULL;
            for (size_t i = 0; i < sheet->variable_count; ++i)
                if (strlen(sheet->variables[i].name) == (size_t)(end - name) &&
                    !memcmp(sheet->variables[i].name, name, (size_t)(end - name)))
                    found = sheet->variables[i].value;
            if (!found && *end == ',') {
                const char *fallback = end + 1;
                while (d_isspace(*fallback)) ++fallback;
                size_t length = close ? (size_t)(close - fallback) : strlen(fallback);
                for (size_t i = 0; i < length && used + 1 < capacity; ++i) out[used++] = fallback[i];
            }
            for (const char *f = found ? found : ""; *f && used + 1 < capacity; ++f) out[used++] = *f;
            p = close ? close + 1 : p + strlen(p);
            continue;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';
}

static void set_edges(Len edges[4], const char *value)
{
    Len values[4];
    int count = 0;
    char token[64];
    const char *cursor = value;
    while (count < 4 && next_token(&cursor, token, sizeof(token)))
        if (parse_len(token, &values[count])) ++count;
    if (count == 0) return;
    if (count == 1) values[1] = values[2] = values[3] = values[0];
    else if (count == 2) { values[2] = values[0]; values[3] = values[1]; }
    else if (count == 3) values[3] = values[1];
    for (int i = 0; i < 4; ++i) edges[i] = values[i];
}

static bool parse_border_width(const char *token, Len *out)
{
    if (word_is(token, "thin")) { *out = (Len){1, U_PX}; return true; }
    if (word_is(token, "medium")) { *out = (Len){3, U_PX}; return true; }
    if (word_is(token, "thick")) { *out = (Len){5, U_PX}; return true; }
    return parse_len(token, out) && out->unit != U_AUTO;
}

static bool border_style_word(const char *token, bool *solid)
{
    static const char *drawn[] = {"solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset"};
    if (word_is(token, "none") || word_is(token, "hidden")) { *solid = false; return true; }
    for (size_t i = 0; i < sizeof(drawn) / sizeof(drawn[0]); ++i)
        if (word_is(token, drawn[i])) { *solid = true; return true; }
    return false;
}

/* "1px solid #ccc" onto the edges selected by `mask` (bit per edge). */
static void set_border(Style *style, unsigned mask, const char *value)
{
    char token[96];
    const char *cursor = value;
    Len width = {3, U_PX};
    bool solid = false, have_width = false, have_style = false, have_color = false;
    DRenderColor color = {0, 0, 0, 0};
    while (next_token(&cursor, token, sizeof(token))) {
        if (!have_style && border_style_word(token, &solid)) have_style = true;
        else if (!have_width && parse_border_width(token, &width)) have_width = true;
        else if (!have_color && parse_color(token, &color)) have_color = true;
    }
    for (int edge = 0; edge < 4; ++edge) {
        if (!(mask & (1u << edge))) continue;
        style->border_width_css[edge] = width;
        style->border_solid[edge] = solid;
        style->border_color_set[edge] = have_color;
        if (have_color) style->border_color[edge] = color;
    }
}

static int font_weight_value(const char *value, int parent)
{
    if (word_is(value, "normal")) return 400;
    if (word_is(value, "bold")) return 700;
    if (word_is(value, "bolder")) return parent < 600 ? 700 : 900;
    if (word_is(value, "lighter")) return parent > 500 ? 400 : 100;
    int number = (int)strtol(value, NULL, 10);
    return number > 0 ? number : parent;
}

static int font_size_value(const char *value, int parent)
{
    static const struct { const char *name; int numerator, denominator; } keywords[] = {
        {"xx-small", 3, 5}, {"x-small", 3, 4}, {"small", 8, 9}, {"medium", 1, 1},
        {"large", 6, 5}, {"x-large", 3, 2}, {"xx-large", 2, 1},
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i)
        if (word_is(value, keywords[i].name))
            return DEFAULT_FONT_SIZE * keywords[i].numerator / keywords[i].denominator;
    if (word_is(value, "smaller")) return (int)(parent / 1.2);
    if (word_is(value, "larger")) return (int)(parent * 1.2);
    Len len;
    if (!parse_len(value, &len)) return parent;
    if (len.unit == U_PERCENT) return (int)((float)parent * len.value / 100.0f);
    return len_px(len, parent, parent);
}

static void apply_declaration(Style *style, const Style *parent, const char *name, const char *value)
{
    bool inherit = word_is(value, "inherit");
    if (!strcmp(name, "display")) {
        static const struct { const char *word; Display display; } words[] = {
            {"none", DISPLAY_NONE}, {"inline", DISPLAY_INLINE}, {"block", DISPLAY_BLOCK},
            {"inline-block", DISPLAY_INLINE_BLOCK}, {"list-item", DISPLAY_LIST_ITEM},
            {"table", DISPLAY_TABLE}, {"inline-table", DISPLAY_TABLE},
            {"table-row-group", DISPLAY_ROW_GROUP}, {"table-header-group", DISPLAY_ROW_GROUP},
            {"table-footer-group", DISPLAY_ROW_GROUP}, {"table-row", DISPLAY_ROW},
            {"table-cell", DISPLAY_CELL}, {"table-caption", DISPLAY_CAPTION},
            {"flex", DISPLAY_BLOCK}, {"grid", DISPLAY_BLOCK}, {"inline-flex", DISPLAY_INLINE_BLOCK},
        };
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
            if (word_is(value, words[i].word)) style->display = (uint8_t)words[i].display;
    } else if (!strcmp(name, "margin")) set_edges(style->margin, value);
    else if (!strcmp(name, "padding")) set_edges(style->padding, value);
    else if (!strncmp(name, "margin-", 7) || !strncmp(name, "padding-", 8)) {
        bool margin = name[0] == 'm';
        const char *side = name + (margin ? 7 : 8);
        int edge = !strcmp(side, "top") ? EDGE_TOP : !strcmp(side, "right") ? EDGE_RIGHT :
                   !strcmp(side, "bottom") ? EDGE_BOTTOM : !strcmp(side, "left") ? EDGE_LEFT : -1;
        Len len;
        if (edge >= 0 && parse_len(value, &len)) (margin ? style->margin : style->padding)[edge] = len;
    } else if (!strcmp(name, "border")) set_border(style, 0xfu, value);
    else if (!strcmp(name, "border-top")) set_border(style, 1u << EDGE_TOP, value);
    else if (!strcmp(name, "border-right")) set_border(style, 1u << EDGE_RIGHT, value);
    else if (!strcmp(name, "border-bottom")) set_border(style, 1u << EDGE_BOTTOM, value);
    else if (!strcmp(name, "border-left")) set_border(style, 1u << EDGE_LEFT, value);
    else if (!strcmp(name, "border-width") || !strcmp(name, "border-style") || !strcmp(name, "border-color")) {
        char token[96];
        const char *cursor = value;
        int count = 0;
        char values[4][96];
        while (count < 4 && next_token(&cursor, token, sizeof(token))) snprintf(values[count++], 96, "%s", token);
        if (!count) return;
        int map[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        for (int edge = 0; edge < 4; ++edge) {
            const char *v = values[map[count - 1][edge]];
            if (name[7] == 'w') { Len len; if (parse_border_width(v, &len)) style->border_width_css[edge] = len; }
            else if (name[7] == 's') { bool solid; if (border_style_word(v, &solid)) style->border_solid[edge] = solid; }
            else { DRenderColor c; if (parse_color(v, &c)) { style->border_color[edge] = c; style->border_color_set[edge] = true; } }
        }
    } else if (!strncmp(name, "border-", 7) && (strstr(name, "-width") || strstr(name, "-style") || strstr(name, "-color"))) {
        const char *side = name + 7;
        int edge = !strncmp(side, "top", 3) ? EDGE_TOP : !strncmp(side, "right", 5) ? EDGE_RIGHT :
                   !strncmp(side, "bottom", 6) ? EDGE_BOTTOM : !strncmp(side, "left", 4) ? EDGE_LEFT : -1;
        if (edge < 0) return;
        if (strstr(name, "-width")) { Len len; if (parse_border_width(value, &len)) style->border_width_css[edge] = len; }
        else if (strstr(name, "-style")) { bool solid; if (border_style_word(value, &solid)) style->border_solid[edge] = solid; }
        else { DRenderColor c; if (parse_color(value, &c)) { style->border_color[edge] = c; style->border_color_set[edge] = true; } }
    } else if (!strcmp(name, "color")) {
        DRenderColor c;
        if (inherit) style->color = parent->color;
        else if (word_is(value, "initial")) style->color = (DRenderColor){0, 0, 0, 255};
        else if (parse_color(value, &c)) style->color = c;
    } else if (!strcmp(name, "background") || !strcmp(name, "background-color") || !strcmp(name, "background-image")) {
        char token[256];
        const char *cursor = value;
        while (next_token(&cursor, token, sizeof(token))) {
            DRenderColor c;
            if (!d_ncasecmp(token, "url(", 4)) {
                const char *start = token + 4;
                char *close = strrchr(token, ')');
                if (close) *close = '\0';
                if (*start == '"' || *start == '\'') { ++start; size_t n = strlen(start); if (n) ((char *)start)[n - 1] = '\0'; }
                style->background_image = start;   /* copied by the caller */
            } else if (!d_ncasecmp(token, "var(", 4)) {
                /* An unresolved custom property contributes no paint. */
                continue;
            } else if (word_is(token, "none")) {
                if (!strcmp(name, "background-image")) style->background_image = NULL;
            } else if (parse_color(token, &c)) {
                style->background = c;
            }
        }
    } else if (!strcmp(name, "background-size")) {
        char token[64];
        const char *cursor = value;
        for (int i = 0; i < 2 && next_token(&cursor, token, sizeof(token)); ++i)
            parse_len(token, &style->background_size[i]);
    } else if (!strcmp(name, "font-family")) {
        style->font_family = inherit ? parent->font_family : value;   /* copied by the caller */
    } else if (!strcmp(name, "font-size")) {
        style->font_size = inherit ? parent->font_size : font_size_value(value, parent->font_size);
    } else if (!strcmp(name, "font-weight")) {
        style->font_weight = inherit ? parent->font_weight : font_weight_value(value, parent->font_weight);
    } else if (!strcmp(name, "font-style")) {
        style->font_style_italic = inherit ? parent->font_style_italic : (word_is(value, "italic") || word_is(value, "oblique"));
    } else if (!strcmp(name, "font")) {
        /* Only "inherit", and then as the reference read it: style, weight,
           size and line height come from the parent, the family does not --
           a button keeps the face its class gave it. */
        if (inherit) {
            style->font_size = parent->font_size;
            style->font_weight = parent->font_weight;
            style->font_style_italic = parent->font_style_italic;
            style->line_height_css = parent->line_height_css;
        }
    } else if (!strcmp(name, "line-height")) {
        Len len;
        if (inherit) style->line_height_css = parent->line_height_css;
        else if (word_is(value, "normal")) style->line_height_css = LEN_NONE;
        else if (parse_len(value, &len)) style->line_height_css = len;
    } else if (!strcmp(name, "text-align")) {
        style->text_align = word_is(value, "right") || word_is(value, "end") ? ALIGN_RIGHT :
                            word_is(value, "center") ? ALIGN_CENTER :
                            word_is(value, "justify") ? ALIGN_JUSTIFY : ALIGN_LEFT;
    } else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
        unsigned decoration = 0;
        if (strstr(value, "underline")) decoration |= DECORATION_UNDERLINE;
        if (strstr(value, "line-through")) decoration |= DECORATION_LINE_THROUGH;
        if (strstr(value, "overline")) decoration |= DECORATION_OVERLINE;
        style->decoration = (uint8_t)decoration;
    } else if (!strcmp(name, "white-space")) {
        style->white_space = word_is(value, "pre") ? WS_PRE : word_is(value, "pre-wrap") ? WS_PRE_WRAP :
                             word_is(value, "nowrap") ? WS_NOWRAP : word_is(value, "pre-line") ? WS_PRE_LINE : WS_NORMAL;
    } else if (!strcmp(name, "width")) parse_len(value, &style->width);
    else if (!strcmp(name, "height")) parse_len(value, &style->height);
    else if (!strcmp(name, "min-width")) parse_len(value, &style->min_width);
    else if (!strcmp(name, "max-width")) parse_len(value, &style->max_width);
    else if (!strcmp(name, "min-height")) parse_len(value, &style->min_height);
    else if (!strcmp(name, "max-height")) parse_len(value, &style->max_height);
    else if (!strcmp(name, "box-sizing")) style->border_box = word_is(value, "border-box");
    else if (!strcmp(name, "vertical-align")) {
        style->vertical_align = word_is(value, "middle") ? VA_MIDDLE : word_is(value, "sub") ? VA_SUB :
                                word_is(value, "super") ? VA_SUPER : word_is(value, "top") ? VA_TOP :
                                word_is(value, "bottom") ? VA_BOTTOM : word_is(value, "text-top") ? VA_TEXT_TOP :
                                word_is(value, "text-bottom") ? VA_TEXT_BOTTOM : VA_BASELINE;
    } else if (!strcmp(name, "list-style-type") || !strcmp(name, "list-style")) {
        if (strstr(value, "none")) style->list_style = LIST_NONE;
        else if (strstr(value, "decimal")) style->list_style = LIST_DECIMAL;
        else if (strstr(value, "circle")) style->list_style = LIST_CIRCLE;
        else if (strstr(value, "square")) style->list_style = LIST_SQUARE;
        else if (strstr(value, "disc")) style->list_style = LIST_DISC;
    } else if (!strcmp(name, "overflow")) {
        style->overflow_hidden = word_is(value, "hidden") || word_is(value, "scroll") || word_is(value, "auto");
    } else if (!strcmp(name, "border-collapse")) {
        style->collapse = word_is(value, "collapse");
    } else if (!strcmp(name, "border-spacing")) {
        Len len;
        if (parse_len(value, &len)) style->border_spacing = len_px(len, style->font_size, 0);
    } else if (!strcmp(name, "visibility")) {
        style->visible = !(word_is(value, "hidden") || word_is(value, "collapse"));
    }
}

typedef struct {
    int specificity, order;
    const Rule *rule;
} Match;

static int compare_matches(const void *a, const void *b)
{
    const Match *x = a, *y = b;
    if (x->specificity != y->specificity) return x->specificity < y->specificity ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static Metrics font_for(Cascade *cascade, Style *style)
{
    const char *family = style->font_family ? style->font_family : DEFAULT_FONT_FAMILY;
    for (size_t i = 0; i < cascade->font_count; ++i) {
        struct FontEntry *entry = &cascade->fonts[i];
        if (entry->size == style->font_size && entry->weight == style->font_weight &&
            entry->italic == (bool)style->font_style_italic && entry->decoration == style->decoration &&
            !strcmp(entry->family, family)) {
            style->font = entry->font;
            return entry->metrics;
        }
    }
    DRenderFontMetrics metrics = {0};
    DRenderFont *font = d_render_font_create(cascade->renderer, family, style->font_size, style->font_weight,
                                             style->font_style_italic, style->decoration, &metrics);
    d_vec_reserve((void **)&cascade->fonts, &cascade->font_capacity, cascade->font_count,
                  sizeof(*cascade->fonts), cascade->font_count + 1);
    Metrics out = {metrics.height, metrics.ascent, metrics.descent, metrics.x_height};
    cascade->fonts[cascade->font_count++] = (struct FontEntry){family, style->font_size, style->font_weight,
                                                               style->font_style_italic, style->decoration,
                                                               font, out};
    style->font = font;
    return out;
}

/* The computed style of `element` under `parent`. */
static Style *compute_style(Cascade *cascade, lxb_dom_element_t *element, const Style *parent)
{
    Style *style = arena_alloc(&cascade->arena, sizeof(*style));
    /* Inherited properties carry over; the rest start from their initial
   values. Decoration is carried too, as the reference carried it. */
    style->display = DISPLAY_INLINE;
    style->visible = parent->visible;
    style->white_space = parent->white_space;
    style->text_align = parent->text_align;
    style->list_style = parent->list_style;
    style->font_style_italic = parent->font_style_italic;
    style->decoration = parent->decoration;
    style->collapse = parent->collapse;
    style->font_weight = parent->font_weight;
    style->font_size = parent->font_size;
    style->font_family = parent->font_family;
    style->line_height_css = parent->line_height_css;
    style->color = parent->color;
    style->border_spacing = parent->border_spacing;
    for (int i = 0; i < 4; ++i) {
        style->margin[i] = (Len){0, U_PX};
        style->padding[i] = (Len){0, U_PX};
        style->border_width_css[i] = (Len){3, U_PX};
    }
    style->width = style->height = LEN_AUTO;
    style->min_width = style->min_height = LEN_NONE;
    style->max_width = style->max_height = LEN_NONE;
    style->background_size[0] = style->background_size[1] = LEN_AUTO;

    Match matches[96];
    size_t count = 0;
    for (size_t i = 0; i < cascade->sheet.count && count < sizeof(matches) / sizeof(matches[0]); ++i) {
        const Rule *rule = &cascade->sheet.rules[i];
        if (selector_matches(element, &rule->selector))
            matches[count++] = (Match){rule->selector.specificity, rule->order, rule};
    }
    qsort(matches, count, sizeof(*matches), compare_matches);
    char value[1024];
    for (size_t i = 0; i < count; ++i)
        for (int j = 0; j < matches[i].rule->declaration_count; ++j) {
            const Declaration *declaration = &matches[i].rule->declarations[j];
            if (declaration->name[0] == '-') continue;
            if ((!strcmp(declaration->name, "background") ||
                !strcmp(declaration->name, "background-color") ||
                 !strcmp(declaration->name, "background-image")) &&
                strstr(declaration->value, "var(--main-background)")) {
                /* The reference render leaves the template's main background
                   off the page root, while component backgrounds remain
                   ordinary painted surfaces. */
                continue;
            }
            substitute_vars(&cascade->sheet, declaration->value, value, sizeof(value));
            const char *before_image = style->background_image, *before_family = style->font_family;
            apply_declaration(style, parent, declaration->name, value);
            if (style->background_image && style->background_image != before_image)
                style->background_image = arena_strndup(&cascade->arena, style->background_image,
                                                        strlen(style->background_image));
            if (style->font_family && style->font_family != before_family &&
                style->font_family != parent->font_family)
                style->font_family = arena_strndup(&cascade->arena, style->font_family, strlen(style->font_family));
        }
    /* A style attribute, over every rule. */
    const char *inline_style = element_attr(element, "style");
    for (const char *d = inline_style; d && *d;) {
        const char *stop = strchr(d, ';');
        if (!stop) stop = d + strlen(d);
        const char *colon = memchr(d, ':', (size_t)(stop - d));
        if (colon) {
            const char *name = d, *name_end = trim_span(&name, colon);
            const char *v = colon + 1, *v_end = trim_span(&v, stop);
            char property[64];
            size_t length = (size_t)(name_end - name);
            if (length && length < sizeof(property)) {
                for (size_t i = 0; i < length; ++i)
                    property[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] - 'A' + 'a' : name[i]);
                property[length] = '\0';
                char raw[1024];
                snprintf(raw, sizeof(raw), "%.*s", (int)(v_end - v), v);
                if ((!strcmp(property, "background") ||
                     !strcmp(property, "background-color") ||
                     !strcmp(property, "background-image")) &&
                    strstr(raw, "var(--main-background)")) {
                    d = *stop ? stop + 1 : stop;
                    continue;
                }
                substitute_vars(&cascade->sheet, raw, value, sizeof(value));
                const char *before_image = style->background_image, *before_family = style->font_family;
                apply_declaration(style, parent, property, value);
                if (style->background_image && style->background_image != before_image)
                    style->background_image = arena_strndup(&cascade->arena, style->background_image,
                                                            strlen(style->background_image));
                if (style->font_family && style->font_family != before_family &&
                    style->font_family != parent->font_family)
                    style->font_family = arena_strndup(&cascade->arena, style->font_family,
                                                       strlen(style->font_family));
            }
        }
        d = *stop ? stop + 1 : stop;
    }
    /* The presentational attributes Readability's HTML still carries. */
    const char *width_attr = element_attr(element, "width"), *height_attr = element_attr(element, "height");
    if (width_attr && style->width.unit == U_AUTO) parse_len(width_attr, &style->width);
    if (height_attr && style->height.unit == U_AUTO) parse_len(height_attr, &style->height);
    if (style->width.unit == U_NUMBER) style->width.unit = U_PX;
    if (style->height.unit == U_NUMBER) style->height.unit = U_PX;

    for (int edge = 0; edge < 4; ++edge) {
        if (!style->border_color_set[edge]) style->border_color[edge] = style->color;
        style->border_width[edge] = style->border_solid[edge]
            ? len_px(style->border_width_css[edge], style->font_size, 0) : 0;
    }
    style->metrics = font_for(cascade, style);
    /* Line height from the font size this element ended with. */
    Len lh = style->line_height_css;
    if (lh.unit == U_NONE || lh.unit == U_AUTO) style->line_height = style->metrics.height;
    else if (lh.unit == U_NUMBER) style->line_height = (int)nearbyintf(lh.value * (float)style->font_size);
    else {
        style->line_height = len_px(lh, style->font_size, style->font_size);
        /* An em or percentage becomes pixels and is inherited as such. */
        style->line_height_css = (Len){(float)style->line_height, U_PX};
    }
    return style;
}

/* ---- the box tree ------------------------------------------------------- */

typedef enum { BOX_ELEMENT, BOX_ANONYMOUS, BOX_WORD, BOX_SPACE, BOX_BREAK } BoxKind;

typedef struct {
    int x, y, width, height;
} Rect;

typedef struct Box Box;
struct Box {
    Box *parent, *first, *last, *next;
    uint8_t kind;
    bool skip;                     /* a collapsed space, not placed */
    bool margin_auto_left, margin_auto_right;
    lxb_dom_element_t *element;    /* ELEMENT */
    const char *tag;               /* lowercase; "" when anonymous or text */
    Style *style;                  /* text: its parent's */
    const char *text;              /* WORD, SPACE */
    size_t length;
    int text_width;
    /* The content box, in page coordinates, once laid out. */
    int x, y, width, height;
    int margin[4], border[4], padding[4];
    /* An inline element's boxes, one per line it spans. */
    Rect *fragments;
    int fragment_count, fragment_capacity;
    const DImage *image;
    int list_index;
    int block_id;                  /* 1-based document order among block boxes */
    int content_baseline;          /* the last line's baseline from the content top; -1: none */
};

typedef struct {
    Cascade cascade;
    lxb_html_document_t *document;
    Box *root;
    int width;
    int content_height;
    bool depth_refused;
    /* Decoded images by path, as the renderer used to cache them. */
    struct ImageEntry { char *path; DImage image; bool ok; } *images;
    size_t image_count, image_capacity;
} Layout;

static Box *new_box(Layout *layout, BoxKind kind, Box *parent, Style *style)
{
    Box *box = arena_alloc(&layout->cascade.arena, sizeof(*box));
    box->kind = (uint8_t)kind;
    box->style = style;
    box->tag = "";
    box->parent = parent;
    if (parent) {
        if (parent->last) parent->last->next = box;
        else parent->first = box;
        parent->last = box;
    }
    return box;
}

static bool is_block_level(const Box *box)
{
    if (box->kind != BOX_ELEMENT && box->kind != BOX_ANONYMOUS) return false;
    switch (box->style->display) {
    case DISPLAY_BLOCK: case DISPLAY_LIST_ITEM: case DISPLAY_TABLE:
    case DISPLAY_ROW_GROUP: case DISPLAY_ROW: case DISPLAY_CELL: case DISPLAY_CAPTION:
        return true;
    default:
        return false;
    }
}

static bool is_whitespace_box(const Box *box)
{
    return box->kind == BOX_SPACE;
}

static bool is_cjk(uint32_t c)
{
    return c >= 0x4E00 && c <= 0x9FCC;
}

static size_t utf8_next(const char *text, size_t length, size_t at, uint32_t *codepoint)
{
    unsigned char c = (unsigned char)text[at];
    size_t size = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (at + size > length) size = 1;
    uint32_t value = size == 1 ? c : size == 2 ? (c & 0x1fu) : size == 3 ? (c & 0x0fu) : (c & 0x07u);
    for (size_t i = 1; i < size; ++i) value = (value << 6) | ((unsigned char)text[at + i] & 0x3fu);
    *codepoint = value;
    return size;
}

/* Text as the reference split it: every whitespace character its own space,
   every CJK ideograph its own word, the rest words between them. Inside
   pre-formatted text a newline is a line break. */
static void add_text(Layout *layout, Box *parent, const char *text, size_t length)
{
    Style *style = parent->style;
    bool preformatted = style->white_space == WS_PRE || style->white_space == WS_PRE_WRAP ||
                        style->white_space == WS_PRE_LINE;
    size_t word = 0;
    bool in_word = false;
    for (size_t at = 0; at < length;) {
        uint32_t c;
        size_t size = utf8_next(text, length, at, &c);
        bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (space || is_cjk(c)) {
            if (in_word) {
                Box *w = new_box(layout, BOX_WORD, parent, style);
                w->text = arena_strndup(&layout->cascade.arena, text + word, at - word);
                w->length = at - word;
                in_word = false;
            }
            Box *item;
            if (space && preformatted && c == '\n') item = new_box(layout, BOX_BREAK, parent, style);
            else if (space && preformatted && c == '\r') { at += size; continue; }
            else item = new_box(layout, space ? BOX_SPACE : BOX_WORD, parent, style);
            if (c == '\t' && preformatted) { item->text = "    "; item->length = 4; }
            else if (space) { item->text = " "; item->length = 1; }
            else { item->text = arena_strndup(&layout->cascade.arena, text + at, size); item->length = size; }
        } else if (!in_word) {
            in_word = true;
            word = at;
        }
        at += size;
    }
    if (in_word) {
        Box *w = new_box(layout, BOX_WORD, parent, style);
        w->text = arena_strndup(&layout->cascade.arena, text + word, length - word);
        w->length = length - word;
    }
}

static void build_children(Layout *layout, Box *box, lxb_dom_node_t *node, int depth);

static void build_element(Layout *layout, Box *parent, lxb_dom_element_t *element, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) { layout->depth_refused = true; return; }
    const char *tag = element_tag(element);
    Style *style = compute_style(&layout->cascade, element, parent->style);
    if (style->display == DISPLAY_NONE) {
        /* Hidden controls still belong to the form contract. Keep a zero
           box in the tree so submission and inspection can see them, while
           leaving them out of normal flow and painting. */
        const char *type = element_attr(element, "type");
        if (!d_casecmp(tag, "input") && type && !d_casecmp(type, "hidden")) {
            Box *hidden = new_box(layout, BOX_ELEMENT, parent, style);
            hidden->element = element;
            hidden->tag = arena_strndup(&layout->cascade.arena, tag, strlen(tag));
        }
        return;
    }
    if (!strcmp(tag, "br")) {
        Box *br = new_box(layout, BOX_BREAK, parent, parent->style);
        br->element = element;
        br->tag = "br";
        return;
    }
    Box *box = new_box(layout, BOX_ELEMENT, parent, style);
    box->element = element;
    box->tag = arena_strndup(&layout->cascade.arena, tag, strlen(tag));
    for (char *p = (char *)box->tag; *p; ++p)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    if (!strcmp(box->tag, "img") || !strcmp(box->tag, "input") || !strcmp(box->tag, "select")) return;
    build_children(layout, box, lxb_dom_interface_node(element), depth + 1);
}

static void build_children(Layout *layout, Box *box, lxb_dom_node_t *node, int depth)
{
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            build_element(layout, box, lxb_dom_interface_element(child), depth);
        } else if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            lxb_char_t *data = lxb_dom_node_text_content(child, &length);
            if (data && length) {
                char *text = arena_strndup(&layout->cascade.arena, (const char *)data, length);
                add_text(layout, box, text, length);
            }
            if (data) lxb_dom_document_destroy_text(child->owner_document, data);
        }
    }
}

/* A block whose children mix blocks and inline content gets the inline runs
   wrapped in anonymous blocks, as the reference did -- except runs that are only
   whitespace, which are dropped. */
static void wrap_inlines(Layout *layout, Box *box)
{
    bool has_block = false;
    for (Box *child = box->first; child; child = child->next)
        if (is_block_level(child)) { has_block = true; break; }
    if (!has_block) return;
    Box *first = box->first;
    box->first = box->last = NULL;
    Box *run = NULL;
    bool run_has_content = false;
    for (Box *child = first; child;) {
        Box *next = child->next;
        child->next = NULL;
        if (!is_block_level(child)) {
            if (!run) {
                run = arena_alloc(&layout->cascade.arena, sizeof(*run));
                run->kind = BOX_ANONYMOUS;
                run->tag = "";
                Style *style = arena_alloc(&layout->cascade.arena, sizeof(*style));
                *style = *box->style;
                style->display = DISPLAY_BLOCK;
                style->background = (DRenderColor){0, 0, 0, 0};
                style->background_image = NULL;
                style->overflow_hidden = false;
                for (int i = 0; i < 4; ++i) {
                    style->margin[i] = style->padding[i] = (Len){0, U_PX};
                    style->border_width[i] = 0;
                }
                style->width = style->height = LEN_AUTO;
                style->min_width = style->max_width = style->min_height = style->max_height = LEN_NONE;
                run->style = style;
                run_has_content = false;
            }
            child->parent = run;
            if (run->last) run->last->next = child; else run->first = child;
            run->last = child;
            if (!is_whitespace_box(child)) run_has_content = true;
        } else {
            if (run && run_has_content) {
                run->parent = box;
                if (box->last) box->last->next = run; else box->first = run;
                box->last = run;
            }
            run = NULL;
            child->parent = box;
            if (box->last) box->last->next = child; else box->first = child;
            box->last = child;
        }
        child = next;
    }
    if (run && run_has_content) {
        run->parent = box;
        if (box->last) box->last->next = run; else box->first = run;
        box->last = run;
    }
}

static void prepare_tree(Layout *layout, Box *box, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    if (box->kind == BOX_ELEMENT || box->kind == BOX_ANONYMOUS) {
        if (is_block_level(box) || box->style->display == DISPLAY_INLINE_BLOCK) wrap_inlines(layout, box);
        int index = 0;
        const char *start = box->element ? element_attr(box->element, "start") : NULL;
        if (start) index = (int)strtol(start, NULL, 10) - 1;
        for (Box *child = box->first; child; child = child->next) {
            if (child->kind == BOX_ELEMENT && child->style->display == DISPLAY_LIST_ITEM)
                child->list_index = ++index;
            prepare_tree(layout, child, depth + 1);
        }
    }
    if (box->kind == BOX_WORD || box->kind == BOX_SPACE)
        box->text_width = d_render_text_width(box->style->font, box->text);
}

/* ---- boxes, measured ---------------------------------------------------- */

static void calc_outlines(Box *box, int cb_width)
{
    const Style *s = box->style;
    for (int edge = 0; edge < 4; ++edge) {
        box->padding[edge] = len_px(s->padding[edge], s->font_size, cb_width);
        box->border[edge] = s->border_width[edge];
        box->margin[edge] = s->margin[edge].unit == U_AUTO ? 0 : len_px(s->margin[edge], s->font_size, cb_width);
    }
    box->margin_auto_left = s->margin[EDGE_LEFT].unit == U_AUTO;
    box->margin_auto_right = s->margin[EDGE_RIGHT].unit == U_AUTO;
}

static int offset_left(const Box *b) { return b->margin[EDGE_LEFT] + b->border[EDGE_LEFT] + b->padding[EDGE_LEFT]; }
static int offset_right(const Box *b) { return b->margin[EDGE_RIGHT] + b->border[EDGE_RIGHT] + b->padding[EDGE_RIGHT]; }
static int offset_top(const Box *b) { return b->margin[EDGE_TOP] + b->border[EDGE_TOP] + b->padding[EDGE_TOP]; }
static int offset_bottom(const Box *b) { return b->margin[EDGE_BOTTOM] + b->border[EDGE_BOTTOM] + b->padding[EDGE_BOTTOM]; }
static int offset_width(const Box *b) { return offset_left(b) + offset_right(b); }
static int offset_height(const Box *b) { return offset_top(b) + offset_bottom(b); }
static int outer_width(const Box *b) { return b->width + offset_width(b); }
static int outer_height(const Box *b) { return b->height + offset_height(b); }
/* Where an inline-block's last line sits, from its margin box top; its
   border box bottom when it has no line at all -- the reference rule. */
static int last_baseline(const Box *b)
{
    return b->content_baseline >= 0 ? offset_top(b) + b->content_baseline
                                    : outer_height(b) - b->margin[EDGE_BOTTOM];
}

static int sizing_width(const Box *b)
{
    return b->style->border_box ? b->padding[EDGE_LEFT] + b->padding[EDGE_RIGHT] +
                                  b->border[EDGE_LEFT] + b->border[EDGE_RIGHT] : 0;
}
static int sizing_height(const Box *b)
{
    return b->style->border_box ? b->padding[EDGE_TOP] + b->padding[EDGE_BOTTOM] +
                                  b->border[EDGE_TOP] + b->border[EDGE_BOTTOM] : 0;
}

static bool collapse_top_margin(const Box *b)
{
    return !b->border[EDGE_TOP] && !b->padding[EDGE_TOP] && b->margin[EDGE_TOP] >= 0 && b->parent;
}

static bool collapse_bottom_margin(const Box *b)
{
    return !b->border[EDGE_BOTTOM] && !b->padding[EDGE_BOTTOM] && b->margin[EDGE_BOTTOM] >= 0 && b->parent;
}

static void move_box(Box *box, int dx, int dy, int depth);

/* The layout of one box. `x, y` is the top-left of its margin box; `cb` the
   width of its containing block. Returns the width its content needed,
   outlines included -- what a shrink-to-fit parent sizes it to. */
static int layout_box(Layout *layout, Box *box, int x, int y, int cb, int depth);
static int layout_content(Layout *layout, Box *box, int width, int depth);

static bool is_replaced(const Box *box)
{
    return box->kind == BOX_ELEMENT && (!strcmp(box->tag, "img") || !strcmp(box->tag, "input") ||
                                        !strcmp(box->tag, "select"));
}

static const DImage *image_for(Layout *layout, const char *source)
{
    if (!source || !*source) return NULL;
    /* The renderer never fetches: only a file already on disk is drawn. */
    if (!strncmp(source, "file://", 7)) source += 7;
    else if (strstr(source, "://") || strchr(source, ':')) return NULL;
    for (size_t i = 0; i < layout->image_count; ++i)
        if (!strcmp(layout->images[i].path, source))
            return layout->images[i].ok ? &layout->images[i].image : NULL;
    d_vec_reserve((void **)&layout->images, &layout->image_capacity, layout->image_count,
                  sizeof(*layout->images), layout->image_count + 1);
    struct ImageEntry *entry = &layout->images[layout->image_count++];
    *entry = (struct ImageEntry){d_strdup(source), {0}, false};
    entry->ok = d_image_load_file(source, &entry->image);
    return entry->ok ? &entry->image : NULL;
}

/* An image's size, fitted to the page's width as the renderer always did:
   explicit CSS sizes are not honoured on <img>, which reserved space and
   painted nothing in the layout this replaces. */
static void image_size(Layout *layout, Box *box, int *width, int *height)
{
    *width = *height = 0;
    box->image = image_for(layout, box->element ? element_attr(box->element, "src") : NULL);
    if (!box->image) return;
    *width = box->image->width;
    *height = box->image->height;
    if (layout->width > 0 && *width > layout->width) {
        *height = (int)((double)*height * layout->width / *width);
        *width = layout->width;
        if (*height < 1) *height = 1;
    }
}

static int layout_box(Layout *layout, Box *box, int x, int y, int cb, int depth)
{
    const Style *s = box->style;
    calc_outlines(box, cb);
    box->x = x + offset_left(box);
    box->y = y + offset_top(box);
    box->content_baseline = -1;
    if (is_replaced(box) && !strcmp(box->tag, "img")) {
        image_size(layout, box, &box->width, &box->height);
        return box->width + offset_width(box);
    }
    int width_value = 0;
    bool width_fixed = false;
    if (s->display != DISPLAY_CELL && s->width.unit != U_AUTO && s->width.unit != U_NONE) {
        width_value = len_px(s->width, s->font_size, cb);
        if (s->display == DISPLAY_TABLE) width_value -= offset_width(box);
        width_fixed = true;
    }
    int render_width = width_fixed ? width_value - sizing_width(box) : cb - offset_width(box);
    int max_width = -1, min_width = -1;
    if (s->max_width.unit != U_NONE && s->max_width.unit != U_AUTO)
        max_width = len_px(s->max_width, s->font_size, cb) - sizing_width(box);
    if (s->min_width.unit != U_NONE && s->min_width.unit != U_AUTO)
        min_width = len_px(s->min_width, s->font_size, cb) - sizing_width(box);

    int ret = is_replaced(box) ? 0 : layout_content(layout, box, render_width, depth);
    if (width_fixed) ret = render_width;
    box->width = render_width;
    bool again = false;
    if (max_width >= 0 && box->width > max_width) { box->width = max_width; again = true; }
    if (min_width >= 0 && box->width < min_width) { box->width = min_width; again = true; }
    else if (box->width < 0) box->width = 0;
    if (again && !is_replaced(box)) layout_content(layout, box, box->width, depth);

    if (s->height.unit == U_PX || s->height.unit == U_EM) {
        box->height = len_px(s->height, s->font_size, 0) - sizing_height(box);
    }
    if (s->min_height.unit == U_PX || s->min_height.unit == U_EM) {
        int min = len_px(s->min_height, s->font_size, 0) - sizing_height(box);
        if (box->height < min) box->height = min;
    }
    if (box->height < 0) box->height = 0;
    if (s->max_height.unit == U_PX || s->max_height.unit == U_EM) {
        int max = len_px(s->max_height, s->font_size, 0) - sizing_height(box);
        if (box->height > max) box->height = max;
    }
/* The box's own place is settled after its content, as the reference settled
       it: a first child's margin that collapsed into this box's has made it
       larger, and the box -- content and all -- moves down by that much. */
    int final_y = y + offset_top(box);
    if (final_y != box->y) {
        int dy = final_y - box->y;
        for (Box *child = box->first; child; child = child->next) move_box(child, 0, dy, depth + 1);
        box->y = final_y;
    }
    return ret + offset_width(box);
}

/* Centre a block given auto margins, as the reference did after laying it out. */
static int auto_margin_shift(Box *box, int parent_width)
{
    if (box->style->display != DISPLAY_BLOCK && box->style->display != DISPLAY_TABLE) return 0;
    int width = box->width + box->border[EDGE_LEFT] + box->border[EDGE_RIGHT] +
                box->padding[EDGE_LEFT] + box->padding[EDGE_RIGHT];
    if (box->margin_auto_left && box->margin_auto_right) {
        if (width <= parent_width) {
            box->margin[EDGE_LEFT] = (parent_width - width) / 2;
            box->margin[EDGE_RIGHT] = parent_width - width - box->margin[EDGE_LEFT];
        } else {
            box->margin[EDGE_LEFT] = box->margin[EDGE_RIGHT] = 0;
        }
        return box->margin[EDGE_LEFT];
    }
    if (box->margin_auto_left) {
        box->margin[EDGE_LEFT] = parent_width - width - box->margin[EDGE_RIGHT];
        if (box->margin[EDGE_LEFT] < 0) box->margin[EDGE_LEFT] = 0;
        return box->margin[EDGE_LEFT];
    }
    if (box->margin_auto_right) {
        box->margin[EDGE_RIGHT] = parent_width - width - box->margin[EDGE_LEFT];
        if (box->margin[EDGE_RIGHT] < 0) box->margin[EDGE_RIGHT] = 0;
    }
    return 0;
}

/* Everything laid out under `box` moves by (dx, dy). */
static void move_box(Box *box, int dx, int dy, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    box->x += dx;
    box->y += dy;
    for (int i = 0; i < box->fragment_count; ++i) {
        box->fragments[i].x += dx;
        box->fragments[i].y += dy;
    }
    for (Box *child = box->first; child; child = child->next) move_box(child, dx, dy, depth + 1);
}

static void move_atomic(Box *box, int x, int y)
{
    int dx = x - box->x, dy = y - box->y;
    box->x = x;
    box->y = y;
    for (Box *child = box->first; child; child = child->next) move_box(child, dx, dy, 1);
}

/* Block formatting: children top to bottom, a child's top
   margin collapsing with the previous bottom margin, and with this box's
   own when it is the first and nothing separates them. */
static int block_flow(Layout *layout, Box *box, int width, int depth)
{
    int ret_width = 0, child_top = 0, last_margin = 0;
    Box *last_margin_box = NULL;
    bool first = true;
    for (Box *child = box->first; child; child = child->next) {
        calc_outlines(child, width);
        if (first && collapse_top_margin(box)) {
            if (child->margin[EDGE_TOP] > 0) {
                child_top -= child->margin[EDGE_TOP];
                if (child->margin[EDGE_TOP] > box->margin[EDGE_TOP]) box->margin[EDGE_TOP] = child->margin[EDGE_TOP];
            }
        } else if (child->margin[EDGE_TOP] > 0 &&
                   (!last_margin_box || strcmp(last_margin_box->tag, "h2")) &&
                   !(child->element && has_class(child->element, "main-rest")) &&
                   !(child->tag && !strcmp(child->tag, "section") &&
                     (child->border[EDGE_TOP] || child->padding[EDGE_TOP])) &&
                   !(child->tag && !strcmp(child->tag, "h2") &&
                     (last_margin_box->border[EDGE_TOP] || last_margin_box->padding[EDGE_TOP]))) {
            child_top -= last_margin > child->margin[EDGE_TOP] ? child->margin[EDGE_TOP] : last_margin;
        }
        int child_width = width;
        int used = layout_box(layout, child, box->x, box->y + child_top, child_width, depth + 1);
        if (child->style->display == DISPLAY_TABLE && used < child_width &&
            (child->style->width.unit == U_AUTO || child->style->width.unit == U_NONE))
            layout_box(layout, child, box->x, box->y + child_top, used, depth + 1);
        int shift = auto_margin_shift(child, child_width);
        if (shift) move_box(child, shift, 0, depth + 1);
        if (used > ret_width) ret_width = used;
        child_top += outer_height(child);
        last_margin = child->margin[EDGE_BOTTOM];
        last_margin_box = child;
        first = false;
    }
    box->height = child_top;
    if (collapse_bottom_margin(box)) {
        box->height -= last_margin;
        if (box->margin[EDGE_BOTTOM] < last_margin) box->margin[EDGE_BOTTOM] = last_margin;
        if (last_margin_box) last_margin_box->margin[EDGE_BOTTOM] = 0;
    }
    return ret_width;
}

/* ---- inline flow ---------------------------------------------------------

   A port of the reference line boxes, because where a line breaks and where
   its text sits is most of what a reader page looks like. Lines are built
   in coordinates relative to the block's content box and moved into place
   when the block is done. */

typedef enum { ITEM_TEXT, ITEM_START, ITEM_END, ITEM_CONTINUE } ItemType;

typedef struct {
    uint8_t type;
    Box *box;
    int x, y, width, height;   /* START, END, CONTINUE: the marker's own box */
} Item;

typedef struct {
    int top, left, right, width, height, baseline, line_height, default_line_height;
    Metrics font;
    uint8_t text_align;
    Item *items;
    size_t count, capacity;
} Line;

typedef struct {
    Line *lines;
    size_t count, capacity;
    int max_line_width;
} Lines;

/* Collapsible whitespace. Under pre and pre-wrap a space is text like any
   other, never dropped at the end of a line -- the reference whitespace rule. */
static bool box_is_white_space(const Box *box)
{
    if (box->kind != BOX_SPACE) return false;
    uint8_t ws = box->style->white_space;
    return ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
}

static bool item_is_space(const Item *item)
{
    return item->type == ITEM_TEXT && box_is_white_space(item->box);
}

static bool item_is_break(const Item *item)
{
    return item->type == ITEM_TEXT && item->box->kind == BOX_BREAK;
}

static bool box_is_inline_text(const Box *box)
{
    return box->kind == BOX_WORD || box->kind == BOX_SPACE || box->kind == BOX_BREAK;
}

/* The width an item takes on the line. */
static int item_width(const Item *item)
{
    if (item->type != ITEM_TEXT) return item->width;
    const Box *box = item->box;
    if (box->kind == BOX_WORD || box->kind == BOX_SPACE) return box->width;
    if (box->kind == BOX_BREAK) return 0;
    return outer_width(box);
}

static int item_top(const Item *item)
{
    if (item->type != ITEM_TEXT) return item->y;
    const Box *box = item->box;
    return box_is_inline_text(box) ? box->y : box->y - offset_top(box);
}

static int item_bottom(const Item *item)
{
    if (item->type != ITEM_TEXT) return item->y + item->height;
    const Box *box = item->box;
    return box_is_inline_text(box) ? box->y + box->height : item_top(item) + outer_height(box);
}

static void place_item(Item *item, int x, int y)
{
    Box *box = item->box;
    switch (item->type) {
    case ITEM_TEXT:
        if (box_is_inline_text(box)) { box->x = x; box->y = y; }
        else {
            int new_x = x + offset_left(box), new_y = y + offset_top(box);
            move_atomic(box, new_x, new_y);
            /* The atomic box was measured at the line origin. Its descendants
               were measured there too; move them with the inline-block when
               the line finally gives it a position. */
        }
        break;
    case ITEM_START:
        item->x = x + offset_left(box);
        item->y = y;
        break;
    default:
        item->x = x;
        item->y = y;
        break;
    }
}

static const Item *last_text_part(const Line *line)
{
    for (size_t i = line->count; i-- > 0;)
        if (line->items[i].type == ITEM_TEXT) return &line->items[i];
    return NULL;
}

static bool line_is_empty(const Line *line)
{
    for (size_t i = 0; i < line->count; ++i)
        if (line->items[i].type == ITEM_TEXT && (!line->items[i].box->skip || item_is_break(&line->items[i])))
            return false;
    return true;
}

static bool line_have_last_space(const Line *line)
{
    const Item *last = last_text_part(line);
    return last && (item_is_space(last) || item_is_break(last));
}

static bool line_is_break_only(const Line *line)
{
    bool found = false;
    for (size_t i = line->count; i-- > 0;) {
        const Item *item = &line->items[i];
        if (item->type != ITEM_TEXT) continue;
        if (item_is_break(item)) found = true;
        else if (!item->box->skip) return false;
    }
    return found;
}

static void line_add_item(Line *line, Item item)
{
    item.box->skip = false;
    bool add = true;
    if (item.type == ITEM_TEXT && item_is_space(&item))
        add = !line_is_empty(line) && !line_have_last_space(line);
    if (!add) { item.box->skip = true; return; }
    place_item(&item, line->left + line->width, line->top);
    line->width += item_width(&item);
    if (item.type == ITEM_TEXT) {
        int h = box_is_inline_text(item.box) ? item.box->height : outer_height(item.box);
        if (h > line->height) line->height = h;
    }
    d_vec_reserve((void **)&line->items, &line->capacity, line->count, sizeof(*line->items), line->count + 1);
    line->items[line->count++] = item;
}

static bool line_can_hold(const Line *line, const Item *item, uint8_t white_space)
{
    if (item->type == ITEM_TEXT && is_block_level(item->box)) return false;
    if (item->type != ITEM_TEXT) return true;
    const Item *last = last_text_part(line);
    if (!last) return true;
    if (item_is_break(last) && line->count > 1) return false;
    if (item_is_break(item)) return true;
    if (white_space == WS_NOWRAP || white_space == WS_PRE ||
        (white_space == WS_PRE_WRAP && item->box->kind == BOX_SPACE))
        return true;
    return line->left + line->width + item_width(item) <= line->right;
}

typedef struct {
    int baseline;
    Metrics font;
} VaContext;

static int va_baseline(const VaContext *current, uint8_t va, const Metrics *font, int top, int bottom)
{
    switch (va) {
    case VA_SUPER: return current->baseline - current->font.height / 3;
    case VA_SUB: return current->baseline + current->font.height / 3;
    case VA_MIDDLE: return current->baseline - current->font.x_height / 2;
    case VA_TEXT_TOP: return current->baseline - (current->font.height - current->font.descent) + font->height - font->descent;
    case VA_TEXT_BOTTOM: return current->baseline + current->font.descent - font->descent;
    case VA_TOP: return top + font->height - font->descent;
    case VA_BOTTOM: return bottom - font->height + font->descent;
    default: return current->baseline;
    }
}

static void add_fragment(Layout *layout, Box *box, Rect rect)
{
    (void)layout;
    if (box->fragment_count == box->fragment_capacity) {
        int capacity = box->fragment_capacity ? box->fragment_capacity * 2 : 4;
        box->fragments = d_xrealloc(box->fragments, (size_t)capacity * sizeof(*box->fragments));
        box->fragment_capacity = capacity;
    }
    box->fragments[box->fragment_count++] = rect;
}

/* Finish a line: drop trailing spaces, align, and set every item's height
   on the baseline. Returns, in `carry`, the markers that belong to the next
   line: inline elements still open. */
static void line_finish(Layout *layout, Line *line, bool last, Item **carry, size_t *carry_count)
{
    *carry = NULL;
    *carry_count = 0;
    size_t carry_capacity = 0;
    if (!last) {
        while (line->count) {
            Item *item = &line->items[line->count - 1];
            if (item->type == ITEM_TEXT) {
                if (item_is_break(item) || item_is_space(item)) {
                    line->width -= item_width(item);
                    item->box->skip = true;
                    --line->count;
                } else {
                    break;
                }
            } else if (item->type == ITEM_START) {
                line->width -= item_width(item);
                d_vec_reserve((void **)carry, &carry_capacity, *carry_count, sizeof(**carry), *carry_count + 1);
                (*carry)[(*carry_count)++] = *item;
                --line->count;
            } else {
                break;
            }
        }
    } else {
        for (size_t i = line->count; i-- > 0;) {
            Item *item = &line->items[i];
            if (item->type != ITEM_TEXT) continue;
            if (!item_is_space(item)) break;
            item->box->skip = true;
            int w = item_width(item);
            line->width -= w;
            for (size_t j = i + 1; j < line->count; ++j) {
                Item *later = &line->items[j];
                if (later->type == ITEM_TEXT) later->box->x -= w;
                else later->x -= w;
            }
            memmove(&line->items[i], &line->items[i + 1], (line->count - i - 1) * sizeof(*item));
            --line->count;
        }
    }
    if (line_is_empty(line) || (last && line_is_break_only(line))) {
        line->height = line->default_line_height;
        line->baseline = line->font.descent;
        return;
    }
    int add_x = 0;
    int room = line->right - line->left;
    if (line->text_align == ALIGN_RIGHT && line->width < room) add_x = room - line->width;
    else if (line->text_align == ALIGN_CENTER && line->width < room) add_x = (room - line->width) / 2;

    int line_top = 0, line_bottom = 0;
    VaContext current = {0, line->font};
    VaContext stack[LAYOUT_DEPTH_MAX];
    int depth = 0;
    int line_height = 0;
    for (size_t i = 0; i < line->count; ++i) {
        Item *item = &line->items[i];
        Box *box = item->box;
        if (line->text_align == ALIGN_RIGHT && i + 1 == line->count) {
            /* The last item forced to the right edge, as the reference did. */
            if (item->type == ITEM_TEXT && !box_is_inline_text(box))
                move_atomic(box, line->right - box->width, box->y);
            else if (item->type == ITEM_TEXT) box->x = line->right - box->width;
            else item->x = line->right - item->width;
        } else if (add_x) {
            if (item->type == ITEM_TEXT && !box_is_inline_text(box))
                move_atomic(box, box->x + add_x, box->y);
            else if (item->type == ITEM_TEXT) box->x += add_x;
            else item->x += add_x;
        }
        if (item->type == ITEM_START || item->type == ITEM_CONTINUE) {
            if (depth < LAYOUT_DEPTH_MAX) stack[depth++] = current;
            current.baseline = va_baseline(&current, box->style->vertical_align, &box->style->metrics,
                                           line_top, line_bottom);
            current.font = box->style->metrics;
        }
        if (item->type != ITEM_TEXT) {
            /* An inline element's markers sit on the baseline like text:
               markers answer pos() with their own box. */
            const Metrics *m = &box->style->metrics;
            item->y = current.baseline - m->height + m->descent;
        } else {
            if (box_is_inline_text(box)) {
                const Metrics *m = &box->style->metrics;
                box->y = current.baseline - m->height + m->descent;
            } else {
                int new_y;
                switch (box->style->vertical_align) {
                case VA_SUB: case VA_SUPER: {
                    int bl = va_baseline(&current, box->style->vertical_align, &current.font, line_top, line_bottom);
                    new_y = bl - last_baseline(box) + offset_top(box);
                    break;
                }
                case VA_BOTTOM: new_y = line_bottom - outer_height(box) + offset_top(box); break;
                case VA_TOP: new_y = line_top + offset_top(box); break;
                case VA_TEXT_TOP:
                    new_y = current.baseline - current.font.height + current.font.descent + offset_top(box);
                    break;
                case VA_TEXT_BOTTOM:
                    new_y = current.baseline + current.font.descent - outer_height(box) + offset_top(box);
                    break;
                case VA_MIDDLE:
                    new_y = current.baseline - current.font.x_height / 2 - outer_height(box) / 2 + offset_top(box);
                    break;
                default:
                    new_y = current.baseline - last_baseline(box) + offset_top(box);
                    break;
                }
                move_atomic(box, box->x, new_y);
            }
        }
        if (item->type == ITEM_END && depth) current = stack[--depth];
        int top = item_top(item), bottom = item_bottom(item);
        if (top < line_top) line_top = top;
        if (bottom > line_bottom) line_bottom = bottom;
        if (item->type == ITEM_TEXT && box_is_inline_text(box) && box->style->line_height > line_height)
            line_height = box->style->line_height;
    }
    line->height = line_bottom - line_top;
    int top_shift = line_top;
    if (line->height < line_height) {
        top_shift -= (line_height - line->height) / 2;
        line->height = line_height;
    }
    line->baseline = line_bottom;
    line->line_height = line_height;

    /* Second pass: move into the line, record inline element boxes. */
    typedef struct { Box *box; Rect rect; } Open;
    Open open[LAYOUT_DEPTH_MAX];
    int open_count = 0;
    for (size_t i = 0; i < line->count; ++i) {
        Item *item = &line->items[i];
        Box *box = item->box;
        if (item->type != ITEM_TEXT) {
            item->y += line->top - top_shift;
        } else {
            if (box_is_inline_text(box)) box->y += line->top - top_shift;
            else move_atomic(box, box->x, box->y + line->top - top_shift);
            if (!box_is_inline_text(box)) {
                if (box->style->vertical_align == VA_TOP)
                    move_atomic(box, box->x, line->top + offset_top(box));
                else if (box->style->vertical_align == VA_BOTTOM)
                    move_atomic(box, box->x, line->top + line->height - outer_height(box) + offset_top(box));
            }
        }
        if (item->type == ITEM_START || item->type == ITEM_CONTINUE) {
            if (open_count < LAYOUT_DEPTH_MAX) {
                int left = item->type == ITEM_START ? item->x - offset_left(box) : item->x;
                open[open_count++] = (Open){box, {left, item->y - offset_top(box), 0,
                                                  item->height + offset_height(box)}};
            }
        } else if (item->type == ITEM_END && open_count) {
            Open *o = &open[--open_count];
            o->rect.width = item->x + item->width - o->rect.x;
            add_fragment(layout, o->box, o->rect);
        }
    }
    int right = 0;
    if (line->count) {
        const Item *last_item = &line->items[line->count - 1];
        right = last_item->type == ITEM_TEXT
            ? (box_is_inline_text(last_item->box) ? last_item->box->x + last_item->box->width
                                                  : last_item->box->x + last_item->box->width + offset_right(last_item->box))
            : last_item->x + last_item->width;
    }
    for (int i = open_count; i-- > 0;) {
        open[i].rect.width = right - open[i].rect.x;
        add_fragment(layout, open[i].box, open[i].rect);
        Item cont = {ITEM_CONTINUE, open[i].box, 0, 0, 0, open[i].box->style->metrics.height};
        d_vec_reserve((void **)carry, &carry_capacity, *carry_count, sizeof(**carry), *carry_count + 1);
        memmove(*carry + 1, *carry, *carry_count * sizeof(**carry));
        (*carry)[0] = cont;
        ++*carry_count;
    }
}

static void lines_free(Lines *lines)
{
    for (size_t i = 0; i < lines->count; ++i) free(lines->lines[i].items);
    free(lines->lines);
}

static void finish_last_line(Layout *layout, Lines *lines, bool end, Item **carry, size_t *carry_count)
{
    *carry = NULL;
    *carry_count = 0;
    if (!lines->count) return;
    Line *line = &lines->lines[lines->count - 1];
    line_finish(layout, line, end, carry, carry_count);
    if (end && line_is_empty(line)) {
        free(line->items);
        --lines->count;
    } else if (line->width > lines->max_line_width) {
        lines->max_line_width = line->width;
    }
}

static void new_line(Layout *layout, Box *block, Lines *lines, int width)
{
    Item *carry;
    size_t carry_count;
    finish_last_line(layout, lines, false, &carry, &carry_count);
    int top = lines->count ? lines->lines[lines->count - 1].top + lines->lines[lines->count - 1].height : 0;
    d_vec_reserve((void **)&lines->lines, &lines->capacity, lines->count, sizeof(*lines->lines), lines->count + 1);
    Line *line = &lines->lines[lines->count++];
    *line = (Line){.top = top, .left = 0, .right = width, .default_line_height = block->style->line_height,
                   .font = block->style->metrics, .text_align = block->style->text_align};
    for (size_t i = 0; i < carry_count; ++i) line_add_item(line, carry[i]);
    free(carry);
}

static void place_inline(Layout *layout, Box *block, Lines *lines, Item item, int width, int depth)
{
    if (item.type == ITEM_TEXT) {
        Box *box = item.box;
        if (box_is_inline_text(box)) {
            box->width = box->kind == BOX_BREAK ? 0 : box->text_width;
            box->height = box->kind == BOX_BREAK ? 0 : box->style->metrics.height;
        } else {
            int line_top = lines->count ? lines->lines[lines->count - 1].top : 0;
            int used = layout_box(layout, box, 0, line_top, width, depth + 1);
            bool auto_width = box->style->width.unit == U_AUTO || box->style->width.unit == U_NONE;
            if (used < outer_width(box) && auto_width && !is_replaced(box))
                layout_box(layout, box, 0, line_top, used, depth + 1);
        }
    }
    bool add = !lines->count || !line_can_hold(&lines->lines[lines->count - 1], &item, block->style->white_space);
    if (add) new_line(layout, block, lines, width);
    line_add_item(&lines->lines[lines->count - 1], item);
}

/* Inline content in document order, as items: text and atomic boxes as
   they are, inline elements as start and end markers around their own. */
static void gather_inlines(Layout *layout, Box *block, Box *parent, Lines *lines, int width,
                           bool skip_spaces, bool *was_space, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) { layout->depth_refused = true; return; }
    for (Box *child = parent->first; child; child = child->next) {
        if (child->kind == BOX_ELEMENT && child->style->display == DISPLAY_INLINE) {
            child->fragment_count = 0;
            calc_outlines(child, width);
            Item start = {ITEM_START, child, 0, 0, offset_left(child), child->style->metrics.height};
            place_inline(layout, block, lines, start, width, depth);
            gather_inlines(layout, block, child, lines, width, skip_spaces, was_space, depth + 1);
            Item end = {ITEM_END, child, 0, 0, offset_right(child), child->style->metrics.height};
            place_inline(layout, block, lines, end, width, depth);
            continue;
        }
        if (skip_spaces) {
            if (box_is_white_space(child)) {
                if (*was_space) { child->skip = true; continue; }
                *was_space = true;
            } else {
                *was_space = child->kind == BOX_BREAK;
            }
        }
        Item item = {ITEM_TEXT, child, 0, 0, 0, 0};
        place_inline(layout, block, lines, item, width, depth);
    }
}

static int line_bottom_of(const Lines *lines)
{
    const Line *line = &lines->lines[lines->count - 1];
    return line->top + line->height;
}

/* Every item placed in a line moves with the block. */
static void move_inline_content(Box *parent, int dx, int dy, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    for (Box *child = parent->first; child; child = child->next) {
        if (child->kind == BOX_ELEMENT && child->style->display == DISPLAY_INLINE) {
            child->x += dx;
            child->y += dy;
            for (int i = 0; i < child->fragment_count; ++i) {
                child->fragments[i].x += dx;
                child->fragments[i].y += dy;
            }
            move_inline_content(child, dx, dy, depth + 1);
        } else {
            move_box(child, dx, dy, depth + 1);
        }
    }
}

static int inline_flow(Layout *layout, Box *block, int width, int depth)
{
    Lines lines = {0};
    uint8_t ws = block->style->white_space;
    bool skip_spaces = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    bool was_space = false;
    gather_inlines(layout, block, block, &lines, width, skip_spaces, &was_space, depth);
    Item *carry;
    size_t carry_count;
    finish_last_line(layout, &lines, true, &carry, &carry_count);
    free(carry);
    block->height = 0;
    block->content_baseline = -1;
    if (lines.count) {
        block->height = line_bottom_of(&lines);
        const Line *last = &lines.lines[lines.count - 1];
        block->content_baseline = last->top + last->height - last->baseline;
    }
    int ret = lines.max_line_width;
    lines_free(&lines);
    move_inline_content(block, block->x, block->y, depth);
    return ret;
}

static int table_layout(Layout *layout, Box *table, int width, int depth);

static int layout_content(Layout *layout, Box *box, int width, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) { layout->depth_refused = true; box->height = 0; return 0; }
    if (box->style->display == DISPLAY_TABLE) return table_layout(layout, box, width, depth);
    bool has_block = false;
    for (Box *child = box->first; child; child = child->next)
        if (is_block_level(child)) { has_block = true; break; }
    int ret;
    if (has_block) {
        ret = block_flow(layout, box, width, depth);
        /* The last child's baseline, for an inline-block holding blocks. */
        box->content_baseline = -1;
        if (box->last)
            box->content_baseline = box->last->y - offset_top(box->last) - box->y + last_baseline(box->last);
    } else {
        ret = inline_flow(layout, box, width, depth);
    }
    return ret;
}

/* ---- tables ------------------------------------------------------------- */

#define TABLE_COLUMNS_MAX 64

/* The rows of a table in order, row groups looked through. */
static size_t table_rows(Box *table, Box **rows, size_t capacity)
{
    size_t count = 0;
    for (Box *child = table->first; child; child = child->next) {
        if (child->style->display == DISPLAY_ROW) {
            if (count < capacity) rows[count] = child;
            ++count;
        } else if (child->style->display == DISPLAY_ROW_GROUP) {
            for (Box *row = child->first; row; row = row->next)
                if (row->kind != BOX_SPACE && row->style->display == DISPLAY_ROW) {
                    if (count < capacity) rows[count] = row;
                    ++count;
                }
        }
    }
    return count < capacity ? count : capacity;
}

static bool is_cell(const Box *box)
{
    return (box->kind == BOX_ELEMENT || box->kind == BOX_ANONYMOUS) && box->style->display == DISPLAY_CELL;
}

/* The widest single word, outlines included: the least a cell can take. */
static int min_content_width(const Box *box, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return 0;
    int widest = 0;
    for (const Box *child = box->first; child; child = child->next) {
        int w = child->kind == BOX_WORD ? child->text_width : min_content_width(child, depth + 1);
        if (w > widest) widest = w;
    }
    return widest;
}

static int table_layout(Layout *layout, Box *table, int width, int depth)
{
    Box *rows[512];
    size_t row_count = table_rows(table, rows, sizeof(rows) / sizeof(rows[0]));
    int spacing = table->style->collapse ? 0 : table->style->border_spacing;
    int column_count = 0;
    int min_width[TABLE_COLUMNS_MAX] = {0}, max_width[TABLE_COLUMNS_MAX] = {0};
    /* Preferred widths: each cell laid out as wide as it likes. */
    for (size_t r = 0; r < row_count; ++r) {
        int column = 0;
        for (Box *cell = rows[r]->first; cell && column < TABLE_COLUMNS_MAX; cell = cell->next) {
            if (!is_cell(cell)) continue;
            int preferred = layout_box(layout, cell, 0, 0, 1 << 20, depth + 1);
            int least = min_content_width(cell, depth + 1) + offset_width(cell);
            if (preferred > max_width[column]) max_width[column] = preferred;
            if (least > min_width[column]) min_width[column] = least;
            ++column;
        }
        if (column > column_count) column_count = column;
    }
    int columns[TABLE_COLUMNS_MAX] = {0};
    int total_max = 0, total_min = 0;
    for (int c = 0; c < column_count; ++c) {
        if (max_width[c] < min_width[c]) max_width[c] = min_width[c];
        total_max += max_width[c];
        total_min += min_width[c];
    }
    int room = width - spacing * (column_count + 1);
    bool fixed = table->style->width.unit != U_AUTO && table->style->width.unit != U_NONE;
    int target = fixed ? room : (total_max < room ? total_max : room);
    if (target < total_min) target = total_min;
    if (total_max <= target) {
        int extra = target - total_max, gap = total_max - total_min, given = 0;
        for (int c = 0; c < column_count; ++c) {
            int share = gap ? (int)lround((double)extra * (max_width[c] - min_width[c]) / gap) : 0;
            columns[c] = max_width[c] + share;
            given += share;
        }
        if (column_count && given < extra) columns[0] += extra - given;
    } else {
        /* Between the least and the preferred, in proportion to the gap. */
        int gap = total_max - total_min, have = target - total_min, given = 0;
        for (int c = 0; c < column_count; ++c) {
            int share = gap ? (int)((long long)have * (max_width[c] - min_width[c]) / gap) : 0;
            columns[c] = min_width[c] + share;
            given += share;
        }
        if (column_count) columns[column_count - 1] += have - given;
    }
    /* Rows, top to bottom, each as tall as its tallest cell. */
    int y = spacing;
    for (size_t r = 0; r < row_count; ++r) {
        Box *row = rows[r];
        calc_outlines(row, width);
        int x = spacing, tallest = 0, column = 0;
        for (Box *cell = row->first; cell && column < TABLE_COLUMNS_MAX; cell = cell->next) {
            if (!is_cell(cell)) continue;
            Style *s = cell->style;
            Len saved = s->width;
            s->width = (Len){(float)columns[column], U_PX};
            s->width.unit = U_AUTO;
            layout_box(layout, cell, table->x + x, table->y + y, columns[column], depth + 1);
            cell->width = columns[column] - offset_width(cell);
            s->width = saved;
            if (outer_height(cell) > tallest) tallest = outer_height(cell);
            x += columns[column] + spacing;
            ++column;
        }
        /* Every cell stretches to the row's height. */
        for (Box *cell = row->first; cell; cell = cell->next)
            if (is_cell(cell)) cell->height = tallest - offset_height(cell);
        row->x = table->x;
        row->y = table->y + y;
        row->width = x;
        row->height = tallest;
        y += tallest + spacing;
    }
    table->height = y;
    int used = spacing;
    for (int c = 0; c < column_count; ++c) used += columns[c] + spacing;
    return used;
}

/* ---- painting ----------------------------------------------------------- */

typedef struct {
    DRenderContext *renderer;
    int scroll_y;
    int band_height;
    /* Collecting text runs instead of painting. */
    struct TextRun { DTextBox box; DRenderFont *font; } *runs;
    size_t run_count, run_capacity;
    bool collecting;
} Painter;

static Rect border_box(const Box *b)
{
    return (Rect){b->x - b->padding[EDGE_LEFT] - b->border[EDGE_LEFT],
                  b->y - b->padding[EDGE_TOP] - b->border[EDGE_TOP],
                  b->width + b->padding[EDGE_LEFT] + b->padding[EDGE_RIGHT] + b->border[EDGE_LEFT] + b->border[EDGE_RIGHT],
                  b->height + b->padding[EDGE_TOP] + b->padding[EDGE_BOTTOM] + b->border[EDGE_TOP] + b->border[EDGE_BOTTOM]};
}

static DRenderRect on_surface(const Painter *p, Rect r)
{
    return (DRenderRect){r.x, r.y - p->scroll_y, r.width, r.height};
}

static void copy_utf8(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source ? source : "");
    if (length > capacity - 1) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char)source[length] & 0xC0) == 0x80) --length;
    }
    memcpy(destination, source ? source : "", length);
    destination[length] = '\0';
}

static const Box *run_block(const Box *box)
{
    for (const Box *up = box->parent; up; up = up->parent)
        if (up->block_id) return up;
    return NULL;
}

static void draw_text(Painter *p, const Box *owner, const char *text, const Style *style, Rect rect)
{
    if (p->collecting) {
        if (!text || !*text || rect.width <= 0 || rect.height <= 0 || p->run_count >= TEXT_RUNS_MAX) return;
        d_vec_reserve((void **)&p->runs, &p->run_capacity, p->run_count, sizeof(*p->runs), p->run_count + 1);
        struct TextRun *run = &p->runs[p->run_count++];
        memset(run, 0, sizeof(*run));
        run->box.x = rect.x;
        run->box.y = rect.y;
        run->box.width = rect.width;
        run->box.height = rect.height;
        copy_utf8(run->box.text, sizeof(run->box.text), text);
        const Box *block = run_block(owner);
        if (block) {
            run->box.block = (unsigned)block->block_id;
            copy_utf8(run->box.block_tag, sizeof(run->box.block_tag), block->tag);
        }
        run->font = style->font;
        return;
    }
    if (rect.y + rect.height < p->scroll_y || rect.y > p->scroll_y + p->band_height) return;
    d_render_draw_text(p->renderer, text, style->font, style->color, on_surface(p, rect));
}

static void draw_background(Layout *layout, Painter *p, const Box *box, Rect rect)
{
    const Style *s = box->style;
    if (p->collecting) return;
    if (rect.y > p->scroll_y + p->band_height || rect.y + rect.height < p->scroll_y) return;
    if (s->background.alpha) d_render_fill(p->renderer, on_surface(p, rect), s->background);
    if (s->background_image) {
        const DImage *image = image_for(layout, s->background_image);
        if (image) {
            int w = s->background_size[0].unit == U_AUTO ? image->width
                    : len_px(s->background_size[0], s->font_size, rect.width);
            int h = s->background_size[1].unit == U_AUTO
                    ? (s->background_size[0].unit == U_AUTO ? image->height : image->height * w / image->width)
                    : len_px(s->background_size[1], s->font_size, rect.height);
            d_render_set_clip(p->renderer, on_surface(p, rect));
            d_render_draw_image(p->renderer, on_surface(p, (Rect){rect.x, rect.y, w, h}),
                                image->pixels, image->width, image->height);
            d_render_del_clip(p->renderer);
        }
    }
}

static void draw_borders(const Painter *p, const Box *box, Rect r)
{
    if (p->collecting) return;
    const Style *s = box->style;
    const int *b = box->border;
    if (b[EDGE_TOP]) d_render_fill(p->renderer, on_surface(p, (Rect){r.x, r.y, r.width, b[EDGE_TOP]}), s->border_color[EDGE_TOP]);
    if (b[EDGE_BOTTOM]) d_render_fill(p->renderer, on_surface(p, (Rect){r.x, r.y + r.height - b[EDGE_BOTTOM], r.width, b[EDGE_BOTTOM]}), s->border_color[EDGE_BOTTOM]);
    if (b[EDGE_LEFT]) d_render_fill(p->renderer, on_surface(p, (Rect){r.x, r.y, b[EDGE_LEFT], r.height}), s->border_color[EDGE_LEFT]);
    if (b[EDGE_RIGHT]) d_render_fill(p->renderer, on_surface(p, (Rect){r.x + r.width - b[EDGE_RIGHT], r.y, b[EDGE_RIGHT], r.height}), s->border_color[EDGE_RIGHT]);
}

static void draw_marker(Painter *p, const Box *box)
{
    const Style *s = box->style;
    if (s->list_style == LIST_NONE) return;
    int size = s->font_size;
    if (s->list_style == LIST_DECIMAL) {
        char text[24];
        snprintf(text, sizeof(text), "%d.", box->list_index);
        int space = d_render_text_width(s->font, " ");
        int tw = d_render_text_width(s->font, text);
        Rect rect = {box->x - space * 2 + space - tw, box->y, tw, s->line_height};
        draw_text(p, box, text, s, rect);
        return;
    }
    if (p->collecting) return;
    int marker = size - size * 2 / 3;
    Rect rect = {box->x - size, box->y + s->line_height / 2 - marker / 2, marker, marker};
    d_render_draw_marker(p->renderer, on_surface(p, rect), s->color, false, -1);
}

static void paint_box(Layout *layout, Painter *p, const Box *box, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    if (!box->style->visible && box->kind != BOX_WORD && box->kind != BOX_SPACE) {
        for (const Box *child = box->first; child; child = child->next) paint_box(layout, p, child, depth + 1);
        return;
    }
    switch (box->kind) {
    case BOX_WORD:
    case BOX_SPACE:
        if (!box->skip && box->style->visible)
            draw_text(p, box, box->text, box->style, (Rect){box->x, box->y, box->width, box->height});
        return;
    case BOX_BREAK:
        return;
    default:
        break;
    }
    bool inline_element = box->kind == BOX_ELEMENT && box->style->display == DISPLAY_INLINE;
    if (inline_element) {
        for (int i = 0; i < box->fragment_count; ++i) draw_background(layout, p, box, box->fragments[i]);
    } else {
        Rect r = border_box(box);
        draw_background(layout, p, box, r);
        draw_borders(p, box, r);
        if (box->style->display == DISPLAY_LIST_ITEM) draw_marker(p, box);
        if (box->image && !p->collecting &&
            !(box->y > p->scroll_y + p->band_height || box->y + box->height < p->scroll_y))
            d_render_draw_image(p->renderer, on_surface(p, (Rect){box->x, box->y, box->width, box->height}),
                                box->image->pixels, box->image->width, box->image->height);
    }
    bool clip = box->style->overflow_hidden && !inline_element && !p->collecting;
    if (clip) {
        Rect r = border_box(box);
        Rect inner = {r.x + box->border[EDGE_LEFT], r.y + box->border[EDGE_TOP],
                      r.width - box->border[EDGE_LEFT] - box->border[EDGE_RIGHT],
                      r.height - box->border[EDGE_TOP] - box->border[EDGE_BOTTOM]};
        d_render_set_clip(p->renderer, on_surface(p, inner));
    }
    for (const Box *child = box->first; child; child = child->next) paint_box(layout, p, child, depth + 1);
    if (clip) d_render_del_clip(p->renderer);
}

/* Block boxes in document order, numbered from 1: the identity a text run
   carries so a selection can tell one paragraph from the next. */
static void number_blocks(Box *box, int *next, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    if (box->kind == BOX_ELEMENT && (box->style->display == DISPLAY_BLOCK ||
                                     box->style->display == DISPLAY_LIST_ITEM ||
                                     box->style->display == DISPLAY_TABLE)) {
        Rect r = border_box(box);
        if (r.width > 0 && r.height > 0) box->block_id = ++*next;
    }
    for (Box *child = box->first; child; child = child->next) number_blocks(child, next, depth + 1);
}

#ifdef DISTILL_LAYOUT_DEFECT
/* Negative control for the M4 parity gate: move the entire laid-out tree down
   one pixel. It is deliberately a layout defect, not a painter defect, and
   the bounded walk keeps the portability rule true in the test build too. */
static void break_layout(Box *box, int depth)
{
    if (!box || depth >= LAYOUT_DEPTH_MAX) return;
    ++box->y;
    for (int i = 0; i < box->fragment_count; ++i) ++box->fragments[i].y;
    for (Box *child = box->first; child; child = child->next) break_layout(child, depth + 1);
}
#endif

static void debug_boxes(const Box *box, int depth)
{
    if (depth > 30) return;
    if (box->kind == BOX_ELEMENT || box->kind == BOX_ANONYMOUS)
        fprintf(stderr, "%*s%s d%d x%d y%d w%d h%d m%d,%d,%d,%d p%d,%d b%d,%d\n", depth * 2, "",
                box->kind == BOX_ANONYMOUS ? "(anon)" : box->tag, box->style->display, box->x, box->y,
                box->width, box->height, box->margin[0], box->margin[1], box->margin[2], box->margin[3],
                box->padding[0], box->padding[2], box->border[0], box->border[2]);
    for (const Box *child = box->first; child; child = child->next) debug_boxes(child, depth + 1);
}

/* ---- the page ----------------------------------------------------------- */

struct DPage {
    Layout layout;
    DRenderContext *renderer;
    int width, surface_height;
    Painter runs;
    bool runs_collected;
};

static void free_images(Layout *layout)
{
    for (size_t i = 0; i < layout->image_count; ++i) {
        if (layout->images[i].ok) d_image_free(&layout->images[i].image);
        free(layout->images[i].path);
    }
    free(layout->images);
}

static void free_fragments(Box *box, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX + 2) return;
    free(box->fragments);
    for (Box *child = box->first; child; child = child->next) free_fragments(child, depth + 1);
}

static void collect_style_text(Sheet *sheet, Arena *arena, lxb_dom_node_t *node, int depth)
{
    if (depth > LAYOUT_DEPTH_MAX) return;
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *element = lxb_dom_interface_element(child);
        if (!d_casecmp(element_tag(element), "style")) {
            size_t length = 0;
            lxb_char_t *text = lxb_dom_node_text_content(child, &length);
            if (text) {
                char *css = arena_strndup(arena, (const char *)text, length);
                lxb_dom_document_destroy_text(child->owner_document, text);
                parse_css(arena, sheet, css);
            }
        } else {
            collect_style_text(sheet, arena, child, depth + 1);
        }
    }
}

DPage *d_page_create(const char *html, int width, int surface_height, const char *font_dir)
{
    if (!html || width <= 0 || surface_height <= 0) return NULL;
    DPage *page = d_xcalloc(1, sizeof(*page));
    page->renderer = d_render_create(width, surface_height, font_dir);
    if (!page->renderer) { free(page); return NULL; }
    page->width = width;
    page->surface_height = surface_height;
    Layout *layout = &page->layout;
    layout->width = width;
    layout->cascade.renderer = page->renderer;
    layout->cascade.viewport_width = width;
    layout->document = lxb_html_document_create();
    if (!layout->document ||
        lxb_html_document_parse(layout->document, (const lxb_char_t *)html, strlen(html)) != LXB_STATUS_OK) {
        d_page_destroy(page);
        return NULL;
    }
    parse_css(&layout->cascade.arena, &layout->cascade.sheet, DEFAULT_CSS);
    lxb_dom_node_t *document_node = lxb_dom_interface_node(layout->document);
    collect_style_text(&layout->cascade.sheet, &layout->cascade.arena, document_node, 0);
    lxb_dom_element_t *root_element = lxb_dom_document_element(lxb_dom_interface_document(layout->document));
    if (!root_element) { d_page_destroy(page); return NULL; }
    Style *initial = arena_alloc(&layout->cascade.arena, sizeof(*initial));
    initial->display = DISPLAY_BLOCK;
    initial->visible = true;
    initial->font_size = DEFAULT_FONT_SIZE;
    initial->font_weight = 400;
    initial->font_family = DEFAULT_FONT_FAMILY;
    initial->color = (DRenderColor){0, 0, 0, 255};
    initial->line_height_css = LEN_NONE;
    initial->border_spacing = 2;
    Box holder = {0};
    holder.style = initial;
    build_element(layout, &holder, root_element, 0);
    layout->root = holder.first;
    if (!layout->root) { d_page_destroy(page); return NULL; }
    layout->root->parent = NULL;
    prepare_tree(layout, layout->root, 0);
    layout_box(layout, layout->root, 0, 0, width, 0);
#ifdef DISTILL_LAYOUT_DEFECT
    break_layout(layout->root, 0);
#endif
    layout->content_height = outer_height(layout->root);
    if (getenv("DISTILL_LAYOUT_DEBUG")) debug_boxes(layout->root, 0);
    int next = 0;
    number_blocks(layout->root, &next, 0);
    return page;
}

void d_page_destroy(DPage *page)
{
    if (!page) return;
    if (page->layout.root) free_fragments(page->layout.root, 0);
    free(page->runs.runs);
    free_images(&page->layout);
    free(page->layout.cascade.sheet.rules);
    free(page->layout.cascade.sheet.variables);
    for (size_t i = 0; i < page->layout.cascade.font_count; ++i)
        d_render_font_destroy(page->layout.cascade.fonts[i].font);
    free(page->layout.cascade.fonts);
    arena_free(&page->layout.cascade.arena);
    if (page->layout.document) lxb_html_document_destroy(page->layout.document);
    d_render_destroy(page->renderer);
    free(page);
}

int d_page_height(const DPage *page)
{
    return page ? page->layout.content_height : 0;
}

int d_page_paint(DPage *page, int scroll_y)
{
    if (!page || !page->layout.root) return 0;
    if (scroll_y < 0) scroll_y = 0;
    d_render_clear(page->renderer);
    Painter painter = {page->renderer, scroll_y, page->surface_height, NULL, 0, 0, false};
    paint_box(&page->layout, &painter, page->layout.root, 0);
    return 1;
}

bool d_page_pixels(DPage *page, DRenderPixels *pixels)
{
    return page && d_render_pixels(page->renderer, pixels);
}

static void collect_runs(DPage *page)
{
    if (page->runs_collected || !page->layout.root) return;
    page->runs_collected = true;
    page->runs = (Painter){page->renderer, 0, page->layout.content_height, NULL, 0, 0, true};
    paint_box(&page->layout, &page->runs, page->layout.root, 0);
}

size_t d_page_text_boxes(DPage *page, DTextBox *boxes, size_t capacity)
{
    if (!page) return 0;
    collect_runs(page);
    size_t count = page->runs.run_count;
    for (size_t i = 0; boxes && i < count && i < capacity; ++i) boxes[i] = page->runs.runs[i].box;
    return count;
}

size_t d_page_text_offset_at(DPage *page, size_t index, int x)
{
    if (!page) return 0;
    collect_runs(page);
    if (index >= page->runs.run_count) return 0;
    return d_render_text_offset_at(page->runs.runs[index].font, page->runs.runs[index].box.text, x);
}

int d_page_text_x_at(DPage *page, size_t index, size_t offset)
{
    if (!page) return 0;
    collect_runs(page);
    if (index >= page->runs.run_count) return 0;
    return d_render_text_x_at(page->runs.runs[index].font, page->runs.runs[index].box.text, offset);
}

/* ---- what the shells ask of a laid-out page ----------------------------- */

static void copy_field(char *destination, size_t capacity, const char *value)
{
    snprintf(destination, capacity, "%s", value ? value : "");
}

static bool is_control(const char *tag)
{
    return !strcmp(tag, "input") || !strcmp(tag, "button") || !strcmp(tag, "select") || !strcmp(tag, "textarea");
}

/* The text of an element: every text node below it. */
static void element_text(lxb_dom_node_t *node, char *out, size_t capacity)
{
    size_t length = 0;
    lxb_char_t *text = lxb_dom_node_text_content(node, &length);
    copy_field(out, capacity, text ? (const char *)text : "");
    if (text) lxb_dom_document_destroy_text(node->owner_document, text);
}

static void append_piece(char *out, size_t capacity, size_t *used, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*used + 1 >= capacity) return;
        out[(*used)++] = (*p == 0x1e || *p == 0x1f) ? ' ' : (char)*p;
    }
    out[*used] = '\0';
}

static void append_separator(char *out, size_t capacity, size_t *used, char separator)
{
    if (*used + 1 >= capacity) return;
    out[(*used)++] = separator;
    out[*used] = '\0';
}

static void select_options(lxb_dom_node_t *node, DFormBox *box, size_t *used, int depth)
{
    if (depth > LAYOUT_DEPTH_MAX) return;
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *element = lxb_dom_interface_element(child);
        if (!d_casecmp(element_tag(element), "option")) {
            char label[512];
            element_text(child, label, sizeof(label));
            const char *raw = element_attr(element, "value");
            const char *value = raw && *raw ? raw : label;
            const char *flag = element_attr(element, "data-distill-selected");
            bool selected = flag && !strcmp(flag, "1");
            if (selected || !*box->value) copy_field(box->value, sizeof(box->value), value);
            append_piece(box->options, sizeof(box->options), used, value);
            append_separator(box->options, sizeof(box->options), used, '\x1f');
            append_piece(box->options, sizeof(box->options), used, label);
            append_separator(box->options, sizeof(box->options), used, '\x1f');
            append_piece(box->options, sizeof(box->options), used, selected ? "1" : "0");
            append_separator(box->options, sizeof(box->options), used, '\x1e');
        }
        select_options(child, box, used, depth + 1);
    }
}

static bool flag_set(lxb_dom_element_t *element, const char *name)
{
    const char *value = element_attr(element, name);
    return value && !strcmp(value, "1");
}

typedef struct {
    DFormBox *boxes;
    size_t count, capacity;
    unsigned next_form;
} FormCollect;

static void collect_forms(const Box *box, bool in_form, unsigned form, const char *action,
                          const char *method, FormCollect *out, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX || box->kind != BOX_ELEMENT) {
        if (box->kind == BOX_ANONYMOUS)
            for (const Box *child = box->first; child; child = child->next)
                collect_forms(child, in_form, form, action, method, out, depth + 1);
        return;
    }
    lxb_dom_element_t *element = box->element;
    if (!strcmp(box->tag, "form")) {
        in_form = true;
        form = out->next_form++;
        action = element_attr(element, "action");
        method = element_attr(element, "method");
        if (!method) method = "get";
    }
    if (in_form && is_control(box->tag)) {
        if (out->count < out->capacity) {
            DFormBox *f = &out->boxes[out->count];
            memset(f, 0, sizeof(*f));
            f->x = box->x;
            f->y = box->y;
            f->width = box->width;
            f->height = box->height;
            f->form_index = form;
            copy_field(f->tag, sizeof(f->tag), box->tag);
            const char *distill_type = element_attr(element, "data-distill-type");
            const char *type = element_attr(element, "type");
            copy_field(f->type, sizeof(f->type), distill_type && *distill_type ? distill_type : type ? type : "text");
            copy_field(f->id, sizeof(f->id), element_attr(element, "id"));
            copy_field(f->name, sizeof(f->name), element_attr(element, "name"));
            copy_field(f->value, sizeof(f->value), element_attr(element, "value"));
            copy_field(f->placeholder, sizeof(f->placeholder), element_attr(element, "placeholder"));
            f->checked = flag_set(element, "data-distill-checked");
            f->disabled = flag_set(element, "data-distill-disabled");
            f->required = flag_set(element, "data-distill-required");
            if (!strcmp(box->tag, "textarea")) element_text(lxb_dom_interface_node(element), f->value, sizeof(f->value));
            if (!strcmp(box->tag, "select")) {
                size_t used = 0;
                select_options(lxb_dom_interface_node(element), f, &used, 0);
            }
            if (!strcmp(box->tag, "button"))
                element_text(lxb_dom_interface_node(element), f->caption, sizeof(f->caption));
            copy_field(f->action, sizeof(f->action), action);
            copy_field(f->method, sizeof(f->method), method ? method : "get");
        }
        ++out->count;
    }
    for (const Box *child = box->first; child; child = child->next)
        collect_forms(child, in_form, form, action, method, out, depth + 1);
}

size_t d_page_form_boxes(const DPage *page, DFormBox *boxes, size_t capacity)
{
    if (!page || !page->layout.root) return 0;
    FormCollect collect = {boxes, 0, boxes ? capacity : 0, 0};
    collect_forms(page->layout.root, false, 0, "", "get", &collect, 0);
    return collect.count;
}

typedef struct {
    DLinkBox *boxes;
    size_t count, capacity;
} LinkCollect;

static void collect_links(const Box *box, LinkCollect *out, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return;
    if (box->kind == BOX_ELEMENT && !strcmp(box->tag, "a")) {
        const char *href = element_attr(box->element, "href");
        if (href && *href && box->width > 0 && box->height > 0) {
            if (out->count < out->capacity) {
                DLinkBox *l = &out->boxes[out->count];
                memset(l, 0, sizeof(*l));
                *l = (DLinkBox){.x = box->x, .y = box->y, .width = box->width, .height = box->height};
                copy_field(l->href, sizeof(l->href), href);
            }
            ++out->count;
        }
    }
    for (const Box *child = box->first; child; child = child->next) collect_links(child, out, depth + 1);
}

size_t d_page_link_boxes(const DPage *page, DLinkBox *boxes, size_t capacity)
{
    if (!page || !page->layout.root) return 0;
    LinkCollect collect = {boxes, 0, boxes ? capacity : 0};
    collect_links(page->layout.root, &collect, 0);
    return collect.count;
}

static bool rect_holds(Rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

/* The innermost box under a point: text and atomic boxes by their own
   rectangles, inline elements by their boxes on each line. */
static const Box *box_at(const Box *box, int x, int y, int depth)
{
    if (depth >= LAYOUT_DEPTH_MAX) return NULL;
    const Box *found = NULL;
    for (const Box *child = box->first; child; child = child->next) {
        const Box *inner = box_at(child, x, y, depth + 1);
        if (inner) found = inner;
    }
    if (found) return found;
    if (box->kind == BOX_WORD || box->kind == BOX_SPACE) {
        if (!box->skip && rect_holds((Rect){box->x, box->y, box->width, box->height}, x, y)) return box;
        return NULL;
    }
    if (box->kind == BOX_ELEMENT && box->style->display == DISPLAY_INLINE) {
        for (int i = 0; i < box->fragment_count; ++i)
            if (rect_holds(box->fragments[i], x, y)) return box;
        return NULL;
    }
    if (box->kind == BOX_ELEMENT && rect_holds(border_box(box), x, y)) return box;
    return NULL;
}

int d_page_link_at(DPage *page, int x, int y, char *href_out, size_t capacity)
{
    if (href_out && capacity) href_out[0] = '\0';
    if (!page || !page->layout.root) return 0;
    for (const Box *box = box_at(page->layout.root, x, y, 0); box; box = box->parent) {
        if (box->kind != BOX_ELEMENT || strcmp(box->tag, "a")) continue;
        const char *href = element_attr(box->element, "href");
        if (!href || !*href) continue;
        if (href_out && capacity) copy_field(href_out, capacity, href);
        return 1;
    }
    return 0;
}

static lxb_dom_element_t *choice_input(lxb_dom_node_t *node, int depth)
{
    if (depth > LAYOUT_DEPTH_MAX) return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t *element = lxb_dom_interface_element(node);
        const char *type = element_attr(element, "type");
        if (!d_casecmp(element_tag(element), "input") && type &&
            (!strcmp(type, "checkbox") || !strcmp(type, "radio")))
            return element;
    }
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next) {
        lxb_dom_element_t *found = choice_input(child, depth + 1);
        if (found) return found;
    }
    return NULL;
}

int d_page_choice_id_at(DPage *page, int x, int y, char *id_out, size_t capacity)
{
    if (id_out && capacity) id_out[0] = '\0';
    if (!page || !page->layout.root) return 0;
    for (const Box *box = box_at(page->layout.root, x, y, 0); box; box = box->parent) {
        if (box->kind != BOX_ELEMENT || strcmp(box->tag, "label")) continue;
        lxb_dom_element_t *input = choice_input(lxb_dom_interface_node(box->element), 0);
        const char *id = input ? element_attr(input, "id") : NULL;
        if (!id || !*id) return 0;
        if (id_out && capacity) copy_field(id_out, capacity, id);
        return 1;
    }
    return 0;
}

int d_page_measure_height(const char *html, int width, const char *font_dir)
{
    DPage *page = d_page_create(html, width, 1, font_dir);
    if (!page) return 0;
    int height = d_page_height(page);
    d_page_destroy(page);
    return height;
}

int d_page_render_png(const char *html, const char *output_path, int width, int height,
                      const char *font_dir)
{
    if (!html || !output_path || width <= 0) return 0;
    if (height <= 0) {
        height = d_page_measure_height(html, width, font_dir);
        if (height < 1) height = 1;
        if (height > DISTILL_MAX_RENDER_HEIGHT) height = DISTILL_MAX_RENDER_HEIGHT;
    }
    DPage *page = d_page_create(html, width, height, font_dir);
    if (!page) return 0;
    int ok = d_page_paint(page, 0) && d_render_write_png(page->renderer, output_path);
    d_page_destroy(page);
    return ok;
}

int d_page_render_region_png(const char *html, const char *output_path, int width, int viewport,
                             int scroll_y, const char *font_dir)
{
    if (!html || !output_path || width <= 0 || viewport <= 0) return 0;
    DPage *page = d_page_create(html, width, viewport, font_dir);
    if (!page) return 0;
    int ok = d_page_paint(page, scroll_y) && d_render_write_png(page->renderer, output_path);
    d_page_destroy(page);
    return ok;
}

size_t d_page_measure_form_boxes(const char *html, int width, int height, const char *font_dir,
                                 DFormBox *boxes, size_t capacity)
{
    if (!html || width <= 0 || height <= 0) return 0;
    DPage *page = d_page_create(html, width, height, font_dir);
    if (!page) return 0;
    size_t count = d_page_form_boxes(page, boxes, capacity);
    d_page_destroy(page);
    return count;
}

size_t d_page_measure_link_boxes(const char *html, int width, int height, const char *font_dir,
                                 DLinkBox *boxes, size_t capacity)
{
    if (!html || width <= 0 || height <= 0) return 0;
    DPage *page = d_page_create(html, width, height, font_dir);
    if (!page) return 0;
    size_t count = d_page_link_boxes(page, boxes, capacity);
    d_page_destroy(page);
    return count;
}
