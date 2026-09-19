#ifndef DISTILL_DOM_UTIL_H
#define DISTILL_DOM_UTIL_H

/* Reading a Lexbor tree: tags, attributes, a small selector matcher and
   whitespace-collapsed text. Shared by the extractors, which read the page's
   own post-script DOM, and by the reader-page walker, which reads the page
   Distill wrote. Header-only in the manner of cutil.h, so each reader keeps
   the inlining it had when these lived beside it. */

#include "cutil.h"

#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>

typedef struct {
    lxb_dom_node_t **items;
    size_t length;
    size_t capacity;
} NodeVec;

static inline void node_vec_push(NodeVec *vec, lxb_dom_node_t *node)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = node;
}

static inline bool is_element(lxb_dom_node_t *node)
{
    return node && node->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

/* lxb_dom_node_text_content() returns a fresh copy in the document's text
   arena, which lives until the document is destroyed unless it is handed
   back. Extraction asks for text inside loops over every element, so a copy
   that is never returned costs the page's text many times over: a 275 KB BBC
   front page grew the engine by 130 MiB this way. */
static inline char *node_text(lxb_dom_node_t *node)
{
    size_t length = 0;
    lxb_char_t *value = lxb_dom_node_text_content(node, &length);
    if (!value) return d_strdup("");
    char *text = d_strndup((const char *)value, length);
    lxb_dom_document_destroy_text(node->owner_document, value);
    return text;
}

static inline char *node_tag(lxb_dom_node_t *node)
{
    if (!is_element(node)) return d_strdup("");
    const lxb_char_t *name = lxb_dom_element_local_name(lxb_dom_interface_element(node), NULL);
    return d_lower(name ? (const char *)name : "");
}

static inline char *attr_value(lxb_dom_element_t *element, const char *name)
{
    if (!element) return d_strdup("");
    size_t length = 0;
    const lxb_char_t *value = lxb_dom_element_get_attribute(element,
        (const lxb_char_t *)name, strlen(name), &length);
    return value ? d_strndup((const char *)value, length) : d_strdup("");
}

static inline bool has_attr(lxb_dom_element_t *element, const char *name)
{
    return lxb_dom_element_has_attribute(element, (const lxb_char_t *)name, strlen(name));
}

static inline bool class_has(lxb_dom_element_t *element, const char *name)
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

static inline bool selector_match(lxb_dom_node_t *node, const char *raw_selector)
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

static inline void walk_nodes(lxb_dom_node_t *node, NodeVisit visit, void *opaque)
{
    if (!node) return;
    visit(node, opaque);
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next)
        walk_nodes(child, visit, opaque);
}

static inline void find_walk(lxb_dom_node_t *node, void *opaque)
{
    FindContext *context = opaque;
    if (selector_match(node, context->selector)) node_vec_push(context->out, node);
}

static inline NodeVec find_selector(lxb_dom_node_t *root, const char *selector)
{
    NodeVec out = {0};
    FindContext context = {selector, &out};
    walk_nodes(root, find_walk, &context);
    return out;
}

static inline char *compact_text(lxb_dom_node_t *node)
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

#endif
