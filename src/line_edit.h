#ifndef DISTILL_LINE_EDIT_H
#define DISTILL_LINE_EDIT_H

/* Editing a line of UTF-8 text: the address line, a form field, a save-as
   path. Every front end without a toolkit needs one, and they must behave
   alike -- the same keys delete the same amount -- so it lives here, once,
   and the terminal and framebuffer shells both draw it their own way.

   The cursor is a byte offset that is always on a character boundary. A
   textarea is the same model holding newlines; the line operations below
   move within and between them. */

#include "cutil.h"

#include <stdint.h>

typedef struct {
    DBuffer text;       /* always terminated */
    size_t cursor;      /* byte offset */
} DLineEdit;

/* Start with `initial` (NULL for empty) and the cursor at its end. */
void d_line_edit_init(DLineEdit *edit, const char *initial);
void d_line_edit_free(DLineEdit *edit);
/* Replace the text, cursor at the end. */
void d_line_edit_set(DLineEdit *edit, const char *text);

static inline const char *d_line_edit_text(const DLineEdit *edit)
{
    return edit->text.data ? edit->text.data : "";
}

void d_line_edit_insert(DLineEdit *edit, const char *bytes, size_t length);
void d_line_edit_insert_codepoint(DLineEdit *edit, uint32_t codepoint);

void d_line_edit_backspace(DLineEdit *edit);
void d_line_edit_delete(DLineEdit *edit);
void d_line_edit_left(DLineEdit *edit);
void d_line_edit_right(DLineEdit *edit);
/* To the start or end of the whole text (Ctrl+A, Ctrl+E in a line). */
void d_line_edit_home(DLineEdit *edit);
void d_line_edit_end(DLineEdit *edit);
/* To the start or end of the line the cursor is on, for a textarea. */
void d_line_edit_line_home(DLineEdit *edit);
void d_line_edit_line_end(DLineEdit *edit);
/* Up or down a line, keeping the byte column where the line allows. */
void d_line_edit_line_up(DLineEdit *edit);
void d_line_edit_line_down(DLineEdit *edit);
/* Delete everything before the cursor (Ctrl+U). */
void d_line_edit_kill_before(DLineEdit *edit);
/* Delete the word before the cursor (Ctrl+W). */
void d_line_edit_delete_word(DLineEdit *edit);

/* Bytes of `codepoint` in UTF-8, written to `out`; 0 for one that is not a
   character. */
size_t d_utf8_encode(uint32_t codepoint, char out[4]);

#endif
