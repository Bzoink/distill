#ifndef DISTILL_CUTIL_H
#define DISTILL_CUTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *d_xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) { fputs("distill: out of memory\n", stderr); abort(); }
    return p;
}

static inline void *d_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) { fputs("distill: out of memory\n", stderr); abort(); }
    return p;
}

static inline void *d_xrealloc(void *old, size_t size)
{
    void *p = realloc(old, size ? size : 1);
    if (!p) { fputs("distill: out of memory\n", stderr); abort(); }
    return p;
}

static inline char *d_strndup(const char *src, size_t len)
{
    char *out = d_xmalloc(len + 1);
    if (src && len) memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static inline char *d_strdup(const char *src)
{
    return d_strndup(src ? src : "", src ? strlen(src) : 0);
}

static inline char *d_cat2(const char *a, const char *b)
{
    size_t al = a ? strlen(a) : 0, bl = b ? strlen(b) : 0;
    char *out = d_xmalloc(al + bl + 1);
    if (al) memcpy(out, a, al);
    if (bl) memcpy(out + al, b, bl);
    out[al + bl] = '\0';
    return out;
}

static inline char *d_cat3(const char *a, const char *b, const char *c)
{
    char *ab = d_cat2(a, b);
    char *out = d_cat2(ab, c);
    free(ab);
    return out;
}

static inline char *d_cat4(const char *a, const char *b, const char *c, const char *d)
{
    char *abc = d_cat3(a, b, c);
    char *out = d_cat2(abc, d);
    free(abc);
    return out;
}

static inline void d_vec_reserve(void **data, size_t *capacity, size_t length,
                                 size_t element_size, size_t wanted)
{
    if (wanted <= *capacity) return;
    size_t cap = *capacity ? *capacity : 8;
    while (cap < wanted) cap *= 2;
    (void)length;
    *data = d_xrealloc(*data, cap * element_size);
    *capacity = cap;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} DBuffer;

static inline void dbuf_init(DBuffer *buffer)
{
    *buffer = (DBuffer){0};
}

static inline void dbuf_reserve(DBuffer *buffer, size_t extra)
{
    d_vec_reserve((void **)&buffer->data, &buffer->capacity, buffer->length,
                  sizeof(char), buffer->length + extra + 1);
}

static inline void dbuf_append(DBuffer *buffer, const char *data, size_t length)
{
    dbuf_reserve(buffer, length);
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static inline void dbuf_puts(DBuffer *buffer, const char *text)
{
    dbuf_append(buffer, text ? text : "", text ? strlen(text) : 0);
}

static inline void dbuf_printf(DBuffer *buffer, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed > 0) {
        dbuf_reserve(buffer, (size_t)needed);
        vsnprintf(buffer->data + buffer->length, (size_t)needed + 1, format, ap);
        buffer->length += (size_t)needed;
    }
    va_end(ap);
}

static inline char *dbuf_take(DBuffer *buffer)
{
    if (!buffer->data) return d_strdup("");
    char *out = buffer->data;
    *buffer = (DBuffer){0};
    return out;
}

static inline void dbuf_free(DBuffer *buffer)
{
    free(buffer->data);
    *buffer = (DBuffer){0};
}

static inline char *d_lower(const char *src)
{
    char *out = d_strdup(src);
    for (char *p = out; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    return out;
}

static inline char *d_trim(const char *src)
{
    if (!src) return d_strdup("");
    size_t begin = 0, end = strlen(src);
    while (begin < end && (unsigned char)src[begin] <= ' ') ++begin;
    while (end > begin && (unsigned char)src[end - 1] <= ' ') --end;
    return d_strndup(src + begin, end - begin);
}

static inline bool d_starts(const char *src, const char *prefix)
{
    return src && prefix && strncmp(src, prefix, strlen(prefix)) == 0;
}

static inline bool d_eq(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

/* ASCII case-insensitive ordering, as strcasecmp and strncasecmp give in the
   C locale. Those are POSIX, not C; what is compared with these is protocol
   text -- schemes, types, header names -- where ASCII folding is the rule. */
static inline int d_ncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
    return 0;
}

static inline int d_casecmp(const char *a, const char *b)
{
    return d_ncasecmp(a, b, SIZE_MAX);
}

static inline bool d_ieq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

static inline bool d_contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static inline bool d_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static inline char *d_read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *data = d_xmalloc((size_t)size + 1);
    size_t got = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[got] = '\0';
    if (length) *length = got;
    return data;
}

#endif
