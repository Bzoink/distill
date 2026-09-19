/* ANSI keys and SGR mouse reports into DEvents; see input_decode.h. */

#include "input_decode.h"

#include <string.h>

void d_input_init(DInputDecoder *decoder, int cell_width, int cell_height)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->cell_width = cell_width > 0 ? cell_width : 8;
    decoder->cell_height = cell_height > 0 ? cell_height : 16;
}

size_t d_input_feed(DInputDecoder *decoder, const void *bytes, size_t length)
{
    size_t room = sizeof(decoder->pending) - decoder->length;
    if (length > room) length = room;
    memcpy(decoder->pending + decoder->length, bytes, length);
    decoder->length += length;
    return length;
}

static void take(DInputDecoder *decoder, size_t count)
{
    if (count > decoder->length) count = decoder->length;
    memmove(decoder->pending, decoder->pending + count, decoder->length - count);
    decoder->length -= count;
}

typedef enum { PARSE_EVENT, PARSE_SKIP, PARSE_INCOMPLETE } Parse;

static void key_event(DEvent *event, DKey key, unsigned mods)
{
    *event = (DEvent){.kind = D_EVENT_KEY, .key = key, .mods = mods};
}

static void char_event(DEvent *event, uint32_t codepoint, unsigned mods)
{
    *event = (DEvent){.kind = D_EVENT_KEY, .key = D_KEY_CHAR, .codepoint = codepoint, .mods = mods};
}

/* One key that is not an escape sequence: a control byte or a UTF-8
   character. `used` is how many bytes it took. */
static Parse parse_plain(const unsigned char *p, size_t length, unsigned mods, DEvent *event,
                         size_t *used)
{
    unsigned char c = p[0];
    *used = 1;
    if (c == '\r' || c == '\n') { key_event(event, D_KEY_ENTER, mods); return PARSE_EVENT; }
    if (c == '\t') { key_event(event, D_KEY_TAB, mods); return PARSE_EVENT; }
    if (c == 0x7f || c == 0x08) { key_event(event, D_KEY_BACKSPACE, mods); return PARSE_EVENT; }
    if (c >= 0x01 && c <= 0x1a) { char_event(event, 'a' + c - 1, mods | D_MOD_CTRL); return PARSE_EVENT; }
    if (c < 0x20) return PARSE_SKIP;
    if (c < 0x80) { char_event(event, c, mods); return PARSE_EVENT; }
    size_t need = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!need) { char_event(event, 0xfffd, mods); return PARSE_EVENT; }
    if (length < need) return PARSE_INCOMPLETE;
    uint32_t codepoint = c & (need == 2 ? 0x1f : need == 3 ? 0x0f : 0x07);
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0xc0) != 0x80) { char_event(event, 0xfffd, mods); return PARSE_EVENT; }
        codepoint = codepoint << 6 | (p[i] & 0x3f);
    }
    *used = need;
    char_event(event, codepoint, mods);
    return PARSE_EVENT;
}

static unsigned modifier_param(int value)
{
    if (value < 2) return 0;
    unsigned bits = (unsigned)(value - 1), mods = 0;
    if (bits & 1) mods |= D_MOD_SHIFT;
    if (bits & 2) mods |= D_MOD_ALT;
    if (bits & 4) mods |= D_MOD_CTRL;
    return mods;
}

static DKey tilde_key(int code)
{
    switch (code) {
    case 1: case 7: return D_KEY_HOME;
    case 2: return D_KEY_INSERT;
    case 3: return D_KEY_DELETE;
    case 4: case 8: return D_KEY_END;
    case 5: return D_KEY_PAGE_UP;
    case 6: return D_KEY_PAGE_DOWN;
    case 11: case 12: case 13: case 14: case 15: return (DKey)(D_KEY_F1 + code - 11);
    case 17: case 18: case 19: case 20: case 21: return (DKey)(D_KEY_F1 + 5 + code - 17);
    case 23: case 24: return (DKey)(D_KEY_F1 + 10 + code - 23);
    default: return D_KEY_NONE;
    }
}

static DKey letter_key(unsigned char final)
{
    switch (final) {
    case 'A': return D_KEY_UP;
    case 'B': return D_KEY_DOWN;
    case 'C': return D_KEY_RIGHT;
    case 'D': return D_KEY_LEFT;
    case 'H': return D_KEY_HOME;
    case 'F': return D_KEY_END;
    case 'P': return D_KEY_F1;
    case 'Q': return (DKey)(D_KEY_F1 + 1);
    case 'R': return (DKey)(D_KEY_F1 + 2);
    case 'S': return (DKey)(D_KEY_F1 + 3);
    case 'M': return D_KEY_ENTER;
    default: return D_KEY_NONE;
    }
}

/* An SGR report, ESC [ < b ; x ; y (M|m), after the '<'. */
static Parse parse_mouse(DInputDecoder *decoder, const unsigned char *p, size_t length,
                         DEvent *event, size_t *used)
{
    int values[3] = {0, 0, 0};
    int count = 0;
    bool digits = false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = p[i];
        if (c >= '0' && c <= '9') {
            if (values[count] < 100000) values[count] = values[count] * 10 + (c - '0');
            digits = true;
        } else if (c == ';') {
            if (!digits || count == 2) { *used = i + 1; return PARSE_SKIP; }
            ++count;
            digits = false;
        } else if ((c == 'M' || c == 'm') && count == 2 && digits) {
            *used = i + 1;
            int b = values[0];
            int x = (values[1] > 0 ? values[1] - 1 : 0) * decoder->cell_width + decoder->cell_width / 2;
            int y = (values[2] > 0 ? values[2] - 1 : 0) * decoder->cell_height + decoder->cell_height / 2;
            unsigned mods = (b & 4 ? D_MOD_SHIFT : 0) | (b & 8 ? D_MOD_ALT : 0) | (b & 16 ? D_MOD_CTRL : 0);
            decoder->pointer_x = x;
            decoder->pointer_y = y;
            if (b & 64) {
                if (c == 'm' || (b & 3) > 1) return PARSE_SKIP;
                *event = (DEvent){.kind = D_EVENT_SCROLL, .x = x, .y = y, .mods = mods,
                                  .lines = (b & 3) == 0 ? -1 : 1};
                return PARSE_EVENT;
            }
            if (b & 32) {
                *event = (DEvent){.kind = D_EVENT_POINTER_MOVE, .x = x, .y = y, .mods = mods};
                return PARSE_EVENT;
            }
            int button = (b & 3) == 0 ? D_BUTTON_LEFT : (b & 3) == 1 ? D_BUTTON_MIDDLE
                       : (b & 3) == 2 ? D_BUTTON_RIGHT : D_BUTTON_LEFT;
            bool pressed = c == 'M' && (b & 3) != 3;
            *event = (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = x, .y = y, .mods = mods,
                              .button = button, .pressed = pressed};
            return PARSE_EVENT;
        } else {
            *used = i + 1;
            return PARSE_SKIP;
        }
        if (i >= 24) { *used = i + 1; return PARSE_SKIP; }
    }
    return PARSE_INCOMPLETE;
}

/* A control sequence, after ESC [. */
static Parse parse_csi(DInputDecoder *decoder, const unsigned char *p, size_t length,
                       unsigned mods, DEvent *event, size_t *used)
{
    if (!length) return PARSE_INCOMPLETE;
    if (p[0] == '[') {
        /* The Linux console's F1 to F5: ESC [ [ A to E. */
        if (length < 2) return PARSE_INCOMPLETE;
        *used = 2;
        if (p[1] < 'A' || p[1] > 'E') return PARSE_SKIP;
        key_event(event, (DKey)(D_KEY_F1 + p[1] - 'A'), mods);
        return PARSE_EVENT;
    }
    if (p[0] == '<') {
        Parse result = parse_mouse(decoder, p + 1, length - 1, event, used);
        if (result != PARSE_INCOMPLETE) ++*used;
        return result;
    }
    int values[4] = {0, 0, 0, 0};
    int count = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = p[i];
        if (c >= '0' && c <= '9') {
            if (count < 4 && values[count] < 10000) values[count] = values[count] * 10 + (c - '0');
            continue;
        }
        if (c == ';') { ++count; continue; }
        if (c >= 0x20 && c <= 0x3f) continue;      /* other parameter or intermediate bytes */
        *used = i + 1;
        if (c < 0x40 || c > 0x7e) return PARSE_SKIP;
        unsigned extra = modifier_param(count >= 1 ? values[1] : 0);
        if (c == '~') {
            DKey key = tilde_key(values[0]);
            if (key == D_KEY_NONE) return PARSE_SKIP;
            key_event(event, key, mods | extra);
            return PARSE_EVENT;
        }
        if (c == 'Z') { key_event(event, D_KEY_TAB, mods | extra | D_MOD_SHIFT); return PARSE_EVENT; }
        DKey key = letter_key(c);
        if (key == D_KEY_NONE) return PARSE_SKIP;
        key_event(event, key, mods | extra);
        return PARSE_EVENT;
    }
    if (length > 32) { *used = length; return PARSE_SKIP; }
    return PARSE_INCOMPLETE;
}

bool d_input_next(DInputDecoder *decoder, DEvent *event, bool batch_end)
{
    for (;;) {
        const unsigned char *p = decoder->pending;
        size_t length = decoder->length;
        if (!length) return false;
        size_t used = 0;
        Parse result;
        if (p[0] != 0x1b) {
            result = parse_plain(p, length, 0, event, &used);
        } else if (length == 1) {
            if (!batch_end) return false;
            key_event(event, D_KEY_ESCAPE, 0);
            used = 1;
            result = PARSE_EVENT;
        } else if (p[1] == '[') {
            result = parse_csi(decoder, p + 2, length - 2, 0, event, &used);
            used += 2;
        } else if (p[1] == 'O') {
            if (length < 3) {
                result = PARSE_INCOMPLETE;
            } else {
                used = 3;
                DKey key = letter_key(p[2]);
                result = key == D_KEY_NONE ? PARSE_SKIP : PARSE_EVENT;
                if (key != D_KEY_NONE) key_event(event, key, 0);
            }
        } else if (p[1] == 0x1b) {
            key_event(event, D_KEY_ESCAPE, 0);
            used = 1;
            result = PARSE_EVENT;
        } else {
            /* Escape and a key in one batch: Alt and that key. */
            result = parse_plain(p + 1, length - 1, D_MOD_ALT, event, &used);
            used += 1;
        }
        if (result == PARSE_INCOMPLETE) {
            if (!batch_end && length < sizeof(decoder->pending)) return false;
            /* Cut short: an Escape the reader pressed, then whatever it was. */
            if (p[0] == 0x1b) {
                take(decoder, 1);
                key_event(event, D_KEY_ESCAPE, 0);
                return true;
            }
            take(decoder, length);
            return false;
        }
        take(decoder, used);
        if (result == PARSE_EVENT) return true;
    }
}
