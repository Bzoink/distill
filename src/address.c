#define _POSIX_C_SOURCE 200809L

#include "address.h"

#include "cutil.h"
#include "form_submit.h"

#include <strings.h>

static bool has_scheme(const char *uri, const char *scheme)
{
    size_t length = strlen(scheme);
    return uri && strncasecmp(uri, scheme, length) == 0 && uri[length] == ':';
}

static bool ascii_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool d_address_looks_like_url(const char *text)
{
    if (!text || !*text) return false;
    if (has_scheme(text, "http") || has_scheme(text, "https") || has_scheme(text, "file")) return true;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t') return false;
    }
    if (text[0] == '/' || text[0] == '~' ||
        strncmp(text, "./", 2) == 0 || strncmp(text, "../", 3) == 0) return true;
    /* A scheme we do not handle is not a search term either; let the
       navigation check reject it by name. */
    const char *colon = strchr(text, ':');
    const char *slash = strchr(text, '/');
    if (colon && (!slash || colon < slash)) {
        bool port = true;
        for (const char *p = colon + 1; *p && *p != '/'; ++p) {
            if (*p < '0' || *p > '9') { port = false; break; }
        }
        if (!port) return false;
    }
    const char *host_end = strpbrk(text, "/?#");
    size_t host_len = host_end ? (size_t)(host_end - text) : strlen(text);
    char *host = d_strndup(text, host_len);
    if (colon && colon < text + host_len) host[colon - text] = '\0';
    bool ok = false;
    if (strcasecmp(host, "localhost") == 0) {
        ok = true;
    } else {
        const char *dot = strrchr(host, '.');
        if (dot && dot != host && dot[1]) {
            /* A top-level domain is alphabetic, which is what separates a
               hostname from a version number: "3.14" is not a host. An
               address is the other accepted form, and must be four numeric
               labels rather than merely digits and dots. */
            bool alpha = true;
            size_t label = 0;
            for (const char *p = dot + 1; *p; ++p, ++label) {
                if (!ascii_alpha(*p)) alpha = false;
            }
            ok = alpha && label >= 2;
            if (!ok && strspn(host, "0123456789.") == strlen(host)) {
                size_t count = 0;
                bool address = true;
                const char *part = host;
                for (;;) {
                    const char *end = strchr(part, '.');
                    size_t length = end ? (size_t)(end - part) : strlen(part);
                    if (!length || length > 3) address = false;
                    ++count;
                    if (!end) break;
                    part = end + 1;
                }
                ok = address && count == 4;
            }
        }
    }
    free(host);
    return ok;
}

static char *trimmed(const char *raw)
{
    const char *start = raw ? raw : "";
    while (*start && d_isspace(*start)) ++start;
    size_t length = strlen(start);
    while (length && d_isspace(start[length - 1])) --length;
    return d_strndup(start, length);
}

char *d_address_resolve(const char *raw, bool *https_guessed)
{
    if (https_guessed) *https_guessed = false;
    char *text = trimmed(raw);
    if (!*text) { free(text); return NULL; }
    if (d_address_looks_like_url(text)) {
        if (has_scheme(text, "http") || has_scheme(text, "https") || has_scheme(text, "file") ||
            text[0] == '/' || text[0] == '~' ||
            strncmp(text, "./", 2) == 0 || strncmp(text, "../", 3) == 0) {
            return text;
        }
        /* Loopback names have no certificate to present, so asking for https
           there fails for a reason that has nothing to do with the request. */
        bool loopback = strncasecmp(text, "localhost", 9) == 0 ||
                        strncasecmp(text, "127.0.0.1", 9) == 0;
        if (https_guessed) *https_guessed = !loopback;
        char *url = d_cat2(loopback ? "http://" : "https://", text);
        free(text);
        return url;
    }
    const char *engine = getenv("DISTILL_SEARCH_URL");
    if (!engine || !*engine) engine = DISTILL_DEFAULT_SEARCH_URL;
    char *query = d_form_encode(text);
    char *url = d_cat2(engine, query);
    free(query);
    free(text);
    return url;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *d_uri_unescape(const char *text)
{
    DBuffer out;
    dbuf_init(&out);
    for (const char *p = text ? text : ""; *p; ++p) {
        if (*p != '%') { dbuf_append(&out, p, 1); continue; }
        int high = p[1] ? hex_value(p[1]) : -1;
        int low = high >= 0 && p[2] ? hex_value(p[2]) : -1;
        if (high < 0 || low < 0 || (high == 0 && low == 0)) { dbuf_free(&out); return NULL; }
        char byte = (char)(high * 16 + low);
        dbuf_append(&out, &byte, 1);
        p += 2;
    }
    if (!out.data) dbuf_puts(&out, "");
    return dbuf_take(&out);
}

char *d_address_unwrap_redirect(const char *uri)
{
    if (!uri) return NULL;
    const char *host = strstr(uri, "duckduckgo.com/l/");
    if (!host) return NULL;
    const char *query = strchr(host, '?');
    if (!query) return NULL;
    for (const char *p = query + 1; *p; ) {
        const char *end = strchr(p, '&');
        size_t span = end ? (size_t)(end - p) : strlen(p);
        if (span > 5 && strncmp(p, "uddg=", 5) == 0) {
            char *encoded = d_strndup(p + 5, span - 5);
            char *target = d_uri_unescape(encoded);
            free(encoded);
            if (target && (has_scheme(target, "http") || has_scheme(target, "https"))) return target;
            free(target);
            return NULL;
        }
        if (!end) break;
        p = end + 1;
    }
    return NULL;
}
