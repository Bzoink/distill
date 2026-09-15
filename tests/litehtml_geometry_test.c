#include "render.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *html =
        "<!doctype html><style>body{font-size:18px}input{width:240px}</style>"
        "<main><form action=\"https://example.test/search\" method=\"get\">"
        "<label>Query<input type=\"search\" name=\"q\" value=\"reader\"></label>"
        "<input type=\"hidden\" name=\"token\" value=\"keep\">"
        "<button type=\"submit\" name=\"go\">Search</button></form></main>";
    size_t count = distill_litehtml_form_boxes(html, 760, 800, NULL, NULL, 0);
    assert(count == 3);
    DFormBox boxes[3] = {0};
    assert(distill_litehtml_form_boxes(html, 760, 800, NULL, boxes, 3) == 3);
    assert(strcmp(boxes[0].type, "search") == 0);
    assert(strcmp(boxes[0].name, "q") == 0);
    assert(boxes[0].width > 0 && boxes[0].height > 0);
    assert(strcmp(boxes[1].type, "hidden") == 0);
    assert(strcmp(boxes[1].value, "keep") == 0);
    assert(strcmp(boxes[2].tag, "button") == 0);
    assert(strcmp(boxes[2].value, "Search") == 0);
    assert(strcmp(boxes[2].action, "https://example.test/search") == 0);
    puts("litehtml-geometry: ok");
    return 0;
}
