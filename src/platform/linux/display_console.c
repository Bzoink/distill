/* The console sink on Linux: pixels on /dev/fb0, keys from the virtual
   console's tty, the pointer from evdev (FRAMEBUFFER_PLAN.md, "The console").

   - It refuses to start anywhere but a virtual console, and on one another
     program already has in graphics mode, rather than scribble over a screen
     X or a compositor owns.
   - KD_GRAPHICS stops the kernel drawing text and its cursor over us.
   - VT_PROCESS makes a VT switch ask first: on release we stop presenting and
     stop reading the mouse, then allow it; on acquire we take the screen back
     and the shell redraws everything, since the console drew in between.
   - The keyboard stays the tty's, in raw mode. The console keymap still
     applies, so a reader's layout, dead keys and compose arrive as text, and
     Ctrl+Alt+Fn still switches VTs, done by the kernel -- not by us, where a
     bug would strand the reader on this screen.
   - Everything is put back on close, and from a signal handler on a crash:
     text mode, automatic VT switching, the tty's mode. SIGKILL cannot be
     caught; switching VT and back, or `kbd_mode -u; chvt N`, recovers. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "cutil.h"
#include "display_sink.h"
#include "input_decode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

enum { MICE_MAX = 4, QUEUE_MAX = 64, CURSOR_W = 12, CURSOR_H = 19 };

/* What a crash must undo, where a signal handler can reach it. */
static volatile sig_atomic_t console_active;
static int console_tty = -1;
static struct termios console_saved_termios;
static int console_saved_kd = KD_TEXT;
static struct vt_mode console_saved_vt;
static int console_wake[2] = {-1, -1};
static volatile sig_atomic_t console_release, console_acquire;

typedef struct {
    DDisplay base;
    int fb;
    unsigned char *memory;
    size_t memory_size;
    unsigned char *origin;          /* the visible screen within the mapping */
    int width, height, stride;
    int cell_width, cell_height;
    DInputDecoder decoder;
    int mice[MICE_MAX];
    size_t mouse_count;
    DEvent queue[QUEUE_MAX];
    size_t queue_head, queue_count;
    bool suspended;
    /* The pointer, drawn on the device only: the back buffer never has it,
       so moving it puts back what the last present showed there. */
    int pointer_x, pointer_y;
    bool pointer_shown, pointer_drawn;
    int drawn_x, drawn_y;
    const uint32_t *last_pixels;
    int last_stride;
    int pending_dx, pending_dy;
} ConsoleDisplay;

static void on_release(int signal_number)
{
    (void)signal_number;
    console_release = 1;
    if (console_wake[1] >= 0) { ssize_t ignored = write(console_wake[1], "r", 1); (void)ignored; }
}

static void on_acquire(int signal_number)
{
    (void)signal_number;
    console_acquire = 1;
    if (console_wake[1] >= 0) { ssize_t ignored = write(console_wake[1], "a", 1); (void)ignored; }
}

void d_display_console_restore(void)
{
    if (!console_active) return;
    console_active = 0;
    ioctl(console_tty, KDSETMODE, console_saved_kd);
    ioctl(console_tty, VT_SETMODE, &console_saved_vt);
    tcsetattr(console_tty, TCSAFLUSH, &console_saved_termios);
}

/* ---- the pointer ---------------------------------------------------------- */

static const char *const cursor_shape[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};

static uint32_t *device_row(ConsoleDisplay *c, int y)
{
    return (uint32_t *)(c->origin + (size_t)y * (size_t)c->stride);
}

static void cursor_restore(ConsoleDisplay *c)
{
    if (!c->pointer_drawn) return;
    c->pointer_drawn = false;
    if (!c->last_pixels) return;
    DRenderRect rect = {c->drawn_x, c->drawn_y, CURSOR_W, CURSOR_H};
    d_display_copy((uint32_t *)c->origin, c->stride, c->width, c->height,
                   c->last_pixels, c->last_stride, &rect, 1);
}

static void cursor_draw(ConsoleDisplay *c)
{
    if (!c->pointer_shown || c->suspended) return;
    for (int y = 0; y < CURSOR_H; ++y) {
        int py = c->pointer_y + y;
        if (py < 0 || py >= c->height) continue;
        uint32_t *row = device_row(c, py);
        for (int x = 0; x < CURSOR_W; ++x) {
            int px = c->pointer_x + x;
            char mark = cursor_shape[y][x];
            if (px < 0 || px >= c->width || mark == ' ') continue;
            row[px] = mark == 'X' ? 0xff000000u : 0xffffffffu;
        }
    }
    c->pointer_drawn = true;
    c->drawn_x = c->pointer_x;
    c->drawn_y = c->pointer_y;
}

/* ---- input ---------------------------------------------------------------- */

static void enqueue(ConsoleDisplay *c, DEvent event)
{
    if (c->queue_count == QUEUE_MAX) return;
    c->queue[(c->queue_head + c->queue_count++) % QUEUE_MAX] = event;
}

static bool dequeue(ConsoleDisplay *c, DEvent *event)
{
    if (!c->queue_count) return false;
    *event = c->queue[c->queue_head];
    c->queue_head = (c->queue_head + 1) % QUEUE_MAX;
    --c->queue_count;
    return true;
}

static bool is_mouse(int fd)
{
    unsigned long relative = 0, keys[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative)), &relative) < 0) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return false;
    bool motion = (relative & (1ul << REL_X)) && (relative & (1ul << REL_Y));
    size_t bits = 8 * sizeof(unsigned long);
    bool left = keys[BTN_LEFT / bits] & (1ul << (BTN_LEFT % bits));
    return motion && left;
}

/* Relative mice, by capability. A touchpad reports absolute positions and
   is not one of these: that is FRAMEBUFFER_PLAN.md's open decision. */
static void open_mice(ConsoleDisplay *c)
{
    DIR *directory = opendir("/dev/input");
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) && c->mouse_count < MICE_MAX) {
        if (strncmp(entry->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (is_mouse(fd)) c->mice[c->mouse_count++] = fd;
        else close(fd);
    }
    closedir(directory);
}

static void pointer_moved(ConsoleDisplay *c)
{
    c->pointer_x += c->pending_dx;
    c->pointer_y += c->pending_dy;
    c->pending_dx = c->pending_dy = 0;
    if (c->pointer_x < 0) c->pointer_x = 0;
    if (c->pointer_y < 0) c->pointer_y = 0;
    if (c->pointer_x >= c->width) c->pointer_x = c->width - 1;
    if (c->pointer_y >= c->height) c->pointer_y = c->height - 1;
    c->pointer_shown = true;
    cursor_restore(c);
    cursor_draw(c);
    enqueue(c, (DEvent){.kind = D_EVENT_POINTER_MOVE, .x = c->pointer_x, .y = c->pointer_y});
}

static void read_mouse(ConsoleDisplay *c, int fd)
{
    struct input_event events[16];
    for (;;) {
        ssize_t got = read(fd, events, sizeof(events));
        if (got <= 0) return;
        if (c->suspended) continue;             /* read and dropped while away */
        for (size_t i = 0; i < (size_t)got / sizeof(events[0]); ++i) {
            const struct input_event *e = &events[i];
            if (e->type == EV_REL && e->code == REL_X) c->pending_dx += e->value;
            else if (e->type == EV_REL && e->code == REL_Y) c->pending_dy += e->value;
            else if (e->type == EV_REL && e->code == REL_WHEEL && e->value)
                enqueue(c, (DEvent){.kind = D_EVENT_SCROLL, .x = c->pointer_x, .y = c->pointer_y,
                                    .lines = -e->value});
            else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE)) {
                if (c->pending_dx || c->pending_dy) pointer_moved(c);
                c->pointer_shown = true;
                int button = e->code == BTN_LEFT ? D_BUTTON_LEFT : e->code == BTN_RIGHT ? D_BUTTON_RIGHT
                                                                                      : D_BUTTON_MIDDLE;
                enqueue(c, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = c->pointer_x, .y = c->pointer_y,
                                    .button = button, .pressed = e->value != 0});
            } else if (e->type == EV_SYN && (c->pending_dx || c->pending_dy)) {
                pointer_moved(c);
            }
        }
    }
}

static void read_keys(ConsoleDisplay *c)
{
    unsigned char bytes[128];
    for (;;) {
        ssize_t got = read(console_tty, bytes, sizeof(bytes));
        if (got <= 0) break;
        size_t fed = 0;
        while (fed < (size_t)got) {
            fed += d_input_feed(&c->decoder, bytes + fed, (size_t)got - fed);
            DEvent event;
            while (d_input_next(&c->decoder, &event, fed == (size_t)got)) enqueue(c, event);
        }
    }
}

static DDisplayMode console_mode(const DDisplay *display)
{
    const ConsoleDisplay *c = (const ConsoleDisplay *)display;
    return (DDisplayMode){c->width, c->height, c->stride, c->cell_width, c->cell_height};
}

static size_t console_handles(DDisplay *display, int *handles, size_t capacity)
{
    ConsoleDisplay *c = (ConsoleDisplay *)display;
    size_t count = 0;
    if (count < capacity) handles[count++] = console_tty;
    if (count < capacity) handles[count++] = console_wake[0];
    for (size_t i = 0; i < c->mouse_count && count < capacity; ++i) handles[count++] = c->mice[i];
    return count;
}

static int console_wait_ms(DDisplay *display)
{
    ConsoleDisplay *c = (ConsoleDisplay *)display;
    return c->queue_count || console_release || console_acquire ? 0 : -1;
}

static bool console_next_event(DDisplay *display, DEvent *event)
{
    ConsoleDisplay *c = (ConsoleDisplay *)display;
    char drain[16];
    while (read(console_wake[0], drain, sizeof(drain)) > 0) {}
    if (console_release) {
        console_release = 0;
        /* Stop first, then let the switch happen. */
        c->suspended = true;
        c->pointer_drawn = false;
        ioctl(console_tty, VT_RELDISP, 1);
        *event = (DEvent){.kind = D_EVENT_SUSPEND};
        return true;
    }
    if (console_acquire) {
        console_acquire = 0;
        ioctl(console_tty, VT_RELDISP, VT_ACKACQ);
        c->suspended = false;
        *event = (DEvent){.kind = D_EVENT_RESUME};
        return true;
    }
    if (dequeue(c, event)) return true;
    read_keys(c);
    for (size_t i = 0; i < c->mouse_count; ++i) read_mouse(c, c->mice[i]);
    return dequeue(c, event);
}

static void console_present(DDisplay *display, const uint32_t *pixels, int stride,
                            const DRenderRect *damage, size_t damage_count)
{
    ConsoleDisplay *c = (ConsoleDisplay *)display;
    c->last_pixels = pixels;
    c->last_stride = stride;
    if (c->suspended) return;
    d_display_copy((uint32_t *)c->origin, c->stride, c->width, c->height, pixels, stride,
                   damage, damage_count);
    c->pointer_drawn = false;
    cursor_draw(c);
}

static void console_close(DDisplay *display)
{
    ConsoleDisplay *c = (ConsoleDisplay *)display;
    d_display_console_restore();
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    for (size_t i = 0; i < c->mouse_count; ++i) close(c->mice[i]);
    if (c->memory) munmap(c->memory, c->memory_size);
    if (c->fb >= 0) close(c->fb);
    for (int i = 0; i < 2; ++i) if (console_wake[i] >= 0) { close(console_wake[i]); console_wake[i] = -1; }
    free(c);
}

static const DDisplayOps console_ops = {
    console_mode, console_handles, console_wait_ms, console_next_event,
    console_present, NULL, console_close,
};

static DDisplay *refuse(ConsoleDisplay *c, char **error, const char *format, const char *detail)
{
    char message[512];
    snprintf(message, sizeof(message), format, detail ? detail : "");
    *error = d_strdup(message);
    if (c) console_close(&c->base);
    return NULL;
}

DDisplay *d_display_console_open(const char *spec, char **error)
{
    *error = NULL;
    console_tty = STDIN_FILENO;
    int kd_mode = KD_TEXT;
    if (ioctl(console_tty, KDGETMODE, &kd_mode) < 0)
        return refuse(NULL, error,
                      "distill-fb draws on a Linux virtual console, and standard input is not one. "
                      "Run it on a text console (Ctrl+Alt+F3, say); inside a desktop, run distill-fb-x11%s", NULL);
    if (kd_mode == KD_GRAPHICS)
        return refuse(NULL, error, "this console is already in graphics mode: another program owns it%s", NULL);

    const char *device = spec[7] == ':' && spec[8] ? spec + 8 : getenv("FRAMEBUFFER");
    if (!device || !*device) device = "/dev/fb0";
    ConsoleDisplay *c = d_xcalloc(1, sizeof(*c));
    c->base.ops = &console_ops;
    c->fb = open(device, O_RDWR | O_CLOEXEC);
    if (c->fb < 0) return refuse(c, error, "cannot open %s (is this user in the video group?)", device);
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(c->fb, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(c->fb, FBIOGET_FSCREENINFO, &fix) < 0)
        return refuse(c, error, "%s is not a framebuffer", device);
    /* Refuse anything but 32-bit XRGB rather than convert: this is the one
       format the painter makes, and every emulated fbdev offers it. */
    if (var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 ||
        var.blue.offset != 0 || var.red.length != 8 || fix.visual != FB_VISUAL_TRUECOLOR)
        return refuse(c, error, "%s is not 32-bit XRGB8888, the one format Distill draws", device);
    c->width = (int)var.xres;
    c->height = (int)var.yres;
    c->stride = (int)fix.line_length;
    c->memory_size = fix.smem_len;
    c->memory = mmap(NULL, c->memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fb, 0);
    if (c->memory == MAP_FAILED) { c->memory = NULL; return refuse(c, error, "cannot map %s", device); }
    c->origin = c->memory + (size_t)var.yoffset * fix.line_length + (size_t)var.xoffset * 4;

    struct winsize size;
    c->cell_width = 8;
    c->cell_height = 16;
    if (ioctl(console_tty, TIOCGWINSZ, &size) == 0 && size.ws_col && size.ws_row) {
        c->cell_width = c->width / size.ws_col;
        c->cell_height = c->height / size.ws_row;
    }
    d_input_init(&c->decoder, c->cell_width, c->cell_height);

    if (pipe(console_wake) != 0) return refuse(c, error, "cannot make a pipe%s", NULL);
    for (int i = 0; i < 2; ++i) {
        fcntl(console_wake[i], F_SETFL, O_NONBLOCK);
        fcntl(console_wake[i], F_SETFD, FD_CLOEXEC);
    }
    /* Without these a VT switch would leave us drawing over the console the
       reader switched to, so unlike a crash handler they are not optional. */
    struct sigaction release = {0}, acquire = {0};
    release.sa_handler = on_release;
    acquire.sa_handler = on_acquire;
    sigemptyset(&release.sa_mask);
    sigemptyset(&acquire.sa_mask);
    if (sigaction(SIGUSR1, &release, NULL) != 0 || sigaction(SIGUSR2, &acquire, NULL) != 0)
        return refuse(c, error, "cannot handle VT switching signals%s", NULL);

    if (tcgetattr(console_tty, &console_saved_termios) != 0 ||
        ioctl(console_tty, VT_GETMODE, &console_saved_vt) != 0)
        return refuse(c, error, "cannot read the console's mode%s", NULL);
    console_saved_kd = kd_mode;
    struct termios raw = console_saved_termios;
    raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    console_active = 1;
    struct vt_mode mode = {.mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2};
    if (tcsetattr(console_tty, TCSAFLUSH, &raw) != 0 || ioctl(console_tty, KDSETMODE, KD_GRAPHICS) != 0 ||
        ioctl(console_tty, VT_SETMODE, &mode) != 0)
        return refuse(c, error, "cannot take the console: %s", strerror(errno));

    open_mice(c);
    c->pointer_x = c->width / 2;
    c->pointer_y = c->height / 2;
    return &c->base;
}
