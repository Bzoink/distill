#ifndef DISTILL_TUI_SCREEN_H
#define DISTILL_TUI_SCREEN_H

/* What the terminal shows, and the bytes that make it so.

   termbox2 owns the terminal -- raw mode, input, resize, restoring it on the
   way out -- but not the output, and that is a measured decision. Terminals
   disagree about how wide a grapheme cluster is: xterm draws the Devanagari
   conjunct क्ष in two cells, kitty in one. termbox advances by its own
   answer and only repositions the cursor after cells it considers wide, so a
   cluster it thinks is narrow and the terminal thinks is not shifts the rest
   of the row. This writer repositions explicitly after every cluster that is
   not a single one-cell code point, which turns any such disagreement into
   at most a one-cell gap instead of a displaced line.

   It keeps what it believes is on screen and sends only what changed, which
   is the other thing a library would have done for us. The bytes go into a
   buffer, so what a frame sends is testable without a terminal. */

#include "cutil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A colour is the terminal's default, one of the sixteen it names, or RGB. */
typedef uint32_t DColor;
#define D_COLOR_DEFAULT 0xffffffffu
#define D_COLOR_ANSI(n) (0x01000000u | (uint32_t)(n))
#define D_COLOR_RGB(rgb) ((uint32_t)(rgb) & 0x00ffffffu)

enum {
    D_SCREEN_BOLD      = 1u << 0,
    D_SCREEN_DIM       = 1u << 1,
    D_SCREEN_ITALIC    = 1u << 2,
    D_SCREEN_UNDERLINE = 1u << 3,
    D_SCREEN_REVERSE   = 1u << 4,
};

typedef enum {
    D_SCREEN_COLORS_16,
    D_SCREEN_COLORS_256,
    D_SCREEN_COLORS_TRUE,
} DScreenColors;

typedef struct {
    char text[40];      /* one cluster, NUL-terminated; "" is a space */
    uint8_t width;      /* cells the cluster covers; 0 marks a continuation */
    uint8_t attrs;
    DColor fg, bg;
} DScreenCell;

typedef struct {
    int width, height;
    DScreenCell *back;    /* the frame being built */
    DScreenCell *front;   /* what was last sent */
    bool front_valid;
    DScreenColors colors;
} DScreen;

void d_screen_init(DScreen *screen, int width, int height, DScreenColors colors);
void d_screen_resize(DScreen *screen, int width, int height);
void d_screen_free(DScreen *screen);
/* Forget what is on screen; the next flush redraws everything. */
void d_screen_invalidate(DScreen *screen);

void d_screen_clear(DScreen *screen, DColor fg, DColor bg);
/* One cluster at (x, y). Returns the column after it; a cluster that would
   cross the right edge is not drawn. */
int d_screen_put(DScreen *screen, int x, int y, const char *bytes, size_t length, int width,
                 DColor fg, DColor bg, uint8_t attrs);
/* Text from x up to (not including) column `limit`. Returns the next column. */
int d_screen_text(DScreen *screen, int x, int y, int limit, const char *text,
                  DColor fg, DColor bg, uint8_t attrs);
/* Fill [x, limit) of a row with blanks. */
void d_screen_fill(DScreen *screen, int x, int y, int limit, DColor fg, DColor bg, uint8_t attrs);
DScreenCell *d_screen_cell(DScreen *screen, int x, int y);

/* Append the bytes that bring the terminal from the front frame to the back
   one, and make the back frame the front. */
void d_screen_flush(DScreen *screen, DBuffer *out);

#endif
