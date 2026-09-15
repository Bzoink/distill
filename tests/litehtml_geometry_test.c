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

    const char *long_page =
        "<!doctype html><style>body{font-size:18px}p{margin:0 0 120px}</style>"
        "<main><p>top</p><p>middle</p><p>bottom</p></main>";
    DLiteHtmlDocument *document = distill_litehtml_document_create(long_page, 760, 240, NULL);
    assert(document);
    assert(distill_litehtml_document_content_height(document) > 240);
    assert(distill_litehtml_document_surface(document) != NULL);
    assert(distill_litehtml_document_render_region(document, 0));
    assert(distill_litehtml_document_render_region(document, 180));
    distill_litehtml_document_destroy(document);
    assert(distill_litehtml_render_region(long_page, "/tmp/distill-region-test.png",
                                          760, 240, 180, NULL));
    assert(remove("/tmp/distill-region-test.png") == 0);

    puts("litehtml-geometry: ok");
    return 0;
}
