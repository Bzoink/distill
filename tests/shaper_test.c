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
    /* A line range runs to the next break opportunity, so the space that
       caused the break belongs to the line before it. Painting trims it;
       the ranges do not. */
    assert(lines[0].end - lines[0].begin == 8);
    assert(strncmp(text + lines[0].begin, "one two ", 8) == 0);
    assert(lines[1].end - lines[1].begin == 6);
    assert(strncmp(text + lines[1].begin, "three ", 6) == 0);
    assert(lines[2].end - lines[2].begin == 4);
    assert(strncmp(text + lines[2].begin, "four", 4) == 0);
    return 0;
}
