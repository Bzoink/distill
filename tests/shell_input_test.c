/* The shared input decoder and line editor, host-tested once for every shell
   that uses them: the Linux console and Muroxi feed the decoder the same
   bytes, and the terminal and framebuffer shells edit with the same model. */

#include "input_decode.h"
#include "line_edit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t decode(const char *bytes, DEvent *events, size_t capacity)
{
    DInputDecoder decoder;
    d_input_init(&decoder, 10, 20);
    size_t length = strlen(bytes), fed = 0, count = 0;
    while (fed < length || decoder.length) {
        fed += d_input_feed(&decoder, bytes + fed, length - fed);
        DEvent event;
        bool any = false;
        while (d_input_next(&decoder, &event, fed == length)) {
            assert(count < capacity);
            events[count++] = event;
            any = true;
        }
        if (!any && fed == length) break;
    }
    return count;
}

static void test_keys(void)
{
    DEvent e[16];
    assert(decode("a\xc3\xa5\xe2\x82\xac", e, 16) == 3);
    assert(e[0].key == D_KEY_CHAR && e[0].codepoint == 'a' && !e[0].mods);
    assert(e[1].codepoint == 0xe5);
    assert(e[2].codepoint == 0x20ac);

    assert(decode("\r\t\x7f\x0c", e, 16) == 4);
    assert(e[0].key == D_KEY_ENTER && e[1].key == D_KEY_TAB && e[2].key == D_KEY_BACKSPACE);
    assert(e[3].key == D_KEY_CHAR && e[3].codepoint == 'l' && e[3].mods == D_MOD_CTRL);

    /* The Linux console's own sequences and xterm's. */
    assert(decode("\x1b[A\x1b[B\x1b[C\x1b[D\x1b[1~\x1b[4~\x1b[5~\x1b[6~\x1b[3~", e, 16) == 9);
    assert(e[0].key == D_KEY_UP && e[1].key == D_KEY_DOWN && e[2].key == D_KEY_RIGHT);
    assert(e[3].key == D_KEY_LEFT && e[4].key == D_KEY_HOME && e[5].key == D_KEY_END);
    assert(e[6].key == D_KEY_PAGE_UP && e[7].key == D_KEY_PAGE_DOWN && e[8].key == D_KEY_DELETE);
    assert(decode("\x1b[[A\x1b[[E\x1b[15~\x1b[24~\x1bOP", e, 16) == 5);
    assert(e[0].key == D_KEY_F1 && e[1].key == D_KEY_F1 + 4 && e[2].key == D_KEY_F1 + 4);
    assert(e[3].key == D_KEY_F12 && e[4].key == D_KEY_F1);
    assert(decode("\x1b[Z\x1b[1;5C\x1b[H\x1bOF", e, 16) == 4);
    assert(e[0].key == D_KEY_TAB && e[0].mods == D_MOD_SHIFT);
    assert(e[1].key == D_KEY_RIGHT && e[1].mods == D_MOD_CTRL);
    assert(e[2].key == D_KEY_HOME && e[3].key == D_KEY_END);

    /* A lone Escape at the end of a read is the key; Escape and a key in the
       same read is Alt and the key. */
    assert(decode("\x1b", e, 16) == 1 && e[0].key == D_KEY_ESCAPE);
    assert(decode("\x1b" "1", e, 16) == 1 && e[0].codepoint == '1' && e[0].mods == D_MOD_ALT);
    /* A sequence split across reads waits for its end. */
    DInputDecoder decoder;
    d_input_init(&decoder, 10, 20);
    DEvent event;
    d_input_feed(&decoder, "\x1b[", 2);
    assert(!d_input_next(&decoder, &event, false));
    d_input_feed(&decoder, "B", 1);
    assert(d_input_next(&decoder, &event, true) && event.key == D_KEY_DOWN);
    /* Unknown sequences are consumed without an event. */
    assert(decode("\x1b[?1;2cq", e, 16) == 1 && e[0].codepoint == 'q');
}

static void test_mouse(void)
{
    DEvent e[16];
    /* Cells are 1-based; a cell becomes the pixel at its centre. */
    assert(decode("\x1b[<0;3;2M\x1b[<0;3;2m", e, 16) == 2);
    assert(e[0].kind == D_EVENT_POINTER_BUTTON && e[0].pressed && e[0].button == D_BUTTON_LEFT);
    assert(e[0].x == 25 && e[0].y == 30);
    assert(e[1].kind == D_EVENT_POINTER_BUTTON && !e[1].pressed);
    assert(decode("\x1b[<35;1;1M\x1b[<64;5;5M\x1b[<65;5;5M\x1b[<2;1;1M", e, 16) == 4);
    assert(e[0].kind == D_EVENT_POINTER_MOVE && e[0].x == 5 && e[0].y == 10);
    assert(e[1].kind == D_EVENT_SCROLL && e[1].lines == -1);
    assert(e[2].kind == D_EVENT_SCROLL && e[2].lines == 1);
    assert(e[3].kind == D_EVENT_POINTER_BUTTON && e[3].button == D_BUTTON_RIGHT);
    assert(decode("\x1b[<16;4;4M", e, 16) == 1 && e[0].mods == D_MOD_CTRL);
}

static void test_line_edit(void)
{
    DLineEdit edit;
    d_line_edit_init(&edit, "h\xc3\xa5l");
    assert(edit.cursor == 4);
    d_line_edit_left(&edit);
    d_line_edit_left(&edit);
    assert(edit.cursor == 1);          /* over the two bytes of å, not into them */
    d_line_edit_insert_codepoint(&edit, 0x20ac);
    assert(!strcmp(d_line_edit_text(&edit), "h\xe2\x82\xac\xc3\xa5l"));
    d_line_edit_delete(&edit);
    assert(!strcmp(d_line_edit_text(&edit), "h\xe2\x82\xacl"));
    d_line_edit_backspace(&edit);
    assert(!strcmp(d_line_edit_text(&edit), "hl") && edit.cursor == 1);
    d_line_edit_set(&edit, "one two  three");
    d_line_edit_delete_word(&edit);
    assert(!strcmp(d_line_edit_text(&edit), "one two  "));
    d_line_edit_left(&edit);
    d_line_edit_kill_before(&edit);
    assert(!strcmp(d_line_edit_text(&edit), " ") && edit.cursor == 0);

    /* A textarea: lines and columns. */
    d_line_edit_set(&edit, "first line\nab\nthird");
    d_line_edit_line_up(&edit);
    assert(edit.cursor == 13);         /* "ab" is shorter: its end */
    d_line_edit_line_up(&edit);
    assert(edit.cursor == 2);
    d_line_edit_line_end(&edit);
    assert(edit.cursor == 10);
    d_line_edit_line_down(&edit);
    assert(edit.cursor == 13);
    d_line_edit_line_home(&edit);
    assert(edit.cursor == 11);
    d_line_edit_line_down(&edit);
    assert(edit.cursor == 14);
    d_line_edit_line_down(&edit);
    assert(edit.cursor == strlen(d_line_edit_text(&edit)));
    d_line_edit_free(&edit);

    char bytes[4];
    assert(d_utf8_encode(0x1f600, bytes) == 4 && (unsigned char)bytes[0] == 0xf0);
    assert(d_utf8_encode(0xd800, bytes) == 0);
}

int main(void)
{
    test_keys();
    test_mouse();
    test_line_edit();
    puts("shell-input: ok");
    return 0;
}
