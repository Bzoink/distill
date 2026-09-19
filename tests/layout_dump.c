/* distill-layout-*: lay a reader page out and write down what the layout decided.

       distill-layout-<layout> PAGE.html WIDTH OUT.txt [FONT_DIR]

   One source, built against the reader layout and its painter. The records
   are text so a failing comparison can be read, and diffed, by a person.

       height H
       run X Y W H <block tag> TAB <text>
       form X Y W H <tag> <type> <name>
       link X Y W H <href>
*/

#include "cutil.h"
#include "page.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s PAGE.html WIDTH OUT.txt [FONT_DIR]\n", argv[0]);
        return 2;
    }
    size_t length = 0;
    char *html = d_read_file(argv[1], &length);
    int width = (int)strtol(argv[2], NULL, 10);
    const char *font_dir = argc > 4 ? argv[4] : NULL;
    if (!html || width <= 0) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    DPage *page = d_page_create(html, width, 1, font_dir);
    if (!page) { fprintf(stderr, "unable to lay out %s\n", argv[1]); return 3; }
    FILE *out = fopen(argv[3], "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[3]); return 2; }
    fprintf(out, "height %d\n", d_page_height(page));

    size_t count = d_page_text_boxes(page, NULL, 0);
    DTextBox *runs = d_xcalloc(count ? count : 1, sizeof(*runs));
    d_page_text_boxes(page, runs, count);
    for (size_t i = 0; i < count; ++i) {
        for (char *p = runs[i].text; *p; ++p)
            if (*p == '\n' || *p == '\t') *p = ' ';
        fprintf(out, "run %d %d %d %d %s\t%s\n", runs[i].x, runs[i].y, runs[i].width,
                runs[i].height, runs[i].block_tag[0] ? runs[i].block_tag : "-", runs[i].text);
    }
    free(runs);

    count = d_page_form_boxes(page, NULL, 0);
    DFormBox *forms = d_xcalloc(count ? count : 1, sizeof(*forms));
    d_page_form_boxes(page, forms, count);
    for (size_t i = 0; i < count; ++i)
        fprintf(out, "form %d %d %d %d %s %s %s\n", forms[i].x, forms[i].y, forms[i].width,
                forms[i].height, forms[i].tag, forms[i].type[0] ? forms[i].type : "-",
                forms[i].name[0] ? forms[i].name : "-");
    free(forms);

    count = d_page_link_boxes(page, NULL, 0);
    DLinkBox *links = d_xcalloc(count ? count : 1, sizeof(*links));
    d_page_link_boxes(page, links, count);
    for (size_t i = 0; i < count; ++i)
        fprintf(out, "link %d %d %d %d %s\n", links[i].x, links[i].y, links[i].width,
                links[i].height, links[i].href);
    free(links);

    fclose(out);
    d_page_destroy(page);
    free(html);
    return 0;
}
