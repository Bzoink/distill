/* distill-paint*: lay a reader page out and paint it, and write the pixels.

       distill-paint[-cairo|-defect] PAGE.html WIDTH OUT.ppm [FONT_DIR]

   One source, three tools, differing only in the painter linked in
   (paint.h): the software painter every target ships, the Cairo reference it
   replaced, and a software painter with a deliberate defect. `check.py
   --paint` runs the first two over the render corpus and compares, and runs
   the third to show the comparison can fail.

   PPM, not PNG, because the comparison is of pixels, not of encoders. The
   page is opaque, so a pixel's colour is its colour whatever its alpha. */

#include "cutil.h"
#include "page.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s PAGE.html WIDTH OUT.ppm [FONT_DIR]\n", argv[0]);
        return 2;
    }
    size_t length = 0;
    char *html = d_read_file(argv[1], &length);
    int width = (int)strtol(argv[2], NULL, 10);
    const char *font_dir = argc > 4 ? argv[4] : NULL;
    if (!html || width <= 0) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    int height = d_page_measure_height(html, width, font_dir);
    if (height < 1) height = 1;
    if (height > DISTILL_MAX_RENDER_HEIGHT) height = DISTILL_MAX_RENDER_HEIGHT;
    DPage *document = d_page_create(html, width, height, font_dir);
    DRenderPixels pixels;
    if (!document || !d_page_paint(document, 0) ||
        !d_page_pixels(document, &pixels)) {
        fprintf(stderr, "unable to paint %s\n", argv[1]);
        return 3;
    }
    FILE *out = fopen(argv[3], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[3]); return 2; }
    fprintf(out, "P6\n%d %d\n255\n", pixels.width, pixels.height);
    unsigned char *row = d_xmalloc((size_t)pixels.width * 3);
    for (int y = 0; y < pixels.height; ++y) {
        const uint32_t *source = (const uint32_t *)((const unsigned char *)pixels.data +
                                                    (size_t)y * (size_t)pixels.stride);
        for (int x = 0; x < pixels.width; ++x) {
            row[x * 3] = (unsigned char)(source[x] >> 16);
            row[x * 3 + 1] = (unsigned char)(source[x] >> 8);
            row[x * 3 + 2] = (unsigned char)source[x];
        }
        fwrite(row, 1, (size_t)pixels.width * 3, out);
    }
    free(row);
    fclose(out);
    d_page_destroy(document);
    free(html);
    return 0;
}
