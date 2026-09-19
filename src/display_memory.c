/* The memory sink: a device that is a buffer, driven by a script.

   It is the framebuffer shell's gate (check.py --fb): everything above the
   sink runs as it would on a console, and what reached the "screen" can be
   written out and compared. Its rows are padded past width * 4 on purpose,
   because the real device on the machine this was written on is not, and a
   blit that ignores the stride must fail somewhere it can be seen.

   The spec is  memory:WxH[,cell=CWxCH][,script=PATH]  and the script is one
   command per line:

       send TEXT        bytes, as a console sends them, through the shared
                        decoder: keys and SGR mouse reports. \e \t \r \n \\
                        and \xHH are escapes.
       click X Y [B]    press and release a button at a pixel (1 left)
       press X Y [B]    release X Y [B]    move X Y
       wheel N          scroll N lines at the pointer (negative is up)
       resize W H       the device changes size
       suspend          resume
       wait [MS]        until the shell reports it is not busy (30 s default)
       sleep MS
       screenshot PATH  what the device shows, as a binary PPM
       state PATH       append the shell's latest state line
       quit

   Each event is handed out alone, and the next line is not read until the
   shell has reported back, so what a screenshot shows is everything the
   script did before it. */

#include "cutil.h"
#include "dclock.h"
#include "display_sink.h"
#include "input_decode.h"

#include <stdio.h>

enum { QUEUE_MAX = 4, DEVICE_PADDING = 13 };

typedef struct {
    DDisplay base;
    int width, height, stride;
    uint32_t *device;
    int cell_width, cell_height;
    char *script;
    size_t cursor;
    int line;
    DInputDecoder decoder;
    DBuffer send;               /* bytes of a send line not yet fed */
    size_t send_fed;
    DEvent queue[QUEUE_MAX];
    size_t queued;
    bool await_report;
    unsigned long reports, idle_report;
    bool waiting;
    unsigned long wait_from;
    uint64_t wait_deadline;
    uint64_t sleep_until;
    bool busy;
    char *state;
    int pointer_x, pointer_y;
    bool quit_sent;
} MemoryDisplay;

static bool allocate(MemoryDisplay *m, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    int stride = (width + DEVICE_PADDING) * 4;
    uint32_t *device = calloc((size_t)stride * (size_t)height, 1);
    if (!device) return false;
    free(m->device);
    m->device = device;
    m->width = width;
    m->height = height;
    m->stride = stride;
    return true;
}

static DDisplayMode memory_mode(const DDisplay *display)
{
    const MemoryDisplay *m = (const MemoryDisplay *)display;
    return (DDisplayMode){m->width, m->height, m->stride, m->cell_width, m->cell_height};
}

static size_t memory_handles(DDisplay *display, int *handles, size_t capacity)
{
    (void)display; (void)handles; (void)capacity;
    return 0;
}

static bool wait_satisfied(const MemoryDisplay *m)
{
    return m->idle_report > m->wait_from;
}

static bool script_done(const MemoryDisplay *m)
{
    return !m->script || !m->script[m->cursor];
}

static int memory_wait_ms(DDisplay *display)
{
    MemoryDisplay *m = (MemoryDisplay *)display;
    if (m->await_report) return -1;
    if (m->queued || m->send_fed < m->send.length || m->decoder.length) return 0;
    uint64_t now = d_clock_ms();
    if (m->waiting && !wait_satisfied(m))
        return m->wait_deadline > now ? (int)(m->wait_deadline - now) : 0;
    if (m->sleep_until > now) return (int)(m->sleep_until - now);
    if (script_done(m) && m->quit_sent) return -1;
    return 0;
}

static void write_screenshot(const MemoryDisplay *m, const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) { fprintf(stderr, "distill-fb: script line %d: cannot write %s\n", m->line, path); return; }
    fprintf(file, "P6\n%d %d\n255\n", m->width, m->height);
    unsigned char row[3 * 1024];
    for (int y = 0; y < m->height; ++y) {
        const uint32_t *pixels = (const uint32_t *)((const unsigned char *)m->device + (size_t)y * (size_t)m->stride);
        for (int x = 0; x < m->width;) {
            int n = 0;
            for (; n < 1024 && x < m->width; ++n, ++x) {
                uint32_t p = pixels[x];
                row[n * 3] = (unsigned char)(p >> 16);
                row[n * 3 + 1] = (unsigned char)(p >> 8);
                row[n * 3 + 2] = (unsigned char)p;
            }
            fwrite(row, 3, (size_t)n, file);
        }
    }
    fclose(file);
}

static void write_state(const MemoryDisplay *m, const char *path)
{
    FILE *file = fopen(path, "ab");
    if (!file) { fprintf(stderr, "distill-fb: script line %d: cannot write %s\n", m->line, path); return; }
    fprintf(file, "%s\n", m->state ? m->state : "{}");
    fclose(file);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void unescape_into(DBuffer *out, const char *text)
{
    for (const char *p = text; *p; ++p) {
        char c = *p;
        if (c == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'e': c = 0x1b; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'n': c = '\n'; break;
            case 'x':
                if (hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
                    c = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
                    p += 2;
                }
                break;
            default: c = *p; break;
            }
        }
        dbuf_append(out, &c, 1);
    }
}

/* The next line of the script, without its newline, in `line`. */
static bool next_line(MemoryDisplay *m, char *line, size_t capacity)
{
    if (script_done(m)) return false;
    const char *start = m->script + m->cursor;
    const char *end = strchr(start, '\n');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    m->cursor += length + (end ? 1 : 0);
    ++m->line;
    if (length >= capacity) length = capacity - 1;
    memcpy(line, start, length);
    line[length] = '\0';
    while (length && (line[length - 1] == '\r' || line[length - 1] == ' ')) line[--length] = '\0';
    return true;
}

static void queue(MemoryDisplay *m, DEvent event)
{
    if (m->queued < QUEUE_MAX) m->queue[m->queued++] = event;
}

static bool hand_out(MemoryDisplay *m, DEvent *event, DEvent value)
{
    *event = value;
    m->await_report = true;
    return true;
}

/* Run commands until one produces an event or the script must wait. */
static bool memory_next_event(DDisplay *display, DEvent *event)
{
    MemoryDisplay *m = (MemoryDisplay *)display;
    if (m->await_report) return false;
    for (;;) {
        if (m->queued) {
            DEvent first = m->queue[0];
            memmove(m->queue, m->queue + 1, --m->queued * sizeof(*m->queue));
            return hand_out(m, event, first);
        }
        if (m->send_fed < m->send.length || m->decoder.length) {
            m->send_fed += d_input_feed(&m->decoder, m->send.data + m->send_fed, m->send.length - m->send_fed);
            DEvent decoded;
            if (d_input_next(&m->decoder, &decoded, m->send_fed == m->send.length))
                return hand_out(m, event, decoded);
            if (m->send_fed == m->send.length && !m->decoder.length) { dbuf_free(&m->send); m->send_fed = 0; }
            continue;
        }
        uint64_t now = d_clock_ms();
        if (m->waiting) {
            if (!wait_satisfied(m)) {
                if (now < m->wait_deadline) return false;
                fprintf(stderr, "distill-fb: script line %d: still busy when the wait ran out\n", m->line);
            }
            m->waiting = false;
        }
        if (m->sleep_until > now) return false;
        m->sleep_until = 0;
        char line[1024];
        if (!next_line(m, line, sizeof(line))) {
            if (m->quit_sent) return false;
            m->quit_sent = true;
            return hand_out(m, event, (DEvent){.kind = D_EVENT_QUIT});
        }
        char command[32] = "";
        int consumed = 0;
        if (sscanf(line, "%31s %n", command, &consumed) < 1 || command[0] == '#') continue;
        const char *rest = line + consumed;
        int a = 0, b = 0, c = D_BUTTON_LEFT;
        if (!strcmp(command, "send")) {
            dbuf_free(&m->send);
            unescape_into(&m->send, rest);
            m->send_fed = 0;
        } else if (!strcmp(command, "click") && sscanf(rest, "%d %d %d", &a, &b, &c) >= 2) {
            m->pointer_x = a; m->pointer_y = b;
            queue(m, (DEvent){.kind = D_EVENT_POINTER_MOVE, .x = a, .y = b});
            queue(m, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = a, .y = b, .button = c, .pressed = true});
            queue(m, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = a, .y = b, .button = c, .pressed = false});
        } else if ((!strcmp(command, "press") || !strcmp(command, "release")) &&
                   sscanf(rest, "%d %d %d", &a, &b, &c) >= 2) {
            m->pointer_x = a; m->pointer_y = b;
            queue(m, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = a, .y = b, .button = c,
                              .pressed = !strcmp(command, "press")});
        } else if (!strcmp(command, "move") && sscanf(rest, "%d %d", &a, &b) == 2) {
            m->pointer_x = a; m->pointer_y = b;
            queue(m, (DEvent){.kind = D_EVENT_POINTER_MOVE, .x = a, .y = b});
        } else if (!strcmp(command, "wheel") && sscanf(rest, "%d", &a) == 1) {
            queue(m, (DEvent){.kind = D_EVENT_SCROLL, .x = m->pointer_x, .y = m->pointer_y, .lines = a});
        } else if (!strcmp(command, "resize") && sscanf(rest, "%d %d", &a, &b) == 2) {
            if (allocate(m, a, b)) queue(m, (DEvent){.kind = D_EVENT_RESIZE});
            else fprintf(stderr, "distill-fb: script line %d: cannot resize to %dx%d\n", m->line, a, b);
        } else if (!strcmp(command, "suspend")) {
            queue(m, (DEvent){.kind = D_EVENT_SUSPEND});
        } else if (!strcmp(command, "resume")) {
            queue(m, (DEvent){.kind = D_EVENT_RESUME});
        } else if (!strcmp(command, "wait")) {
            int limit = 30000;
            sscanf(rest, "%d", &limit);
            m->waiting = true;
            m->wait_from = m->reports;
            m->wait_deadline = now + (uint64_t)(limit > 0 ? limit : 30000);
        } else if (!strcmp(command, "sleep") && sscanf(rest, "%d", &a) == 1) {
            m->sleep_until = now + (uint64_t)(a > 0 ? a : 0);
        } else if (!strcmp(command, "screenshot") && *rest) {
            write_screenshot(m, rest);
        } else if (!strcmp(command, "state") && *rest) {
            write_state(m, rest);
        } else if (!strcmp(command, "quit")) {
            m->quit_sent = true;
            m->cursor = strlen(m->script);
            return hand_out(m, event, (DEvent){.kind = D_EVENT_QUIT});
        } else {
            fprintf(stderr, "distill-fb: script line %d: cannot read \"%s\"\n", m->line, line);
        }
    }
}

static void memory_present(DDisplay *display, const uint32_t *pixels, int stride,
                           const DRenderRect *damage, size_t damage_count)
{
    MemoryDisplay *m = (MemoryDisplay *)display;
    d_display_copy(m->device, m->stride, m->width, m->height, pixels, stride, damage, damage_count);
}

static void memory_report(DDisplay *display, bool busy, const char *state, const char *title)
{
    (void)title;
    MemoryDisplay *m = (MemoryDisplay *)display;
    m->await_report = false;
    ++m->reports;
    m->busy = busy;
    if (!busy) m->idle_report = m->reports;
    free(m->state);
    m->state = d_strdup(state ? state : "{}");
}

static void memory_close(DDisplay *display)
{
    MemoryDisplay *m = (MemoryDisplay *)display;
    free(m->device);
    free(m->script);
    free(m->state);
    dbuf_free(&m->send);
    free(m);
}

static const DDisplayOps memory_ops = {
    memory_mode, memory_handles, memory_wait_ms, memory_next_event,
    memory_present, memory_report, memory_close,
};

DDisplay *d_display_memory_open(const char *spec, char **error)
{
    *error = NULL;
    const char *p = spec + strlen("memory:");
    int width = 0, height = 0;
    if (!d_display_parse_size(p, &width, &height)) {
        *error = d_strdup("the memory display needs a size, as memory:1280x800");
        return NULL;
    }
    MemoryDisplay *m = d_xcalloc(1, sizeof(*m));
    m->base.ops = &memory_ops;
    m->cell_width = 8;
    m->cell_height = 16;
    const char *cell = strstr(p, ",cell=");
    if (cell) d_display_parse_size(cell + 6, &m->cell_width, &m->cell_height);
    const char *script = strstr(p, ",script=");
    if (script) {
        m->script = d_read_file(script + 8, NULL);
        if (!m->script) {
            char message[512];
            snprintf(message, sizeof(message), "cannot read the script %s", script + 8);
            *error = d_strdup(message);
            free(m);
            return NULL;
        }
    }
    if (!allocate(m, width, height)) {
        *error = d_strdup("the memory display's size is out of range");
        free(m->script);
        free(m);
        return NULL;
    }
    d_input_init(&m->decoder, m->cell_width, m->cell_height);
    return &m->base;
}
