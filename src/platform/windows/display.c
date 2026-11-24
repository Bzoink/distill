#include "display_sink.h"
#include "win.h"

#include <windowsx.h>

enum { EVENT_CAPACITY = 128, DEFAULT_WIDTH = 1050, DEFAULT_HEIGHT = 760 };

typedef struct {
    DDisplay base;
    HWND window;
    int width, height;
    uint32_t *pixels;
    DEvent events[EVENT_CAPACITY];
    size_t head, count;
    unsigned high_surrogate;
    int wheel;
} WinDisplay;

static void enqueue(WinDisplay *display, DEvent event)
{
    if (display->count && event.kind == D_EVENT_POINTER_MOVE) {
        size_t previous = (display->head + display->count - 1) % EVENT_CAPACITY;
        if (display->events[previous].kind == D_EVENT_POINTER_MOVE) {
            display->events[previous] = event;
            return;
        }
    }
    if (display->count == EVENT_CAPACITY) {
        if (event.kind != D_EVENT_QUIT) return;
        display->head = (display->head + 1) % EVENT_CAPACITY;
        --display->count;
    }
    display->events[(display->head + display->count++) % EVENT_CAPACITY] = event;
}

static void resize(WinDisplay *display, int width, int height)
{
    if (width <= 0 || height <= 0 || (width == display->width && height == display->height))
        return;
    uint32_t *pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
    if (!pixels) return;
    free(display->pixels);
    display->pixels = pixels;
    display->width = width;
    display->height = height;
    enqueue(display, (DEvent){.kind = D_EVENT_RESIZE});
}

static unsigned modifiers(void)
{
    return (GetKeyState(VK_SHIFT) < 0 ? D_MOD_SHIFT : 0) |
           (GetKeyState(VK_CONTROL) < 0 ? D_MOD_CTRL : 0) |
           (GetKeyState(VK_MENU) < 0 ? D_MOD_ALT : 0);
}

static DKey translate_key(WPARAM key)
{
    if (key >= VK_F1 && key <= VK_F12) return (DKey)(D_KEY_F1 + key - VK_F1);
    switch (key) {
    case VK_RETURN: return D_KEY_ENTER;
    case VK_TAB: return D_KEY_TAB;
    case VK_ESCAPE: return D_KEY_ESCAPE;
    case VK_BACK: return D_KEY_BACKSPACE;
    case VK_DELETE: return D_KEY_DELETE;
    case VK_INSERT: return D_KEY_INSERT;
    case VK_UP: return D_KEY_UP;
    case VK_DOWN: return D_KEY_DOWN;
    case VK_LEFT: return D_KEY_LEFT;
    case VK_RIGHT: return D_KEY_RIGHT;
    case VK_HOME: return D_KEY_HOME;
    case VK_END: return D_KEY_END;
    case VK_PRIOR: return D_KEY_PAGE_UP;
    case VK_NEXT: return D_KEY_PAGE_DOWN;
    default: return D_KEY_NONE;
    }
}

static void character(WinDisplay *display, unsigned value)
{
    if (value >= 0xd800 && value <= 0xdbff) {
        display->high_surrogate = value;
        return;
    }
    unsigned high = display->high_surrogate;
    display->high_surrogate = 0;
    if (value >= 0xdc00 && value <= 0xdfff) {
        if (!high) return;
        value = 0x10000 + ((high - 0xd800) << 10) + value - 0xdc00;
    }
    if (value >= 0x20 && value != 0x7f)
        enqueue(display, (DEvent){.kind = D_EVENT_KEY, .key = D_KEY_CHAR,
                                  .mods = modifiers(), .codepoint = value});
}

static void paint(WinDisplay *display, HWND window)
{
    PAINTSTRUCT update;
    HDC dc = BeginPaint(window, &update);
    if (display->pixels) {
        BITMAPINFO bitmap = {0};
        bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
        bitmap.bmiHeader.biWidth = display->width;
        bitmap.bmiHeader.biHeight = -display->height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, (DWORD)display->width, (DWORD)display->height, 0, 0, 0,
                          (UINT)display->height, display->pixels, &bitmap, DIB_RGB_COLORS);
    }
    EndPaint(window, &update);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    WinDisplay *display = (WinDisplay *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    if (!display) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CLOSE:
        enqueue(display, (DEvent){.kind = D_EVENT_QUIT});
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) resize(display, LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_PAINT:
        paint(display, window);
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned mods = modifiers();
        if (message == WM_SYSKEYDOWN && wparam == VK_F4 && (mods & D_MOD_ALT)) break;
        DKey key = translate_key(wparam);
        if (key != D_KEY_NONE) {
            enqueue(display, (DEvent){.kind = D_EVENT_KEY, .key = key, .mods = mods});
            return 0;
        }
        if ((mods & (D_MOD_CTRL | D_MOD_ALT)) == D_MOD_CTRL && wparam >= 'A' && wparam <= 'Z') {
            enqueue(display, (DEvent){.kind = D_EVENT_KEY, .key = D_KEY_CHAR, .mods = mods,
                                      .codepoint = (uint32_t)wparam + ('a' - 'A')});
            return 0;
        }
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        character(display, (unsigned)wparam);
        return 0;
    case WM_MOUSEMOVE:
        enqueue(display, (DEvent){.kind = D_EVENT_POINTER_MOVE,
                                  .x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)});
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int button = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ? D_BUTTON_LEFT
                   : message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ? D_BUTTON_MIDDLE
                   : D_BUTTON_RIGHT;
        bool down = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN;
        enqueue(display, (DEvent){.kind = D_EVENT_POINTER_BUTTON, .mods = modifiers(),
                                  .x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam),
                                  .button = button, .pressed = down});
        return 0;
    }
    case WM_MOUSEWHEEL: {
        display->wheel += GET_WHEEL_DELTA_WPARAM(wparam);
        int steps = display->wheel / WHEEL_DELTA;
        display->wheel %= WHEEL_DELTA;
        if (steps) enqueue(display, (DEvent){.kind = D_EVENT_SCROLL, .mods = modifiers(),
                                             .lines = -3 * steps});
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static DDisplayMode mode(const DDisplay *base)
{
    const WinDisplay *display = (const WinDisplay *)base;
    return (DDisplayMode){.width = display->width, .height = display->height,
                          .stride = display->width * 4, .cell_width = 1, .cell_height = 1};
}

static size_t handles(DDisplay *base, int *list, size_t capacity)
{
    /* d_poll already waits on the current thread's window message queue. */
    (void)base; (void)list; (void)capacity;
    return 0;
}

static int wait_ms(DDisplay *base)
{
    return ((WinDisplay *)base)->count ? 0 : -1;
}

static bool next_event(DDisplay *base, DEvent *event)
{
    WinDisplay *display = (WinDisplay *)base;
    MSG message;
    while (!display->count && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            *event = (DEvent){.kind = D_EVENT_QUIT};
            return true;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!display->count) return false;
    *event = display->events[display->head];
    display->head = (display->head + 1) % EVENT_CAPACITY;
    --display->count;
    return true;
}

static void present(DDisplay *base, const uint32_t *pixels, int stride,
                    const DRenderRect *damage, size_t damage_count)
{
    WinDisplay *display = (WinDisplay *)base;
    if (!damage_count) return;
    d_display_copy(display->pixels, display->width * 4, display->width, display->height,
                   pixels, stride, damage, damage_count);
    InvalidateRect(display->window, NULL, FALSE);
}

static void report(DDisplay *base, bool busy, const char *state, const char *title)
{
    (void)state;
    WinDisplay *display = (WinDisplay *)base;
    const char *prefix = busy ? "Loading - " : "";
    char *caption = d_cat2(prefix, title ? title : "");
    wchar_t *wide = d_win_wide(caption);
    if (wide) {
        wchar_t old[512] = {0};
        GetWindowTextW(display->window, old, sizeof(old) / sizeof(*old));
        if (wcscmp(old, wide)) SetWindowTextW(display->window, wide);
    }
    free(wide);
    free(caption);
}

static void close_window(DDisplay *base)
{
    WinDisplay *display = (WinDisplay *)base;
    if (display->window) DestroyWindow(display->window);
    free(display->pixels);
    free(display);
}

static const DDisplayOps ops = {mode, handles, wait_ms, next_event, present, report, close_window};

DDisplay *d_display_open(const char *spec, char **error)
{
    if (error) *error = NULL;
    if (spec && d_starts(spec, "memory:")) return d_display_memory_open(spec, error);
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
    if (spec && strcmp(spec, "windows") &&
        (!d_starts(spec, "windows:") || !d_display_parse_size(spec + 8, &width, &height) ||
         width > 16384 || height > 16384)) {
        if (error) *error = d_strdup("display must be windows[:WxH] or memory:WxH");
        return NULL;
    }
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW type = {.lpfnWndProc = window_proc, .hInstance = instance,
                      .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
                      .hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)),
                      .lpszClassName = L"DistillReaderWindow"};
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        d_win_error(error, "could not register the Distill window", GetLastError());
        return NULL;
    }
    WinDisplay *display = d_xcalloc(1, sizeof(*display));
    display->base.ops = &ops;
    RECT client = {0, 0, width, height};
    AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);
    display->window = CreateWindowExW(0, type.lpszClassName, L"Distill", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, client.right - client.left,
                                      client.bottom - client.top, NULL, NULL, instance, display);
    if (!display->window) {
        d_win_error(error, "could not open the Distill window", GetLastError());
        close_window(&display->base);
        return NULL;
    }
    GetClientRect(display->window, &client);
    resize(display, client.right, client.bottom);
    if (!display->pixels) {
        if (error) *error = d_strdup("could not allocate the Distill window buffer");
        close_window(&display->base);
        return NULL;
    }
    ShowWindow(display->window, SW_SHOW);
    return &display->base;
}

void d_display_emergency_restore(void) {}
