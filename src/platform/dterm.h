#ifndef DISTILL_PLATFORM_DTERM_H
#define DISTILL_PLATFORM_DTERM_H

/* The controlling terminal, for the terminal shell. termbox2 draws; this is
   what the shell needs around it: opening the terminal, knowing its size in
   pixels, talking to it directly for capability queries, and putting it back
   the way it was found -- also from a signal handler, when the shell dies. */

#include <stddef.h>

/* Open the controlling terminal and remember its mode. The handle is the
   one termbox2 is initialised on. -1 with `*error` set (caller frees). */
int d_term_open(char **error);

/* Put the terminal's mode back and close it. */
void d_term_close(int handle);

/* The terminal's size, when it reports one. Pixel sizes are 0 when the
   terminal does not say. */
typedef struct {
    int columns, rows;
    int pixel_width, pixel_height;
} DTermSize;
bool d_term_size(int handle, DTermSize *size);

/* Write all of `data`. False if the terminal refused it. */
bool d_term_write(int handle, const void *data, size_t length);

/* Read what the terminal sends within `timeout_ms`: the byte count, 0 when
   nothing came. */
size_t d_term_read(int handle, void *buffer, size_t size, int timeout_ms);

/* For a signal handler: write `reset` (escape sequences undoing what the
   shell turned on) and put the saved mode back. Uses only calls that are safe
   in a handler. Does nothing if no terminal is open. */
void d_term_emergency_restore(const char *reset, size_t length);

#endif
