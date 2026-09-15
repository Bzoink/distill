#include "shaper.h"

#include <assert.h>
#include <string.h>

static int character_measure(void *opaque, const char *text, size_t length)
{
    (void)opaque;
    (void)text;
    return (int)(length * 10);
}

int main(void)
{
    const char *text = "one two three four";
    DTextLine lines[8] = {0};
    size_t count = d_text_wrap_utf8(text, strlen(text), 80, character_measure, NULL,
                                    lines, 8);
    assert(count == 3);
    assert(strncmp(text + lines[0].begin, "one two", lines[0].end - lines[0].begin) == 0);
    assert(strncmp(text + lines[1].begin, "three", lines[1].end - lines[1].begin) == 0);
    assert(strncmp(text + lines[2].begin, "four", lines[2].end - lines[2].begin) == 0);
    return 0;
}
