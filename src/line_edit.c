/* The shared line editor; see line_edit.h. */

#include "line_edit.h"

void d_line_edit_init(DLineEdit *edit, const char *initial)
{
    dbuf_init(&edit->text);
    dbuf_puts(&edit->text, initial ? initial : "");
    edit->cursor = edit->text.length;
}

void d_line_edit_free(DLineEdit *edit)
{
    dbuf_free(&edit->text);
    edit->cursor = 0;
}

void d_line_edit_set(DLineEdit *edit, const char *text)
{
    d_line_edit_free(edit);
    d_line_edit_init(edit, text);
}

void d_line_edit_insert(DLineEdit *edit, const char *bytes, size_t length)
{
    if (!length) return;
    DBuffer *text = &edit->text;
    size_t at = edit->cursor;
    dbuf_reserve(text, length);
    memmove(text->data + at + length, text->data + at, text->length - at);
    memcpy(text->data + at, bytes, length);
    text->length += length;
    text->data[text->length] = '\0';
    edit->cursor += length;
}

size_t d_utf8_encode(uint32_t c, char out[4])
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c >= 0xd800 && c <= 0xdfff) return 0;
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c < 0x110000) {
        out[0] = (char)(0xf0 | c >> 18); out[1] = (char)(0x80 | (c >> 12 & 0x3f));
        out[2] = (char)(0x80 | (c >> 6 & 0x3f)); out[3] = (char)(0x80 | (c & 0x3f));
        return 4;
    }
    return 0;
}

void d_line_edit_insert_codepoint(DLineEdit *edit, uint32_t codepoint)
{
    char bytes[4];
    size_t length = d_utf8_encode(codepoint, bytes);
    d_line_edit_insert(edit, bytes, length);
}

static size_t previous_char(const char *text, size_t at)
{
    if (!at) return 0;
    --at;
    while (at && ((unsigned char)text[at] & 0xc0) == 0x80) --at;
    return at;
}

static size_t next_char(const char *text, size_t length, size_t at)
{
    if (at >= length) return length;
    ++at;
    while (at < length && ((unsigned char)text[at] & 0xc0) == 0x80) ++at;
    return at;
}

static void delete_range(DLineEdit *edit, size_t from, size_t to)
{
    DBuffer *text = &edit->text;
    if (to <= from || !text->data) return;
    memmove(text->data + from, text->data + to, text->length - to);
    text->length -= to - from;
    text->data[text->length] = '\0';
    edit->cursor = from;
}

void d_line_edit_backspace(DLineEdit *edit)
{
    delete_range(edit, previous_char(d_line_edit_text(edit), edit->cursor), edit->cursor);
}

void d_line_edit_delete(DLineEdit *edit)
{
    delete_range(edit, edit->cursor, next_char(d_line_edit_text(edit), edit->text.length, edit->cursor));
}

void d_line_edit_left(DLineEdit *edit)
{
    edit->cursor = previous_char(d_line_edit_text(edit), edit->cursor);
}

void d_line_edit_right(DLineEdit *edit)
{
    edit->cursor = next_char(d_line_edit_text(edit), edit->text.length, edit->cursor);
}

void d_line_edit_home(DLineEdit *edit) { edit->cursor = 0; }
void d_line_edit_end(DLineEdit *edit) { edit->cursor = edit->text.length; }

static size_t line_start(const char *text, size_t at)
{
    while (at && text[at - 1] != '\n') --at;
    return at;
}

static size_t line_end(const char *text, size_t length, size_t at)
{
    while (at < length && text[at] != '\n') ++at;
    return at;
}

void d_line_edit_line_home(DLineEdit *edit)
{
    edit->cursor = line_start(d_line_edit_text(edit), edit->cursor);
}

void d_line_edit_line_end(DLineEdit *edit)
{
    edit->cursor = line_end(d_line_edit_text(edit), edit->text.length, edit->cursor);
}

/* Place the cursor `column` bytes into the line starting at `start`, backed
   off to a character boundary. */
static void to_column(DLineEdit *edit, size_t start, size_t column)
{
    const char *text = d_line_edit_text(edit);
    size_t end = line_end(text, edit->text.length, start);
    size_t at = start + column < end ? start + column : end;
    while (at > start && ((unsigned char)text[at] & 0xc0) == 0x80) --at;
    edit->cursor = at;
}

void d_line_edit_line_up(DLineEdit *edit)
{
    const char *text = d_line_edit_text(edit);
    size_t start = line_start(text, edit->cursor);
    if (!start) { edit->cursor = 0; return; }
    to_column(edit, line_start(text, start - 1), edit->cursor - start);
}

void d_line_edit_line_down(DLineEdit *edit)
{
    const char *text = d_line_edit_text(edit);
    size_t start = line_start(text, edit->cursor);
    size_t end = line_end(text, edit->text.length, edit->cursor);
    if (end >= edit->text.length) { edit->cursor = edit->text.length; return; }
    to_column(edit, end + 1, edit->cursor - start);
}

void d_line_edit_kill_before(DLineEdit *edit)
{
    delete_range(edit, 0, edit->cursor);
}

void d_line_edit_delete_word(DLineEdit *edit)
{
    const char *text = d_line_edit_text(edit);
    size_t at = edit->cursor;
    while (at && text[at - 1] == ' ') --at;
    while (at && text[at - 1] != ' ') --at;
    delete_range(edit, at, edit->cursor);
}
