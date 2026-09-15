#define _POSIX_C_SOURCE 200809L

#include "shaper.h"

#include <linebreak.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool is_break_boundary(char value)
{
    return value == LINEBREAK_ALLOWBREAK || value == LINEBREAK_MUSTBREAK ||
           value == LINEBREAK_INDETERMINATE;
}

static int measure_range(const char *text, size_t begin, size_t end,
                         DTextMeasure measure, void *opaque)
{
    size_t length = end - begin;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text + begin, length);
    copy[length] = '\0';
    int width = measure(opaque, copy, length);
    free(copy);
    return width;
}

size_t d_text_wrap_utf8(const char *text, size_t length, int max_width,
                        DTextMeasure measure, void *opaque,
                        DTextLine *lines, size_t line_capacity)
{
    if (!text || !length || !max_width || !measure || !lines || !line_capacity) return 0;
    char *breaks = malloc(length);
    if (!breaks) return 0;
    init_linebreak();
    set_linebreaks_utf8((const utf8_t *)text, length, "en", breaks);

    size_t line_count = 0;
    size_t begin = 0;
    while (begin < length && line_count < line_capacity) {
        size_t cursor = begin;
        size_t last_break = begin;
        size_t forced_break = 0;
        while (cursor < length) {
            ++cursor;
            if (cursor < length && !is_break_boundary(breaks[cursor - 1])) continue;
            int width = measure_range(text, begin, cursor, measure, opaque);
            if (width <= max_width || cursor == begin + 1) {
                if (cursor == length || breaks[cursor - 1] == LINEBREAK_MUSTBREAK)
                    forced_break = cursor;
                if (cursor < length && is_break_boundary(breaks[cursor - 1]))
                    last_break = cursor;
                if (cursor == length) break;
                continue;
            }
            break;
        }

        size_t end = forced_break ? forced_break : last_break;
        if (end <= begin) {
            end = cursor > begin ? cursor : begin + 1;
            while (end < length && (breaks[end - 1] == LINEBREAK_INSIDEACHAR)) ++end;
        }
        lines[line_count++] = (DTextLine){begin, end};
        begin = end;
        while (begin < length && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    }
    if (begin < length && line_count == line_capacity) {
        lines[line_count - 1].end = length;
    }
    free(breaks);
    return line_count;
}
