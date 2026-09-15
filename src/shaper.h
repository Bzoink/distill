#ifndef DISTILL_SHAPER_H
#define DISTILL_SHAPER_H

#include <stddef.h>

typedef int (*DTextMeasure)(void *opaque, const char *text, size_t length);

typedef struct {
    size_t begin;
    size_t end;
} DTextLine;

/* UTF-8 byte ranges produced by UAX #14 opportunities and a width callback. */
size_t d_text_wrap_utf8(const char *text, size_t length, int max_width,
                        DTextMeasure measure, void *opaque,
                        DTextLine *lines, size_t line_capacity);

#endif
