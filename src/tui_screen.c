#include "tui_screen.h"

#include "reader_cells.h"

static DScreenCell blank(DColor fg, DColor bg, uint8_t attrs)
{
    return (DScreenCell){.text = "", .width = 1, .attrs = attrs, .fg = fg, .bg = bg};
}

void d_screen_init(DScreen *screen, int width, int height, DScreenColors colors)
{
    *screen = (DScreen){.colors = colors};
    d_screen_resize(screen, width, height);
}

void d_screen_resize(DScreen *screen, int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    free(screen->back);
    free(screen->front);
    size_t count = (size_t)width * (size_t)height;
    screen->back = d_xcalloc(count, sizeof(*screen->back));
    screen->front = d_xcalloc(count, sizeof(*screen->front));
    screen->width = width;
    screen->height = height;
    screen->front_valid = false;
    d_screen_clear(screen, D_COLOR_DEFAULT, D_COLOR_DEFAULT);
}

void d_screen_free(DScreen *screen)
{
    free(screen->back);
    free(screen->front);
    *screen = (DScreen){0};
}

void d_screen_invalidate(DScreen *screen)
{
    screen->front_valid = false;
}

DScreenCell *d_screen_cell(DScreen *screen, int x, int y)
{
    return &screen->back[(size_t)y * (size_t)screen->width + (size_t)x];
}

void d_screen_clear(DScreen *screen, DColor fg, DColor bg)
{
    size_t count = (size_t)screen->width * (size_t)screen->height;
    for (size_t i = 0; i < count; ++i) screen->back[i] = blank(fg, bg, 0);
}

void d_screen_fill(DScreen *screen, int x, int y, int limit, DColor fg, DColor bg, uint8_t attrs)
{
    if (y < 0 || y >= screen->height) return;
    if (limit > screen->width) limit = screen->width;
    for (int i = x < 0 ? 0 : x; i < limit; ++i) *d_screen_cell(screen, i, y) = blank(fg, bg, attrs);
}

int d_screen_put(DScreen *screen, int x, int y, const char *bytes, size_t length, int width,
                 DColor fg, DColor bg, uint8_t attrs)
{
    if (y < 0 || y >= screen->height || x < 0 || width <= 0 || x + width > screen->width) return x;
    DScreenCell *cell = d_screen_cell(screen, x, y);
    *cell = blank(fg, bg, attrs);
    if (length >= sizeof(cell->text)) length = sizeof(cell->text) - 1;
    memcpy(cell->text, bytes, length);
    cell->text[length] = '\0';
    cell->width = (uint8_t)width;
    for (int i = 1; i < width; ++i) {
        DScreenCell *rest = d_screen_cell(screen, x + i, y);
        *rest = blank(fg, bg, attrs);
        rest->width = 0;
    }
    return x + width;
}

typedef struct {
    DScreen *screen;
    int x, y, limit;
    DColor fg, bg;
    uint8_t attrs;
    bool stopped;
} TextWrite;

static void write_cluster(void *opaque, const char *bytes, size_t length, int width)
{
    TextWrite *write = opaque;
    if (write->stopped || !width) return;
    if (write->x + width > write->limit) { write->stopped = true; return; }
    write->x = d_screen_put(write->screen, write->x, write->y, bytes, length, width,
                            write->fg, write->bg, write->attrs);
}

int d_screen_text(DScreen *screen, int x, int y, int limit, const char *text,
                  DColor fg, DColor bg, uint8_t attrs)
{
    if (!text) return x;
    if (limit > screen->width) limit = screen->width;
    TextWrite write = {screen, x, y, limit, fg, bg, attrs, false};
    reader_text_clusters(text, strlen(text), write_cluster, &write);
    return write.x;
}

static bool same(const DScreenCell *a, const DScreenCell *b)
{
    return a->width == b->width && a->attrs == b->attrs && a->fg == b->fg && a->bg == b->bg &&
           !strcmp(a->text, b->text);
}

/* The nearest of the 256-colour palette's cube to an RGB colour, for a
   terminal that has the palette but not direct colour. */
static int to_256(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    return 16 + ri * 36 + gi * 6 + bi;
}

static void append_color(DBuffer *out, DColor color, bool foreground, DScreenColors colors)
{
    if (color == D_COLOR_DEFAULT) {
        dbuf_puts(out, foreground ? ";39" : ";49");
    } else if (color & 0x01000000u) {
        int n = (int)(color & 0xff);
        int base = n < 8 ? (foreground ? 30 : 40) : (foreground ? 90 : 100);
        dbuf_printf(out, ";%d", base + (n & 7));
    } else if (colors == D_SCREEN_COLORS_TRUE) {
        dbuf_printf(out, ";%d;2;%u;%u;%u", foreground ? 38 : 48,
                    (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
    } else {
        dbuf_printf(out, ";%d;5;%d", foreground ? 38 : 48, to_256(color));
    }
}

static void append_style(DBuffer *out, const DScreenCell *cell, DScreenColors colors)
{
    dbuf_puts(out, "\033[0");
    if (cell->attrs & D_SCREEN_BOLD) dbuf_puts(out, ";1");
    if (cell->attrs & D_SCREEN_DIM) dbuf_puts(out, ";2");
    if (cell->attrs & D_SCREEN_ITALIC) dbuf_puts(out, ";3");
    if (cell->attrs & D_SCREEN_UNDERLINE) dbuf_puts(out, ";4");
    if (cell->attrs & D_SCREEN_REVERSE) dbuf_puts(out, ";7");
    append_color(out, cell->fg, true, colors);
    append_color(out, cell->bg, false, colors);
    dbuf_puts(out, "m");
}

/* One code point, one cell: the only kind of cluster every terminal agrees
   about, so the only kind after which the cursor's position is trusted. */
static bool trusted(const DScreenCell *cell)
{
    if (cell->width != 1) return false;
    const unsigned char *t = (const unsigned char *)cell->text;
    if (!*t) return true;
    size_t n = *t < 0x80 ? 1 : (*t >> 5) == 0x6 ? 2 : (*t >> 4) == 0xe ? 3 : 4;
    return strlen(cell->text) == n;
}

void d_screen_flush(DScreen *screen, DBuffer *out)
{
    int cursor_x = -1, cursor_y = -1;
    const DScreenCell *style = NULL;
    for (int y = 0; y < screen->height; ++y) {
        for (int x = 0; x < screen->width; ++x) {
            DScreenCell *cell = d_screen_cell(screen, x, y);
            DScreenCell *shown = &screen->front[(size_t)y * (size_t)screen->width + (size_t)x];
            if (!cell->width) { *shown = *cell; continue; }
            if (screen->front_valid && same(cell, shown)) continue;
            if (cursor_x != x || cursor_y != y) dbuf_printf(out, "\033[%d;%dH", y + 1, x + 1);
            if (!style || style->attrs != cell->attrs || style->fg != cell->fg || style->bg != cell->bg)
                append_style(out, cell, screen->colors);
            style = cell;
            dbuf_puts(out, *cell->text ? cell->text : " ");
            *shown = *cell;
            /* A wide cluster's continuations belong to it; they are sent
               with it and must not be compared on their own afterwards. */
            for (int i = 1; i < cell->width && x + i < screen->width; ++i)
                screen->front[(size_t)y * (size_t)screen->width + (size_t)(x + i)] =
                    *d_screen_cell(screen, x + i, y);
            if (trusted(cell)) { cursor_x = x + 1; cursor_y = y; }
            else cursor_x = cursor_y = -1;
        }
    }
    if (style) dbuf_puts(out, "\033[0m");
    screen->front_valid = true;
}
