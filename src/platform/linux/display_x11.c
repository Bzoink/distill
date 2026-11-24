/* The window sink: a plain X11 window, for developing the framebuffer shell
   where a gate can screenshot it (FRAMEBUFFER_PLAN.md, "The window sink").

   Xlib and MIT-SHM, no toolkit. It exercises the painter, the shell, the
   chrome, forms, focus and navigation; it does not exercise fbdev, VT
   switching, console restore, tty decoding or evdev, which only a real
   console shows. Built only into distill-fb-x11, so libX11 can never leak
   into distill-fb.

   The image the window shows is wider than the window: its rows are padded
   past width * 4, so a blit that ignores the stride shows here too, not only
   on a device that happens to have padding. */

#define _POSIX_C_SOURCE 200809L

#include "app_icon.h"
#include "cutil.h"
#include "display_sink.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>
#include <locale.h>
#include <png.h>
#include <sys/ipc.h>
#include <sys/shm.h>

enum { QUEUE_MAX = 64, PADDING = 16 };

typedef struct {
    DDisplay base;
    Display *x;
    Window window;
    GC gc;
    Visual *visual;
    int depth;
    XImage *image;
    XShmSegmentInfo shm;
    bool use_shm, shm_attached;
    int width, height;
    Atom delete_window;
    XIM im;
    XIC ic;
    DEvent queue[QUEUE_MAX];
    size_t head, count;
    char *title;
} X11Display;

static void set_window_icon(X11Display *d)
{
    png_image image = {.version = PNG_IMAGE_VERSION};
    if (png_image_begin_read_from_memory(&image, distill_icon_48_png, sizeof(distill_icon_48_png))) {
        image.format = PNG_FORMAT_RGBA;
        unsigned char *rgba = d_xcalloc(PNG_IMAGE_SIZE(image), 1);
        if (png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
            size_t count = (size_t)image.width * image.height;
            /* Xlib expects a long for each 32-bit value. Pixels are straight ARGB. */
            unsigned long *icon = d_xcalloc(count + 2, sizeof(*icon));
            icon[0] = image.width;
            icon[1] = image.height;
            for (size_t i = 0; i < count; ++i) {
                const unsigned char *pixel = rgba + i * 4;
                icon[i + 2] = (unsigned long)pixel[3] << 24 | (unsigned long)pixel[0] << 16 |
                              (unsigned long)pixel[1] << 8 | pixel[2];
            }
            Atom name = XInternAtom(d->x, "_NET_WM_ICON", False);
            XChangeProperty(d->x, d->window, name, XA_CARDINAL, 32, PropModeReplace,
                            (const unsigned char *)icon, (int)(count + 2));
            free(icon);
        }
        free(rgba);
    }
    png_image_free(&image);
}

static void enqueue(X11Display *d, DEvent event)
{
    if (event.kind == D_EVENT_POINTER_MOVE && d->count) {
        /* Motion arrives faster than it is drawn: keep only the latest. */
        DEvent *last = &d->queue[(d->head + d->count - 1) % QUEUE_MAX];
        if (last->kind == D_EVENT_POINTER_MOVE) { *last = event; return; }
    }
    if (d->count == QUEUE_MAX) return;
    d->queue[(d->head + d->count++) % QUEUE_MAX] = event;
}

static void free_image(X11Display *d)
{
    if (!d->image) return;
    if (d->use_shm) {
        if (d->shm_attached) XShmDetach(d->x, &d->shm);
        XDestroyImage(d->image);
        shmdt(d->shm.shmaddr);
        d->shm_attached = false;
    } else {
        XDestroyImage(d->image);   /* frees its data */
    }
    d->image = NULL;
}

static bool make_image(X11Display *d, int width, int height)
{
    free_image(d);
    int padded = width + PADDING;
    if (d->use_shm) {
        d->image = XShmCreateImage(d->x, d->visual, (unsigned)d->depth, ZPixmap, NULL, &d->shm,
                                   (unsigned)padded, (unsigned)height);
        if (d->image) {
            d->shm.shmid = shmget(IPC_PRIVATE, (size_t)d->image->bytes_per_line * (size_t)height,
                                  IPC_CREAT | 0600);
            d->shm.shmaddr = d->image->data = d->shm.shmid >= 0 ? shmat(d->shm.shmid, NULL, 0) : (void *)-1;
            if (d->shm.shmaddr != (void *)-1) {
                d->shm.readOnly = False;
                d->shm_attached = XShmAttach(d->x, &d->shm);
                XSync(d->x, False);
                shmctl(d->shm.shmid, IPC_RMID, NULL);
                if (d->shm_attached) {
                    d->width = width;
                    d->height = height;
                    return true;
                }
                shmdt(d->shm.shmaddr);
            }
            XDestroyImage(d->image);
            d->image = NULL;
        }
        d->use_shm = false;
    }
    int stride = padded * 4;
    char *data = calloc((size_t)stride * (size_t)height, 1);
    if (!data) return false;
    d->image = XCreateImage(d->x, d->visual, (unsigned)d->depth, ZPixmap, 0, data,
                            (unsigned)width, (unsigned)height, 32, stride);
    if (!d->image) { free(data); return false; }
    d->width = width;
    d->height = height;
    return true;
}

static void put(X11Display *d, int x, int y, int width, int height)
{
    if (d->use_shm) XShmPutImage(d->x, d->window, d->gc, d->image, x, y, x, y, (unsigned)width, (unsigned)height, False);
    else XPutImage(d->x, d->window, d->gc, d->image, x, y, x, y, (unsigned)width, (unsigned)height);
}

static DDisplayMode x11_mode(const DDisplay *display)
{
    const X11Display *d = (const X11Display *)display;
    return (DDisplayMode){d->width, d->height, d->image ? d->image->bytes_per_line : d->width * 4, 8, 16};
}

static size_t x11_handles(DDisplay *display, int *handles, size_t capacity)
{
    X11Display *d = (X11Display *)display;
    if (!capacity) return 0;
    handles[0] = ConnectionNumber(d->x);
    return 1;
}

static int x11_wait_ms(DDisplay *display)
{
    X11Display *d = (X11Display *)display;
    return d->count || XPending(d->x) ? 0 : -1;
}

static unsigned modifiers(unsigned state)
{
    return (state & ShiftMask ? D_MOD_SHIFT : 0) | (state & ControlMask ? D_MOD_CTRL : 0) |
           (state & Mod1Mask ? D_MOD_ALT : 0);
}

static DKey key_for(KeySym sym)
{
    switch (sym) {
    case XK_Return: case XK_KP_Enter: return D_KEY_ENTER;
    case XK_Tab: case XK_ISO_Left_Tab: return D_KEY_TAB;
    case XK_Escape: return D_KEY_ESCAPE;
    case XK_BackSpace: return D_KEY_BACKSPACE;
    case XK_Delete: case XK_KP_Delete: return D_KEY_DELETE;
    case XK_Insert: return D_KEY_INSERT;
    case XK_Up: case XK_KP_Up: return D_KEY_UP;
    case XK_Down: case XK_KP_Down: return D_KEY_DOWN;
    case XK_Left: case XK_KP_Left: return D_KEY_LEFT;
    case XK_Right: case XK_KP_Right: return D_KEY_RIGHT;
    case XK_Home: case XK_KP_Home: return D_KEY_HOME;
    case XK_End: case XK_KP_End: return D_KEY_END;
    case XK_Prior: case XK_KP_Prior: return D_KEY_PAGE_UP;
    case XK_Next: case XK_KP_Next: return D_KEY_PAGE_DOWN;
    default:
        if (sym >= XK_F1 && sym <= XK_F12) return (DKey)(D_KEY_F1 + (sym - XK_F1));
        return D_KEY_NONE;
    }
}

static uint32_t first_codepoint(const unsigned char *s, int length)
{
    if (length <= 0) return 0;
    if (s[0] < 0x80) return s[0];
    int need = (s[0] & 0xe0) == 0xc0 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!need || length < need) return 0;
    uint32_t c = s[0] & (need == 2 ? 0x1f : need == 3 ? 0x0f : 0x07);
    for (int i = 1; i < need; ++i) c = c << 6 | (s[i] & 0x3f);
    return c;
}

static void translate(X11Display *d, XEvent *event)
{
    switch (event->type) {
    case KeyPress: {
        char text[32];
        KeySym sym = NoSymbol;
        Status status = 0;
        int length = d->ic ? Xutf8LookupString(d->ic, &event->xkey, text, sizeof(text), &sym, &status)
                           : XLookupString(&event->xkey, text, sizeof(text), &sym, NULL);
        unsigned mods = modifiers(event->xkey.state);
        DKey key = key_for(sym);
        if (key != D_KEY_NONE) {
            if (sym == XK_ISO_Left_Tab) mods |= D_MOD_SHIFT;
            enqueue(d, (DEvent){.kind = D_EVENT_KEY, .key = key, .mods = mods});
        } else if ((mods & D_MOD_CTRL) && ((sym >= XK_a && sym <= XK_z) || (sym >= XK_A && sym <= XK_Z))) {
            uint32_t c = (uint32_t)(sym >= XK_a ? sym : sym - XK_A + XK_a);
            enqueue(d, (DEvent){.kind = D_EVENT_KEY, .key = D_KEY_CHAR, .codepoint = c, .mods = mods});
        } else {
            uint32_t c = d->ic ? first_codepoint((const unsigned char *)text, length) : 0;
            /* Without an input method the text is not UTF-8; the keysym still
               names the character: Latin-1 directly, the rest as Unicode. */
            if (!c && sym >= 0x20 && sym <= 0xff) c = (uint32_t)sym;
            else if (!c && (sym & 0xff000000ul) == 0x01000000ul) c = (uint32_t)(sym & 0xffffff);
            if (c >= 0x20 && c != 0x7f)
                enqueue(d, (DEvent){.kind = D_EVENT_KEY, .key = D_KEY_CHAR, .codepoint = c,
                                    .mods = mods & ~(unsigned)D_MOD_SHIFT});
        }
        break;
    }
    case ButtonPress:
    case ButtonRelease: {
        unsigned b = event->xbutton.button;
        int x = event->xbutton.x, y = event->xbutton.y;
        if (b == 4 || b == 5) {
            if (event->type == ButtonPress)
                enqueue(d, (DEvent){.kind = D_EVENT_SCROLL, .x = x, .y = y, .lines = b == 4 ? -1 : 1});
            break;
        }
        if (b > 3) break;
        int button = b == 1 ? D_BUTTON_LEFT : b == 2 ? D_BUTTON_MIDDLE : D_BUTTON_RIGHT;
        enqueue(d, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .x = x, .y = y, .button = button,
                            .pressed = event->type == ButtonPress,
                            .mods = modifiers(event->xbutton.state)});
        break;
    }
    case MotionNotify:
        enqueue(d, (DEvent){.kind = D_EVENT_POINTER_MOVE, .x = event->xmotion.x, .y = event->xmotion.y});
        break;
    case ConfigureNotify:
        if (event->xconfigure.width != d->width || event->xconfigure.height != d->height) {
            if (make_image(d, event->xconfigure.width, event->xconfigure.height))
                enqueue(d, (DEvent){.kind = D_EVENT_RESIZE});
        }
        break;
    case Expose:
        if (event->xexpose.count == 0 && d->image) put(d, 0, 0, d->width, d->height);
        break;
    case ClientMessage:
        if ((Atom)event->xclient.data.l[0] == d->delete_window)
            enqueue(d, (DEvent){.kind = D_EVENT_QUIT});
        break;
    default:
        break;
    }
}

static bool x11_next_event(DDisplay *display, DEvent *event)
{
    X11Display *d = (X11Display *)display;
    while (!d->count && XPending(d->x)) {
        XEvent x;
        XNextEvent(d->x, &x);
        if (XFilterEvent(&x, None)) continue;
        translate(d, &x);
    }
    if (!d->count) return false;
    *event = d->queue[d->head];
    d->head = (d->head + 1) % QUEUE_MAX;
    --d->count;
    return true;
}

static void x11_present(DDisplay *display, const uint32_t *pixels, int stride,
                        const DRenderRect *damage, size_t damage_count)
{
    X11Display *d = (X11Display *)display;
    if (!d->image) return;
    d_display_copy((uint32_t *)d->image->data, d->image->bytes_per_line, d->width, d->height,
                   pixels, stride, damage, damage_count);
    for (size_t i = 0; i < damage_count; ++i) {
        DRenderRect r = damage[i];
        if (r.x < 0) { r.width += r.x; r.x = 0; }
        if (r.y < 0) { r.height += r.y; r.y = 0; }
        if (r.x + r.width > d->width) r.width = d->width - r.x;
        if (r.y + r.height > d->height) r.height = d->height - r.y;
        if (r.width > 0 && r.height > 0) put(d, r.x, r.y, r.width, r.height);
    }
    XFlush(d->x);
}

static void x11_report(DDisplay *display, bool busy, const char *state, const char *title)
{
    (void)busy; (void)state;
    X11Display *d = (X11Display *)display;
    if (!title || (d->title && !strcmp(d->title, title))) return;
    free(d->title);
    d->title = d_strdup(title);
    Xutf8SetWMProperties(d->x, d->window, title, title, NULL, 0, NULL, NULL, NULL);
    XFlush(d->x);
}

static void x11_close(DDisplay *display)
{
    X11Display *d = (X11Display *)display;
    free_image(d);
    if (d->ic) XDestroyIC(d->ic);
    if (d->im) XCloseIM(d->im);
    if (d->gc) XFreeGC(d->x, d->gc);
    if (d->window) XDestroyWindow(d->x, d->window);
    XCloseDisplay(d->x);
    free(d->title);
    free(d);
}

static const DDisplayOps x11_ops = {
    x11_mode, x11_handles, x11_wait_ms, x11_next_event, x11_present, x11_report, x11_close,
};

DDisplay *d_display_x11_open(const char *spec, char **error)
{
    *error = NULL;
    int width = 1920, height = 1080;
    if (spec[3] == ':' && spec[4]) d_display_parse_size(spec + 4, &width, &height);
    Display *x = XOpenDisplay(NULL);
    if (!x) { *error = d_strdup("cannot open the X display (is DISPLAY set?)"); return NULL; }
    int screen = DefaultScreen(x);
    XVisualInfo info;
    if (!XMatchVisualInfo(x, screen, 24, TrueColor, &info) || info.red_mask != 0xff0000 ||
        info.green_mask != 0xff00 || info.blue_mask != 0xff) {
        XCloseDisplay(x);
        *error = d_strdup("this X display has no 24-bit XRGB visual");
        return NULL;
    }
    X11Display *d = d_xcalloc(1, sizeof(*d));
    d->base.ops = &x11_ops;
    d->x = x;
    d->visual = info.visual;
    d->depth = info.depth;
    d->use_shm = XShmQueryExtension(x) && !getenv("DISTILL_X11_NO_SHM");
    XSetWindowAttributes attributes = {0};
    attributes.background_pixel = 0;
    attributes.border_pixel = 0;
    attributes.colormap = XCreateColormap(x, RootWindow(x, screen), info.visual, AllocNone);
    attributes.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            StructureNotifyMask | ExposureMask;
    d->window = XCreateWindow(x, RootWindow(x, screen), 0, 0, (unsigned)width, (unsigned)height, 0,
                              info.depth, InputOutput, info.visual,
                              CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attributes);
    d->gc = XCreateGC(x, d->window, 0, NULL);
    d->delete_window = XInternAtom(x, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x, d->window, &d->delete_window, 1);
    Xutf8SetWMProperties(x, d->window, "Distill", "Distill", NULL, 0, NULL, NULL, NULL);
    set_window_icon(d);
    /* Only the character type, which an input method needs to hand back
       UTF-8; numbers are never read through the locale (cutil.h d_strtod). */
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    d->im = XOpenIM(x, NULL, NULL, NULL);
    if (d->im)
        d->ic = XCreateIC(d->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, d->window, XNFocusWindow, d->window, NULL);
    if (!make_image(d, width, height)) {
        x11_close(&d->base);
        *error = d_strdup("cannot make the window's image");
        return NULL;
    }
    XMapWindow(x, d->window);
    XFlush(x);
    return &d->base;
}
