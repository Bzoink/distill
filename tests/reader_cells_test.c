/* The cell renderer as a pure function: pages in, grids and geometry out.
   The dump is gated through the corpus; this checks what a dump cannot show
   -- where links, controls, anchors and tables are, and that the state a
   shell changes afterwards lands in the right cells. */

#include "reader_cells.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ReaderLayout *layout_of(const char *html, int columns)
{
    ReaderLayout *layout = reader_layout(html, &(ReaderLayoutOptions){.columns = columns});
    assert(layout);
    return layout;
}

static char *row_text(const ReaderLayout *layout, int row)
{
    return reader_layout_dump(layout, row, 1);
}

static bool row_has(const ReaderLayout *layout, int row, const char *needle)
{
    char *text = row_text(layout, row);
    bool found = strstr(text, needle) != NULL;
    free(text);
    return found;
}

static int find_row(const ReaderLayout *layout, const char *needle)
{
    for (int r = 0; r < layout->rows; ++r)
        if (row_has(layout, r, needle)) return r;
    return -1;
}

static void test_measure_and_wrapping(void)
{
    const char *page = "<html><body><p>one two three four five six seven eight nine ten "
                       "eleven twelve thirteen fourteen</p></body></html>";
    ReaderLayout *layout = layout_of(page, 20);
    assert(layout->measure == 18);
    assert(layout->left == 1);
    assert(layout->rows > 3);
    for (int r = 0; r < layout->rows; ++r) {
        char *text = row_text(layout, r);
        assert(reader_text_cells(text, strlen(text) - 1) <= 19);
        free(text);
    }
    reader_layout_free(layout);

    /* A wide terminal gets a centred column, not prose the full width. */
    layout = layout_of(page, 200);
    assert(layout->measure == 80);
    assert(layout->left == 60);
    reader_layout_free(layout);
}

static void test_clusters(void)
{
    assert(reader_text_cells("abc", 3) == 3);
    assert(reader_text_cells("\xe4\xb8\xad\xe6\x96\x87", 6) == 4);                /* 中文 */
    assert(reader_text_cells("e\xcc\x81", 3) == 1);                              /* e + acute */
    /* family: man ZWJ woman ZWJ girl -- one cluster, two cells */
    const char *family = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
    assert(reader_text_cells(family, strlen(family)) == 2);
    assert(reader_text_cells("\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa", 8) == 2);      /* flag */
    assert(reader_text_cells("\xe2\x9d\xa4\xef\xb8\x8f", 6) == 2);              /* heart + VS16 */

    /* A wide character never straddles the edge: the one that would is
       moved to the next line whole. */
    ReaderLayout *layout = layout_of("<html><body><p>"
        "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"
        "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"
        "</p></body></html>", 11);
    for (int r = 0; r < layout->rows; ++r)
        for (int c = 0; c < layout->columns; ++c) {
            ReaderCell *cell = reader_layout_cell(layout, r, c);
            if (cell->width == 2) assert(c + 1 < layout->columns &&
                                         reader_layout_cell(layout, r, c + 1)->width == 0);
        }
    reader_layout_free(layout);
}

static void test_links_and_anchors(void)
{
    const char *page =
        "<html><body><main><h1>Title</h1><p>Read <a href=\"https://example.com/a\">the "
        "article</a> now.</p><a class=\"section-anchor\" href=\"distill-section:more\"></a>"
        "<section id=\"more\" class=\"audio-card\"><h2>More</h2>"
        "<a class=\"button\" href=\"distill-play:https://example.com/a.mp3\">Play</a>"
        "</section></main></body></html>";
    ReaderLayout *layout = layout_of(page, 60);
    assert(layout->link_count == 2);
    ReaderLayoutLink *article = &layout->links[0];
    assert(!strcmp(article->href, "https://example.com/a"));
    assert(!article->button);
    ReaderCell *first = reader_layout_cell(layout, article->row, article->column);
    size_t length = 0;
    const char *text = reader_cell_text(layout, first, &length);
    assert(length == 1 && text[0] == 't');
    assert(first->link == 0 && first->role == READER_ROLE_LINK);
    assert(first->attrs & READER_CELL_UNDERLINE);

    ReaderLayoutLink *play = &layout->links[1];
    assert(play->button);
    assert(row_has(layout, play->row, "[ Play ]"));
    assert(reader_layout_cell(layout, play->row, play->column)->role == READER_ROLE_BUTTON);

    assert(layout->anchor_count == 1);
    assert(!strcmp(layout->anchors[0].id, "more"));
    assert(layout->anchors[0].row == find_row(layout, "More"));
    reader_layout_free(layout);
}

static void test_controls(void)
{
    const char *page =
        "<html><body><form method=\"post\" action=\"https://example.com/f\">"
        "<label>Name</label><input type=\"text\" name=\"n\" value=\"Ada\">"
        "<input type=\"hidden\" name=\"t\" value=\"tok\">"
        "<label class=\"form-choice\"><input type=\"checkbox\" name=\"c\" data-distill-checked=\"1\">"
        "<span>Remember</span></label>"
        "<select name=\"s\"><option value=\"a\">Alpha</option>"
        "<option value=\"b\" data-distill-selected=\"1\">Beta</option></select>"
        "<button type=\"submit\" data-distill-type=\"submit\" name=\"go\" value=\"1\">Send</button>"
        "</form></body></html>";
    ReaderLayout *layout = layout_of(page, 60);
    assert(layout->control_count == 5);
    ReaderLayoutControl *name = &layout->controls[0];
    assert(!strcmp(name->name, "n") && !strcmp(name->value, "Ada"));
    assert(!strcmp(name->method, "post") && !strcmp(name->action, "https://example.com/f"));
    assert(name->in_form && name->form_index == 0);
    assert(name->row >= 0 && name->width == 40);
    assert(row_has(layout, name->row, "Ada"));

    ReaderLayoutControl *hidden = &layout->controls[1];
    assert(hidden->width == 0 && hidden->row < 0);
    assert(!strcmp(hidden->value, "tok"));

    ReaderLayoutControl *box = &layout->controls[2];
    assert(box->checked && box->width == 3);
    assert(row_has(layout, box->row, "[x] Remember"));

    ReaderLayoutControl *select = &layout->controls[3];
    assert(!strcmp(select->value, "b"));
    assert(row_has(layout, select->row, "Beta"));

    ReaderLayoutControl *send = &layout->controls[4];
    assert(!strcmp(send->type, "submit") && !strcmp(send->caption, "Send"));
    assert(row_has(layout, send->row, "[ Send ]"));

    /* What the user changes is painted in place. */
    reader_layout_paint_control(layout, 0, "Grace", false);
    assert(row_has(layout, name->row, "Grace") && !row_has(layout, name->row, "Ada"));
    reader_layout_paint_control(layout, 2, NULL, false);
    assert(row_has(layout, box->row, "[ ] Remember"));
    reader_layout_paint_control(layout, 3, "a", false);
    assert(row_has(layout, select->row, "Alpha"));
    reader_layout_free(layout);
}

static void test_table_window(void)
{
    const char *page =
        "<html><body><table><tr><th>Name</th><th>Description</th></tr>"
        "<tr><td>alpha</td><td>a long description that runs well past the edge</td></tr>"
        "</table></body></html>";
    ReaderLayout *layout = layout_of(page, 30);
    assert(layout->table_count == 1);
    ReaderLayoutTable *table = &layout->tables[0];
    assert(table->full_width > table->visible);
    int row = table->row;
    assert(row_has(layout, row, "Name"));
    assert(row_has(layout, row, "\xe2\x80\xba"));                 /* › more to the right */
    reader_layout_scroll_table(layout, 0, table->full_width);
    assert(table->offset == table->full_width - table->visible);
    assert(!row_has(layout, row, "Name"));
    assert(row_has(layout, row, "\xe2\x80\xb9"));                 /* ‹ more to the left */
    reader_layout_scroll_table(layout, 0, 0);
    assert(row_has(layout, row, "Name"));
    reader_layout_free(layout);
}

static void test_pre_and_rtl(void)
{
    ReaderLayout *layout = layout_of("<html><body><pre>a\tb\n\tc</pre></body></html>", 40);
    assert(row_has(layout, 0, "a       b"));
    assert(row_has(layout, 1, "        c"));
    reader_layout_free(layout);

    layout = layout_of("<html dir=\"rtl\"><body><p>short</p></body></html>", 40);
    char *text = row_text(layout, 0);
    /* Right-aligned inside the measure: the text ends at its right edge. */
    assert(strlen(text) - 1 == (size_t)(layout->left + layout->measure));
    free(text);
    assert(!strcmp(layout->dir, "rtl"));
    reader_layout_free(layout);
}

int main(void)
{
    test_measure_and_wrapping();
    test_clusters();
    test_links_and_anchors();
    test_controls();
    test_table_window();
    test_pre_and_rtl();
    puts("reader cells: ok");
    return 0;
}
