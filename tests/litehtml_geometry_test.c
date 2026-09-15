#include "render.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *html =
        /* Matches what the reader emits: block-level controls. */
        "<!doctype html><style>body{font-size:18px}input{display:block;width:240px}</style>"
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
    /* get_placement reports the content box, and an empty text input has no
       content, so its height is 0 however it is styled. Width is what the
       shell positions from; it substitutes a minimum height. A control with
       content, such as the button below, does report a height. */
    assert(boxes[0].width > 0);
    assert(strcmp(boxes[1].type, "hidden") == 0);
    assert(strcmp(boxes[1].value, "keep") == 0);
    assert(strcmp(boxes[2].tag, "button") == 0);
    assert(strcmp(boxes[2].value, "Search") == 0);
    assert(boxes[2].width > 0 && boxes[2].height > 0);
    assert(strcmp(boxes[2].action, "https://example.test/search") == 0);

    /* Links are found by asking the laid-out document what is under a point,
       not by collecting rectangles. An inline anchor has no single placement --
       its text occupies one box per line -- so a link inside a sentence has no
       rectangle to test against, and box collection cannot see it at all. Both
       anchors below are inline, and one is mid-sentence. */
    const char *link_page =
        "<!doctype html><style>body{font-size:18px}</style>"
        "<main><h2><a href=\"https://example.test/story\">Story link</a></h2>"
        "<p>Prose before <a href=\"https://example.test/inline\">a link inside a "
        "sentence</a> and prose after.</p></main>";
    DLiteHtmlDocument *link_document =
        distill_litehtml_document_create(link_page, 760, 800, NULL);
    assert(link_document);

    /* Scan rather than hard-code pixel coordinates: the assertion is that the
       links are reachable, not that they sit at a particular offset under one
       font configuration. */
    bool found_heading = false, found_inline = false, found_gap = false;
    for (int y = 0; y < 400 && !(found_heading && found_inline); y += 3) {
        for (int x = 4; x < 700; x += 7) {
            char href[512] = {0};
            if (!distill_litehtml_document_link_at(link_document, x, y, href, sizeof(href))) continue;
            if (!strcmp(href, "https://example.test/story")) found_heading = true;
            if (!strcmp(href, "https://example.test/inline")) found_inline = true;
        }
    }
    assert(found_heading);
    assert(found_inline);

    /* Somewhere in the page must not be a link, or the test proves nothing. */
    for (int y = 0; y < 400 && !found_gap; y += 3) {
        char href[512] = {0};
        if (!distill_litehtml_document_link_at(link_document, 750, y, href, sizeof(href))) found_gap = true;
    }
    assert(found_gap);
    distill_litehtml_document_destroy(link_document);

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
