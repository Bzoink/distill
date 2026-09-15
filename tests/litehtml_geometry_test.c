#include "image.h"
#include "render.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write a solid-colour PNG, so a render of it can be checked pixel by pixel.
   Hand-built rather than encoded with a library, to keep this test's only
   dependency the one it is testing. */
static bool write_solid_png(const char *path, int width, int height,
                            unsigned char red, unsigned char green, unsigned char blue);

/* The image path is worth a pixel-level assertion rather than a "did it draw
   something" one. Its first bug was an offset: litehtml reports a resolved
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
    assert(distill_litehtml_render_region(html, output, 400, 200, 0, NULL));

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
   reserves its box and is never painted in this litehtml build. */
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
    int height = distill_litehtml_content_height(html, 300, NULL);
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
    int height = distill_litehtml_content_height(html, 400, NULL);
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
    int height = distill_litehtml_content_height(html, 400, NULL);
    assert(height >= 0 && height < 40);
}

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

    test_image_is_drawn_where_it_is_laid_out();
    test_wide_image_is_fitted_to_the_surface();
    test_unreadable_image_takes_no_space();
    test_remote_sources_are_refused();

    puts("litehtml-geometry: ok");
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
