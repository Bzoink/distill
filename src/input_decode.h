#ifndef DISTILL_INPUT_DECODE_H
#define DISTILL_INPUT_DECODE_H

/* The keyboard and the pointer as one byte stream, decoded.

   A Linux virtual console delivers keys on its tty as ANSI sequences, with
   the console keymap already applied, so a reader's layout, dead keys and
   compose arrive as text. Muroxi delivers its keyboard the same way and its
   pointer in the same stream as SGR (1006) mouse reports, in character
   cells. This turns either into DEvents, once, in portable code, so both
   consoles and the memory sink's scripts share it (muroxi_compat.md, "The
   input decoder is shared").

   Bytes are fed as they are read and events taken out one at a time. A
   read is a batch: a lone Escape at the end of one is the Escape key, and an
   Escape with a key behind it in the same batch is Alt and that key -- which
   is how a terminal sends them, and how the terminal shell reads them. */

#include "ddisplay.h"

#include <stdbool.h>
#include <stddef.h>

enum { D_INPUT_PENDING_MAX = 256 };

typedef struct {
    unsigned char pending[D_INPUT_PENDING_MAX];
    size_t length;
    int cell_width, cell_height;   /* pixels per cell, for mouse reports */
    int pointer_x, pointer_y;      /* where the last report put the pointer */
} DInputDecoder;

void d_input_init(DInputDecoder *decoder, int cell_width, int cell_height);

/* Append bytes. Returns how many were taken: fewer than `length` only when
   the pending buffer is full, and taking events out makes room. */
size_t d_input_feed(DInputDecoder *decoder, const void *bytes, size_t length);

/* The next complete event, or false. `batch_end` says no more bytes of this
   read are coming, so an incomplete sequence is resolved rather than kept:
   a lone Escape becomes the key, and a sequence cut short is dropped. */
bool d_input_next(DInputDecoder *decoder, DEvent *event, bool batch_end);

#endif
