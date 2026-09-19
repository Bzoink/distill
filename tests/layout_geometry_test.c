#include "image.h"
#include "page.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DISTILL_TEST_FONT_DIR
#define DISTILL_TEST_FONT_DIR "../external/fonts"
#endif

/* Write a solid-colour PNG, so a render of it can be checked pixel by pixel.
   Hand-built rather than encoded with a library, to keep this test's only
   dependency the one it is testing. */
static bool write_solid_png(const char *path, int width, int height,
                            unsigned char red, unsigned char green, unsigned char blue);

/* The image path is worth a pixel-level assertion rather than a "did it draw
   something" one. Its first bug was an offset: the layout reports a resolved
   document position for a background, and adding that to the origin box drew
   every image at twice its own offset -- which looks like a partly clipped
   image, not like a wrong coordinate, and a smoke test would have passed. */
static void test_image_is_drawn_where_it_is_laid_out(void)
{
    const char *source = "/tmp/distill-image-geometry-source.png";
    const char *output = "/tmp/distill-image-geometry-out.png";
    assert(write_solid_png(source, 120, 60, 0xff, 0x00, 0x00));

    /* A 40px band above the image, and the image hard against the left edge,
       so the expected rectangle is arithmetic rather than guesswork. */
    char html[512];
    snprintf(html, sizeof(html),
             "<!doctype html><html><head><style>"
             "html,body{margin:0;padding:0;background:#ffffff;}"
             "div.spacer{height:40px;}img{display:block;}"
             "</style></head><body><div class=\"spacer\"></div>"
             "<img src=\"%s\"></body></html>", source);
    assert(d_page_render_region_png(html, output, 400, 200, 0, NULL));

    DImage rendered;
    assert(d_image_load_file(output, &rendered));
    assert(rendered.width == 400 && rendered.height == 200);

    /* Inside the image: red. The two corners catch an offset in either axis
       and a size that is too small. */
    uint32_t top_left = rendered.pixels[(size_t)41 * 400 + 1];
    uint32_t bottom_right = rendered.pixels[(size_t)98 * 400 + 118];
    assert((top_left >> 16 & 0xff) > 200);
    assert((top_left >> 8 & 0xff) < 60);
    assert((bottom_right >> 16 & 0xff) > 200);
    assert((bottom_right >> 8 & 0xff) < 60);

    /* Above it, and to the right of it, the page background. An image drawn
       at the wrong offset fails one of these rather than merely looking odd. */
    uint32_t above = rendered.pixels[(size_t)10 * 400 + 10];
    uint32_t beside = rendered.pixels[(size_t)60 * 400 + 300];
    uint32_t below = rendered.pixels[(size_t)150 * 400 + 10];
    assert((above >> 16 & 0xff) > 200 && (above >> 8 & 0xff) > 200);
    assert((beside >> 16 & 0xff) > 200 && (beside >> 8 & 0xff) > 200);
    assert((below >> 16 & 0xff) > 200 && (below >> 8 & 0xff) > 200);

    d_image_free(&rendered);
    assert(remove(source) == 0);
    assert(remove(output) == 0);
}

/* An image wider than the surface is fitted to it rather than overflowing:
   the renderer decides that, because an <img> carrying an explicit CSS size
   reserves its box and is never painted by this layout. */
static void test_wide_image_is_fitted_to_the_surface(void)
{
    const char *source = "/tmp/distill-image-wide-source.png";
    assert(write_solid_png(source, 900, 300, 0x00, 0x80, 0x00));
    char html[512];
    snprintf(html, sizeof(html),
             "<!doctype html><html><head><style>html,body{margin:0;padding:0;}"
             "img{display:block;}</style></head><body><img src=\"%s\"></body></html>",
             source);
    /* 900 wide fitted into 300 is 100 tall, so the content is about that and
       certainly not the 300 it would be at natural size. */
    int height = d_page_measure_height(html, 300, NULL);
    assert(height > 0);
    assert(height < 200);
    assert(remove(source) == 0);
}

/* An image the renderer cannot read must not be drawn and must not take up
   space as though it had been: a missing file is not a 300px hole. */
static void test_unreadable_image_takes_no_space(void)
{
    const char *html =
        "<!doctype html><html><head><style>html,body{margin:0;padding:0;}"
        "</style></head><body><img src=\"/tmp/distill-does-not-exist.png\"></body></html>";
    int height = d_page_measure_height(html, 400, NULL);
    assert(height >= 0 && height < 40);
}

/* The renderer never fetches. A remote source is refused here rather than
   turned into a request, which is what keeps the network on the engine's side
   of the process boundary. */
static void test_remote_sources_are_refused(void)
{
    const char *html =
        "<!doctype html><html><head><style>html,body{margin:0;padding:0;}"
        "</style></head><body><img src=\"https://example.test/photo.png\"></body></html>";
    int height = d_page_measure_height(html, 400, NULL);
    assert(height >= 0 && height < 40);
}

static void test_bundled_font_roles_are_available(void)
{
    DRenderContext *context = d_render_create(760, 240, DISTILL_TEST_FONT_DIR);
    assert(context);
    const char *families[] = {"Source Serif 4", "Source Sans 3", "Source Code Pro"};
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
        DRenderFont *font = d_render_font_create(context, families[i], 18, 400,
                                                 false, 0, NULL);
        assert(font);
        assert(d_render_text_width(font, "Typography role sample") > 0);
        d_render_font_destroy(font);
    }
    d_render_destroy(context);
}

int main(void)
{
    const char *html =
        /* Matches the reader form structure: the label and control are
           sibling blocks, so the control starts below and at the label's
           left edge instead of becoming inline label content. */
        "<!doctype html><style>html,body{margin:0}body{font-size:18px}"
        ".form-field-label{display:block;text-align:left}"
        ".form-field-input{display:block;width:240px;height:28px;margin:3px 0 0;"
        "padding:10px;border:1px solid #000}.form-card form input{padding:0;border-width:0}</style>"
        "<main class=\"form-card\"><form action=\"https://example.test/search\" method=\"get\">"
        "<label class=\"form-field-label\" for=\"query\">Query</label>"
        "<input id=\"query\" class=\"form-field-input\" type=\"search\""
        " name=\"q\" value=\"reader\">"
        "<input type=\"hidden\" name=\"token\" value=\"keep\">"
        "<button type=\"submit\" name=\"go\">Search</button></form></main>";
    size_t count = d_page_measure_form_boxes(html, 760, 800, NULL, NULL, 0);
    assert(count == 3);
    DFormBox boxes[3] = {0};
    assert(d_page_measure_form_boxes(html, 760, 800, NULL, boxes, 3) == 3);
    assert(strcmp(boxes[0].type, "search") == 0);
    assert(strcmp(boxes[0].name, "q") == 0);
    /* The reader gives every input the same explicit height as its GTK
       overlay. That makes the layout reserve the vertical space rather than
       putting the next label where an empty input's zero-height content box
       would be. */
    assert(boxes[0].width > 0);
    assert(boxes[0].height == 28);
    assert(boxes[0].x == 0);
    assert(boxes[0].y > 18);
    assert(strcmp(boxes[1].type, "hidden") == 0);
    assert(strcmp(boxes[1].value, "keep") == 0);
    assert(strcmp(boxes[2].tag, "button") == 0);
    assert(strcmp(boxes[2].caption, "Search") == 0);
    assert(boxes[2].width > 0 && boxes[2].height > 0);
    assert(strcmp(boxes[2].action, "https://example.test/search") == 0);

    /* The GTK shell receives a compact representation of every native control
       it needs to instantiate. In particular, a select retains option IDs and
       selectedness rather than becoming an entry containing all option text. */
    const char *native_controls =
        "<!doctype html><style>html,body{margin:0}textarea,select{display:block;width:240px;height:28px}"
        "textarea{height:72px}input[type=checkbox],input[type=radio]{width:18px;height:18px}</style>"
        "<form><textarea data-distill-type=\"textarea\" name=\"message\""
        " placeholder=\"Write here\">First line\nSecond line</textarea>"
        "<select data-distill-type=\"select\" name=\"frequency\">"
        "<option value=\"never\">Never</option>"
        "<option value=\"weekly\" data-distill-selected=\"1\">Weekly</option></select>"
        "<input type=\"checkbox\" name=\"topics\" value=\"tech\" data-distill-checked=\"1\">"
        "<input type=\"radio\" name=\"mode\" value=\"focus\" data-distill-required=\"1\"></form>";
    DFormBox native[4] = {0};
    assert(d_page_measure_form_boxes(native_controls, 760, 800, NULL, native, 4) == 4);
    assert(strcmp(native[0].type, "textarea") == 0);
    assert(strcmp(native[0].value, "First line\nSecond line") == 0);
    assert(strcmp(native[0].placeholder, "Write here") == 0);
    assert(strcmp(native[1].type, "select") == 0);
    assert(strcmp(native[1].value, "weekly") == 0);
    assert(strstr(native[1].options, "weekly\x1f" "Weekly\x1f" "1") != NULL);
    assert(strcmp(native[2].type, "checkbox") == 0 && native[2].checked);
    assert(strcmp(native[3].type, "radio") == 0 && native[3].required);

    /* Choice labels are rendered by the page layout while their indicators are GTK
       overlays. Hitting either label must identify the input it controls. */
    const char *choice_page =
        "<!doctype html><style>html,body{margin:0}body{font-size:18px}"
        ".form-choice{display:block;margin:10px 0}.form-choice-input{width:18px;height:18px;"
        "vertical-align:middle;margin:0 8px 0 0}.form-choice span{vertical-align:middle}</style>"
        "<form><label class=\"form-choice\"><input id=\"choice-checkbox\""
        " class=\"form-choice-input\" type=\"checkbox\"><span>Technology</span></label>"
        "<label class=\"form-choice\"><input id=\"choice-radio\""
        " class=\"form-choice-input\" type=\"radio\"><span>Focused</span></label></form>";
    DPage *choice_document =
        d_page_create(choice_page, 760, 300, NULL);
    assert(choice_document);
    bool found_checkbox = false, found_radio = false;
    for (int y = 0; y < 180 && !(found_checkbox && found_radio); y += 2) {
        for (int x = 0; x < 300; x += 2) {
            char id[64] = {0};
            if (!d_page_choice_id_at(choice_document, x, y, id, sizeof(id)))
                continue;
            if (!strcmp(id, "choice-checkbox")) found_checkbox = true;
            if (!strcmp(id, "choice-radio")) found_radio = true;
        }
    }
    assert(found_checkbox && found_radio);
    d_page_destroy(choice_document);

    /* An input's explicit height must become part of the next field's layout;
       otherwise the GTK entry (which has a real allocation) covers the next
       label even though the painted template looks correct on its own. */
    const char *stacked_fields =
        "<!doctype html><style>html,body{margin:0}body{font-size:18px}"
        ".form-field-label{display:block;margin:.7em 0 0;text-align:left}"
        ".form-field-input{display:block;width:240px;height:28px;margin:3px 0 0}</style>"
        "<form><label class=\"form-field-label\">First</label>"
        "<input class=\"form-field-input\" name=\"first\">"
        "<label class=\"form-field-label\">Second</label>"
        "<input class=\"form-field-input\" name=\"second\"></form>";
    DFormBox stacked[2] = {0};
    assert(d_page_measure_form_boxes(stacked_fields, 760, 800, NULL, stacked, 2) == 2);
    assert(stacked[0].height == 28 && stacked[1].height == 28);
    assert(stacked[0].x == 0 && stacked[1].x == 0);
    assert(stacked[1].y >= stacked[0].y + stacked[0].height + 20);

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
    DPage *link_document =
        d_page_create(link_page, 760, 800, NULL);
    assert(link_document);

    /* Scan rather than hard-code pixel coordinates: the assertion is that the
       links are reachable, not that they sit at a particular offset under one
       font configuration. */
    bool found_heading = false, found_inline = false, found_gap = false;
    for (int y = 0; y < 400 && !(found_heading && found_inline); y += 3) {
        for (int x = 4; x < 700; x += 7) {
            char href[512] = {0};
            if (!d_page_link_at(link_document, x, y, href, sizeof(href))) continue;
            if (!strcmp(href, "https://example.test/story")) found_heading = true;
            if (!strcmp(href, "https://example.test/inline")) found_inline = true;
        }
    }
    assert(found_heading);
    assert(found_inline);

    /* Somewhere in the page must not be a link, or the test proves nothing. */
    for (int y = 0; y < 400 && !found_gap; y += 3) {
        char href[512] = {0};
        if (!d_page_link_at(link_document, 750, y, href, sizeof(href))) found_gap = true;
    }
    assert(found_gap);
    d_page_destroy(link_document);

    /* Text runs are what a selection is built from. The container is handed
       one per line with the box it filled, and nothing else in the boundary
       can answer where a character sits -- the shell has no layout of its own
       and no access to the fonts chosen by the layout. */
    const char *text_page =
        "<!doctype html><style>html,body{margin:0}body{font-size:18px;width:420px}"
        "p{margin:0 0 20px}li{margin:0}</style>"
        "<main><p>The harbour was quiet that morning and the tugs were still tied "
        "up against the wall, waiting for the tide to turn at noon.</p>"
        "<ul><li>First item</li><li>Second item</li></ul>"
        "<p>Sm\xc3\xb6rg\xc3\xa5sbord</p></main>";
    DPage *text_document =
        d_page_create(text_page, 420, 800, DISTILL_TEST_FONT_DIR);
    assert(text_document);
    size_t run_count = d_page_text_boxes(text_document, NULL, 0);
    /* The first paragraph alone wraps, so anything that collected one run per
       block -- or only what fits the painted band -- fails here. */
    assert(run_count >= 5);
    DTextBox *runs = calloc(run_count, sizeof(*runs));
    assert(runs);
    assert(d_page_text_boxes(text_document, runs, run_count) == run_count);

    bool saw_paragraph = false, saw_list_item = false, saw_swedish = false;
    unsigned int first_block = runs[0].block;
    unsigned int other_block = 0;
    for (size_t i = 0; i < run_count; ++i) {
        assert(runs[i].width > 0 && runs[i].height > 0);
        assert(runs[i].text[0]);
        if (!strcmp(runs[i].block_tag, "p")) saw_paragraph = true;
        if (!strcmp(runs[i].block_tag, "li")) saw_list_item = true;
        if (strstr(runs[i].text, "Sm\xc3\xb6rg\xc3\xa5sbord")) saw_swedish = true;
        if (runs[i].block != first_block) other_block = runs[i].block;
    }
    /* A block per paragraph and per list item: copying a wrapped line is one
       paragraph, copying two list items is two lines, and only the block
       identity distinguishes them -- the geometry is the same. */
    assert(saw_paragraph && saw_list_item && saw_swedish);
    assert(other_block != 0);

    /* Wrapped lines of one paragraph share a block; the run below them does
       not. Both directions matter: a selection joins the first pair and breaks
       at the second. */
    size_t wrapped = 0;
    while (wrapped + 1 < run_count && runs[wrapped + 1].block == runs[0].block) ++wrapped;
    assert(wrapped >= 1);
    assert(runs[wrapped].y > runs[0].y);

    /* Every character boundary must map to an x and back to itself: this is
       what stops a selection from landing inside a character. The Swedish run
       is here for that reason -- \xc3\xb6 is two bytes, and byte arithmetic
       would put a boundary between them. */
    for (size_t i = 0; i < run_count; ++i) {
        size_t length = strlen(runs[i].text);
        for (size_t offset = 0; offset <= length; ++offset) {
            if (offset < length && (runs[i].text[offset] & 0xC0) == 0x80) continue;
            int x = d_page_text_x_at(text_document, i, offset);
            size_t back = d_page_text_offset_at(text_document, i, x);
            assert(back == offset);
        }
        assert(d_page_text_offset_at(text_document, i, -5) == 0);
        assert(d_page_text_offset_at(text_document, i, 100000) == length);
    }
    free(runs);
    d_page_destroy(text_document);

    /* Collecting the text must leave the document as it found it. Both passes
       ask the layout what is under a point, and the collecting one asks it a
       thousand times in a row; a link that stops answering afterwards is a
       page whose links stop working the moment a reader selects a word. */
    const char *mixed_page =
        "<!doctype html><style>body{font-size:18px}</style>"
        "<main><p>Prose before <a href=\"https://example.test/inline\">a link inside "
        "a sentence</a> and prose after.</p></main>";
    DPage *mixed = d_page_create(mixed_page, 760, 800,
                                                               DISTILL_TEST_FONT_DIR);
    assert(mixed);
    int hits_before = 0, hits_after = 0;
    for (int y = 0; y < 200; y += 3)
        for (int x = 4; x < 700; x += 7) {
            char href[512] = {0};
            if (d_page_link_at(mixed, x, y, href, sizeof(href)))
                ++hits_before;
        }
    assert(hits_before > 0);
    assert(d_page_text_boxes(mixed, NULL, 0) > 0);
    for (int y = 0; y < 200; y += 3)
        for (int x = 4; x < 700; x += 7) {
            char href[512] = {0};
            if (d_page_link_at(mixed, x, y, href, sizeof(href)))
                ++hits_after;
        }
    assert(hits_after == hits_before);
    d_page_destroy(mixed);

    /* Section markers are intentionally empty, but their one-pixel layout box
       is what lets the desktop shell locate the section without a second
       layout implementation. */
    const char *marker_page =
        "<!doctype html><html><head><style>html,body{margin:0}"
        ".section-anchor{display:block;width:1px;height:1px;}</style></head><body>"
        "<main><p style=\"height:100px\">intro</p>"
        "<a class=\"section-anchor\" href=\"distill-section:images\"></a>"
        "<section id=\"images\"><h2>Images</h2></section></main></body></html>";
    DPage *marker_document =
        d_page_create(marker_page, 760, 800, NULL);
    assert(marker_document);
    size_t marker_count =
        d_page_link_boxes(marker_document, NULL, 0);
    assert(marker_count == 1);
    DLinkBox marker_box = {0};
    assert(d_page_link_boxes(marker_document, &marker_box, 1) == 1);
    assert(strcmp(marker_box.href, "distill-section:images") == 0);
    assert(marker_box.width == 1 && marker_box.height == 1);
    assert(marker_box.y > 0);
    d_page_destroy(marker_document);

    const char *long_page =
        "<!doctype html><style>body{font-size:18px}p{margin:0 0 120px}</style>"
        "<main><p>top</p><p>middle</p><p>bottom</p></main>";
    DPage *document = d_page_create(long_page, 760, 240, NULL);
    assert(document);
    assert(d_page_height(document) > 240);
    DRenderPixels pixels;
    assert(d_page_pixels(document, &pixels) && pixels.data && pixels.stride >= pixels.width * 4);
    assert(d_page_paint(document, 0));
    assert(d_page_paint(document, 180));
    d_page_destroy(document);
    assert(d_page_render_region_png(long_page, "/tmp/distill-region-test.png",
                                          760, 240, 180, NULL));
    assert(remove("/tmp/distill-region-test.png") == 0);

    test_image_is_drawn_where_it_is_laid_out();
    test_wide_image_is_fitted_to_the_surface();
    test_unreadable_image_takes_no_space();
    test_remote_sources_are_refused();
    test_bundled_font_roles_are_available();

    puts("reader-layout-geometry: ok");
    return 0;
}

static unsigned long crc_of(const unsigned char *data, size_t length)
{
    static unsigned long table[256];
    if (!table[1]) {
        for (unsigned long n = 0; n < 256; ++n) {
            unsigned long c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    }
    unsigned long c = 0xffffffffUL;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

static void put32(unsigned char *out, unsigned long value)
{
    out[0] = (unsigned char)(value >> 24); out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);  out[3] = (unsigned char)value;
}

static size_t chunk(unsigned char *out, const char *type,
                    const unsigned char *data, size_t length)
{
    put32(out, (unsigned long)length);
    memcpy(out + 4, type, 4);
    if (length) memcpy(out + 8, data, length);
    put32(out + 8 + length, crc_of(out + 4, length + 4));
    return length + 12;
}

/* A stored-deflate PNG: no compression, so no zlib dependency in the test. */
static bool write_solid_png(const char *path, int width, int height,
                            unsigned char red, unsigned char green, unsigned char blue)
{
    size_t row = (size_t)width * 3 + 1;
    size_t raw_length = row * (size_t)height;
    unsigned char *raw = malloc(raw_length);
    if (!raw) return false;
    for (int y = 0; y < height; ++y) {
        unsigned char *line = raw + (size_t)y * row;
        line[0] = 0;
        for (int x = 0; x < width; ++x) {
            line[1 + x * 3] = red; line[2 + x * 3] = green; line[3 + x * 3] = blue;
        }
    }
    size_t blocks = raw_length / 65535 + 1;
    size_t stream_length = 2 + blocks * 5 + raw_length + 4;
    unsigned char *stream = malloc(stream_length);
    if (!stream) { free(raw); return false; }
    size_t at = 0;
    stream[at++] = 0x78; stream[at++] = 0x01;
    size_t left = raw_length, offset = 0;
    do {
        size_t take = left > 65535 ? 65535 : left;
        stream[at++] = (left - take) ? 0 : 1;
        stream[at++] = (unsigned char)(take & 0xff);
        stream[at++] = (unsigned char)(take >> 8);
        stream[at++] = (unsigned char)(~take & 0xff);
        stream[at++] = (unsigned char)((~take >> 8) & 0xff);
        memcpy(stream + at, raw + offset, take);
        at += take; offset += take; left -= take;
    } while (left);
    unsigned long s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_length; ++i) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    put32(stream + at, (s2 << 16) | s1);
    at += 4;

    unsigned char header[13];
    put32(header, (unsigned long)width);
    put32(header + 4, (unsigned long)height);
    header[8] = 8; header[9] = 2; header[10] = 0; header[11] = 0; header[12] = 0;

    unsigned char *out = malloc(at + raw_length + 256);
    if (!out) { free(raw); free(stream); return false; }
    size_t total = 0;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8); total = 8;
    total += chunk(out + total, "IHDR", header, sizeof(header));
    total += chunk(out + total, "IDAT", stream, at);
    total += chunk(out + total, "IEND", NULL, 0);
    free(raw); free(stream);

    FILE *file = fopen(path, "wb");
    bool ok = file && fwrite(out, 1, total, file) == total;
    if (file && fclose(file) != 0) ok = false;
    free(out);
    return ok;
}
