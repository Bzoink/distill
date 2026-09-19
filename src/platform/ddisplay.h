#ifndef DISTILL_PLATFORM_DDISPLAY_H
#define DISTILL_PLATFORM_DDISPLAY_H

/* A place to put pixels, and the input that comes with it, for the
   framebuffer shell (FRAMEBUFFER_PLAN.md, "Three layers").

   The shell paints into a back buffer of its own and hands it here; a sink
   copies what changed onto its device, row by row, honouring both strides --
   never assuming either is width * 4. A sink and its input source come as a
   pair, because the window that shows the pixels is also where the keys and
   the pointer arrive, and so is the console.

   Unlike the other platform interfaces, the sink is chosen at run time: the
   gates use the memory sink and a real run uses the screen. The spec names
   it:

       memory:WxH[,cell=CWxCH]   an in-memory device, driven by a script
       console                   the console: /dev/fb0, the tty, evdev on Linux
       x11:WxH                   a window, in the development build

   Like termbox2 for the terminal shell, the sink hands back handles to wait
   on instead of owning the loop: the shell also waits on the engine, on
   transfers and on the audio core. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render.h"

typedef struct DDisplay DDisplay;

typedef struct {
    int width, height;          /* pixels */
    int stride;                 /* bytes per row of the device; never assumed width * 4 */
    int cell_width, cell_height;/* a character cell, where input arrives in cells */
} DDisplayMode;

typedef enum {
    D_EVENT_NONE,
    D_EVENT_KEY,                /* key + modifiers, or a character */
    D_EVENT_POINTER_MOVE,       /* absolute, in display pixels */
    D_EVENT_POINTER_BUTTON,
    D_EVENT_SCROLL,             /* the wheel, in lines: positive is down */
    D_EVENT_RESIZE,             /* the device changed size; read the mode again */
    D_EVENT_SUSPEND,            /* VT switched away, presentation taken: stop drawing */
    D_EVENT_RESUME,             /* back again: redraw everything */
    D_EVENT_QUIT,               /* the window was closed, the script ended */
} DEventKind;

/* Keys that are not characters. A character key is D_KEY_CHAR with its code
   point; Ctrl+L is D_KEY_CHAR 'l' with D_MOD_CTRL. */
typedef enum {
    D_KEY_NONE,
    D_KEY_CHAR,
    D_KEY_ENTER,
    D_KEY_TAB,
    D_KEY_ESCAPE,
    D_KEY_BACKSPACE,
    D_KEY_DELETE,
    D_KEY_INSERT,
    D_KEY_UP,
    D_KEY_DOWN,
    D_KEY_LEFT,
    D_KEY_RIGHT,
    D_KEY_HOME,
    D_KEY_END,
    D_KEY_PAGE_UP,
    D_KEY_PAGE_DOWN,
    D_KEY_F1,                   /* D_KEY_F1 + n - 1 is Fn, up to F12 */
    D_KEY_F12 = D_KEY_F1 + 11,
} DKey;

enum {
    D_MOD_SHIFT = 1u << 0,
    D_MOD_ALT   = 1u << 1,
    D_MOD_CTRL  = 1u << 2,
};

enum {
    D_BUTTON_LEFT = 1,
    D_BUTTON_MIDDLE = 2,
    D_BUTTON_RIGHT = 3,
};

typedef struct {
    DEventKind kind;
    DKey key;
    unsigned mods;
    uint32_t codepoint;         /* D_KEY_CHAR */
    int x, y;                   /* pointer events, in pixels */
    int button;                 /* D_EVENT_POINTER_BUTTON */
    bool pressed;
    int lines;                  /* D_EVENT_SCROLL */
} DEvent;

/* NULL with `*error` set (caller frees) when the sink cannot be opened --
   and for the console, when this is not a console to open. */
DDisplay *d_display_open(const char *spec, char **error);

DDisplayMode d_display_mode(const DDisplay *display);

/* Handles to wait on (d_poll) before asking for events: up to `capacity` of
   them, and how many there were. */
size_t d_display_handles(DDisplay *display, int *handles, size_t capacity);

/* How long the shell may wait before asking for events again: -1 for as
   long as the handles are quiet, 0 when events are ready now (a script with
   lines left), else milliseconds. */
int d_display_wait_ms(DDisplay *display);

/* The next event, or false when there is none yet. Never blocks. */
bool d_display_next_event(DDisplay *display, DEvent *event);

/* Copy the damaged rectangles of the back buffer onto the device. `pixels`
   is 0xAARRGGBB, opaque, `stride` bytes per row, at the display's size. The
   sink may keep the pointer until the next present, to put back what a drawn
   pointer covers. Nothing is shown while suspended. */
void d_display_present(DDisplay *display, const uint32_t *pixels, int stride,
                       const DRenderRect *damage, size_t damage_count);

/* What the shell is doing, for a sink that can use it: whether it is busy
   (loading, fetching) and one JSON line of its state. The memory sink's
   script waits on the first and writes out the second; a window shows the
   title. `title` may be NULL. */
void d_display_report(DDisplay *display, bool busy, const char *state, const char *title);

/* Put the device back and close it. */
void d_display_close(DDisplay *display);

/* For a signal handler: undo what the open sink did to the console --
   graphics mode, VT handling, the tty's mode -- with only calls that are
   safe there. Does nothing when nothing needs undoing. */
void d_display_emergency_restore(void);

#endif
