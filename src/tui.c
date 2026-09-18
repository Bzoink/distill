#define _GNU_SOURCE

/* Distill in a terminal.

   The same process model as the desktop shell: the engine runs once per
   navigation and writes the reader page to a file; this reads it back, lays
   it out in cells (reader_cells.h) and draws a window onto it. Everything a
   front end shares with the desktop -- the form rules, what a typed address
   means, the settings file, fetching media -- comes from the same code the
   desktop uses, so the two send the same requests and read the same
   preferences.

   termbox2 owns the terminal: raw mode, the key and mouse decoding, resize,
   and putting the terminal back. Output is written here, through tb_send;
   tui_screen.h says why. */

#include "address.h"
#include "app_state.h"
#include "cutil.h"
#include "form_submit.h"
#include "image.h"
#include "media_audio.h"
#include "media_transfer.h"
#include "reader_cells.h"
#include "settings.h"
#include "term_image.h"
#include "tui_screen.h"

#include "termbox2.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef DISTILL_ENGINE_PATH
#define DISTILL_ENGINE_PATH ""
#endif
#ifndef DISTILL_SOURCE_DIR
#define DISTILL_SOURCE_DIR ""
#endif

extern char **environ;

#define SECTION_SCHEME "distill-section:"
#define MEDIA_SCHEME "distill-media:"
#define PLAY_SCHEME "distill-play:"
#define DOWNLOAD_SCHEME "distill-download:"
#define OPEN_SCHEME "distill-open:"

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    uint32_t page_bg, text, main_bg, heading, card_bg, border, link, pre_bg, muted;
    bool valid;
} Palette;

typedef enum { FOCUS_LINK, FOCUS_CONTROL } FocusKind;
typedef struct {
    FocusKind kind;
    size_t index;
    int row, column;
} Focus;

typedef struct {
    char *url;
    char *html;
    ReaderLayout *layout;
    Palette palette;
    int scroll;
    int focus;               /* index into focus, or -1 */
    Focus *focusables;
    size_t focus_count;
    char **values;           /* what each control holds now */
    bool *checked;
} Page;

typedef enum { PROMPT_NONE, PROMPT_ADDRESS, PROMPT_FIELD, PROMPT_SAVE_AS } PromptKind;
typedef struct {
    PromptKind kind;
    char label[160];
    DBuffer text;
    size_t cursor;           /* byte offset */
    bool secret;
    size_t control;
    char *url;
} Prompt;

typedef enum { NAV_NEW, NAV_RELOAD } NavKind;
typedef struct {
    pid_t pid;
    int out_fd, err_fd;
    DBuffer pending;
    DBuffer diagnostics;
    char *url;
    char *template_path;
    char *body_path;
    char *location;
    int percent;
    char phase[16];
    NavKind kind;
} Navigation;

typedef enum { GRAPHICS_NONE, GRAPHICS_KITTY, GRAPHICS_SIXEL } Graphics;

typedef struct {
    bool active;
    char **urls;
    size_t count;
    size_t index;
    DImage image;
    bool loaded;
    char *file;
    char *failure;
    bool drawn;
} Viewer;

typedef struct {
    size_t control;
    char **values;
    char **labels;
    size_t count;
    size_t selected;
} Popup;

typedef struct {
    int tty;
    struct termios saved;
    bool terminal_ready;
    int width, height;
    DScreen screen;
    Graphics graphics;
    int cell_width, cell_height;

    Page page;
    bool has_page;
    DAppHistory history;
    char *theme;
    int zoom;
    char *engine;
    char *session_file;

    Navigation nav;
    bool loading;
    Prompt prompt;
    bool popup_open;
    Popup popup;
    bool help;
    int help_scroll, help_most;
    Viewer viewer;

    DMediaTransfer *transfer;
    enum { TRANSFER_AUDIO, TRANSFER_DOWNLOAD, TRANSFER_IMAGE } transfer_kind;
    char *audio_file;
    char *audio_title;
    int transport_row, bar_start, bar_end;
    bool seeking;

    pid_t externals[16];
    size_t external_count;

    char *status;
    bool quit;
} Tui;

static Tui tui;

static void set_status(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    free(tui.status);
    tui.status = d_strdup(buffer);
}

/* ---- the terminal, and putting it back ---------------------------------- */

/* What a crash must undo, written with nothing but write(2) and tcsetattr:
   both are safe in a signal handler, and a terminal left in raw mode on the
   alternate screen with mouse reporting on is the failure a reader notices
   before any other. */
static const char terminal_reset[] =
    "\033[0m\033[?25h\033[?7h\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l\033[?1049l";

/* Everything this run writes -- the session's cookies, reader pages, request
   bodies, fetched media -- goes in one private directory. A clean exit
   removes it; a crash cannot walk a directory safely, but it can unlink the
   one file that matters most, the live cookies, by a path fixed in advance. */
static char run_directory[PATH_MAX];
static char session_path[PATH_MAX];

static void emergency_restore(int signal_number)
{
    if (session_path[0]) unlink(session_path);
    if (tui.terminal_ready) {
        ssize_t ignored = write(tui.tty, terminal_reset, sizeof(terminal_reset) - 1);
        (void)ignored;
        tcsetattr(tui.tty, TCSAFLUSH, &tui.saved);
    }
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void install_emergency_restore(void)
{
    int signals[] = {SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        struct sigaction action = {0};
        action.sa_handler = emergency_restore;
        sigemptyset(&action.sa_mask);
        sigaction(signals[i], &action, NULL);
    }
    signal(SIGPIPE, SIG_IGN);
}

static void cell_pixels(void)
{
    struct winsize size = {0};
    tui.cell_width = 8;
    tui.cell_height = 16;
    if (ioctl(tui.tty, TIOCGWINSZ, &size) == 0 && size.ws_xpixel && size.ws_ypixel &&
        size.ws_col && size.ws_row) {
        tui.cell_width = size.ws_xpixel / size.ws_col;
        tui.cell_height = size.ws_ypixel / size.ws_row;
    }
}

/* Which image protocol this terminal speaks, asked of the terminal itself:
   a kitty graphics query answers OK only where kitty graphics exist, and the
   primary device attributes list 4 where sixel does. The attributes reply is
   sent by every terminal, so it also marks the end of the answer. */
static Graphics probe_graphics(void)
{
    const char *forced = getenv("DISTILL_TUI_GRAPHICS");
    if (forced && *forced) {
        if (!strcmp(forced, "kitty")) return GRAPHICS_KITTY;
        if (!strcmp(forced, "sixel")) return GRAPHICS_SIXEL;
        return GRAPHICS_NONE;
    }
    const char query[] = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\\033[c";
    if (write(tui.tty, query, sizeof(query) - 1) < 0) return GRAPHICS_NONE;
    char reply[512];
    size_t used = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > 400 || used >= sizeof(reply) - 1) break;
        struct pollfd fd = {tui.tty, POLLIN, 0};
        if (poll(&fd, 1, (int)(400 - elapsed)) <= 0) break;
        ssize_t got = read(tui.tty, reply + used, sizeof(reply) - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        reply[used] = '\0';
        char *attributes = strstr(reply, "\033[?");
        if (attributes && strchr(attributes, 'c')) break;
    }
    reply[used] = '\0';
    if (strstr(reply, "_Gi=31;OK")) return GRAPHICS_KITTY;
    char *attributes = strstr(reply, "\033[?");
    if (attributes) {
        for (char *p = attributes + 3; *p && *p != 'c';) {
            long value = strtol(p, &p, 10);
            if (value == 4) return GRAPHICS_SIXEL;
            if (*p == ';') ++p;
            else break;
        }
    }
    return GRAPHICS_NONE;
}

static DScreenColors detect_colors(void)
{
    const char *forced = getenv("DISTILL_TUI_COLORS");
    if (forced && *forced) {
        if (!strcmp(forced, "true")) return D_SCREEN_COLORS_TRUE;
        if (!strcmp(forced, "256")) return D_SCREEN_COLORS_256;
        return D_SCREEN_COLORS_16;
    }
    const char *colorterm = getenv("COLORTERM");
    if (colorterm && (!strcmp(colorterm, "truecolor") || !strcmp(colorterm, "24bit")))
        return D_SCREEN_COLORS_TRUE;
    const char *term = getenv("TERM");
    if (term && strstr(term, "256color")) return D_SCREEN_COLORS_256;
    return D_SCREEN_COLORS_16;
}

static bool terminal_open(void)
{
    tui.tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tui.tty < 0) {
        fprintf(stderr, "distill-tui: no terminal to run in (%s)\n", strerror(errno));
        return false;
    }
    if (tcgetattr(tui.tty, &tui.saved) != 0) {
        fprintf(stderr, "distill-tui: not a terminal\n");
        return false;
    }
    install_emergency_restore();
    if (tb_init_fd(tui.tty) != TB_OK) {
        fprintf(stderr, "distill-tui: unable to set up the terminal\n");
        return false;
    }
    tui.terminal_ready = true;
    /* ESC mode, with Alt rebuilt in the event loop: in termbox's ALT mode a
       lone Escape never arrives as a key, and Escape is how every mode here
       is left. */
    tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
    tui.graphics = probe_graphics();
    /* No autowrap: writing the last cell of the last row must not scroll. */
    tb_send("\033[?7l", 5);
    tb_hide_cursor();
    tui.width = tb_width();
    tui.height = tb_height();
    cell_pixels();
    d_screen_init(&tui.screen, tui.width, tui.height, detect_colors());
    return true;
}

static void terminal_close(void)
{
    if (!tui.terminal_ready) return;
    if (tui.graphics == GRAPHICS_KITTY) {
        const char *clear = d_term_image_kitty_clear();
        tb_send(clear, strlen(clear));
    }
    tb_send("\033[?7h\033[0m", 10);
    tb_present();
    tb_shutdown();
    tui.terminal_ready = false;
    tcsetattr(tui.tty, TCSAFLUSH, &tui.saved);
    close(tui.tty);
    d_screen_free(&tui.screen);
}

/* ---- files -------------------------------------------------------------- */

static char *private_temp(const char *prefix)
{
    const char *directory = getenv("TMPDIR");
    char *path = d_cat3(directory && *directory ? directory : "/tmp", "/", prefix);
    char *full = d_cat2(path, "-XXXXXX");
    free(path);
    int fd = mkstemp(full);
    if (fd < 0) { free(full); return NULL; }
    fchmod(fd, S_IRUSR | S_IWUSR);
    close(fd);
    return full;
}

static char *engine_path(void)
{
    const char *configured = getenv("DISTILL_ENGINE");
    if (configured && *configured && access(configured, X_OK) == 0) return d_strdup(configured);
    if (*DISTILL_ENGINE_PATH && access(DISTILL_ENGINE_PATH, X_OK) == 0) return d_strdup(DISTILL_ENGINE_PATH);
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = '\0';
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            char *candidate = d_cat2(executable, "/distill");
            if (access(candidate, X_OK) == 0) return candidate;
            free(candidate);
        }
    }
    return d_strdup("distill");
}

static char *welcome_page(void)
{
    const char *configured = getenv("DISTILL_WELCOME_PAGE");
    if (configured && *configured && access(configured, R_OK) == 0) return d_cat2("file://", configured);
    if (*DISTILL_SOURCE_DIR) {
        char *source = d_cat2(DISTILL_SOURCE_DIR, "/share/welcome.html");
        if (access(source, R_OK) == 0) { char *url = d_cat2("file://", source); free(source); return url; }
        free(source);
    }
#ifdef DISTILL_INSTALL_WELCOME_PAGE
    if (access(DISTILL_INSTALL_WELCOME_PAGE, R_OK) == 0) return d_cat2("file://", DISTILL_INSTALL_WELCOME_PAGE);
#endif
    return NULL;
}

/* ---- settings ------------------------------------------------------------ */

static bool valid_theme(const char *theme)
{
    return theme && (!strcmp(theme, "warm") || !strcmp(theme, "night") || !strcmp(theme, "paper"));
}

static void load_settings(char **start)
{
    DSettings *settings = d_settings_load(NULL);
    const char *theme = d_settings_get(settings, "browser", "theme");
    tui.theme = d_strdup(valid_theme(theme) ? theme : "warm");
    int zoom = d_settings_get_int(settings, "browser", "zoom", 100);
    tui.zoom = zoom >= 50 && zoom <= 300 ? zoom : 100;
    const char *value = d_settings_get(settings, "browser", "start-page");
    if (!value || !*value || !strcmp(value, "welcome")) *start = welcome_page();
    else if (!strcmp(value, "blank")) *start = NULL;
    else *start = d_strdup(value);
    d_settings_free(settings);
}

static void save_theme(void)
{
    DSettings *settings = d_settings_load(NULL);
    d_settings_set(settings, "browser", "theme", tui.theme);
    if (!d_settings_save(settings)) set_status("Could not save the settings file");
    d_settings_free(settings);
}

/* ---- the page ------------------------------------------------------------ */

/* The reader's colours, read from the page the engine wrote rather than kept
   in a second table here: the theme is the page's, and a copy would drift. */
static uint32_t css_color(const char *html, const char *name, uint32_t fallback)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "--%s:#", name);
    const char *at = strstr(html, needle);
    if (!at) return fallback;
    at += strlen(needle);
    char hex[7] = {0};
    for (int i = 0; i < 6; ++i) {
        if (!at[i]) return fallback;
        hex[i] = at[i];
    }
    return (uint32_t)strtoul(hex, NULL, 16);
}

static Palette palette_of(const char *html)
{
    Palette p = {
        .page_bg = css_color(html, "page-background", 0xf4f1ea),
        .text = css_color(html, "page-text", 0x282622),
        .main_bg = css_color(html, "main-background", 0xfffdf8),
        .heading = css_color(html, "heading-text", 0x171614),
        .card_bg = css_color(html, "card-background", 0xf5f2ec),
        .border = css_color(html, "border", 0xddd5c9),
        .link = css_color(html, "link", 0x165d7a),
        .pre_bg = css_color(html, "pre-background", 0xf0eee8),
        .muted = css_color(html, "muted", 0x827a6e),
    };
    p.valid = strstr(html, "--page-background:#") != NULL;
    return p;
}

static int view_top(void) { return 1; }

static int view_height(void)
{
    int reserved = 2;
    if (tui.transport_row >= 0) ++reserved;
    int height = tui.height - reserved;
    return height < 1 ? 1 : height;
}

static void page_free_state(Page *page)
{
    if (page->layout) {
        for (size_t i = 0; i < page->layout->control_count; ++i) free(page->values[i]);
        reader_layout_free(page->layout);
    }
    free(page->values);
    free(page->checked);
    free(page->focusables);
    free(page->html);
    free(page->url);
    *page = (Page){.focus = -1};
}

static int compare_focus(const void *a, const void *b)
{
    const Focus *x = a, *y = b;
    if (x->row != y->row) return x->row - y->row;
    return x->column - y->column;
}

static void build_focus(Page *page)
{
    free(page->focusables);
    page->focusables = NULL;
    page->focus_count = 0;
    ReaderLayout *layout = page->layout;
    size_t capacity = layout->link_count + layout->control_count;
    if (!capacity) return;
    page->focusables = d_xmalloc(capacity * sizeof(*page->focusables));
    for (size_t i = 0; i < layout->link_count; ++i)
        if (layout->links[i].row >= 0)
            page->focusables[page->focus_count++] = (Focus){FOCUS_LINK, i, layout->links[i].row,
                                                            layout->links[i].column};
    for (size_t i = 0; i < layout->control_count; ++i) {
        ReaderLayoutControl *control = &layout->controls[i];
        if (control->row >= 0 && control->width > 0 && !control->disabled)
            page->focusables[page->focus_count++] = (Focus){FOCUS_CONTROL, i, control->row,
                                                            control->column};
    }
    qsort(page->focusables, page->focus_count, sizeof(*page->focusables), compare_focus);
}

/* Lay the page out at the terminal's width. Control state survives a relayout
   -- a resize must not throw away what the reader typed. */
static void relayout(Page *page)
{
    ReaderLayout *old = page->layout;
    int old_rows = old ? old->rows : 0;
    int old_scroll = page->scroll;
    ReaderLayout *layout = reader_layout(page->html, &(ReaderLayoutOptions){.columns = tui.width});
    if (!layout) return;
    if (!old) {
        page->values = d_xcalloc(layout->control_count + 1, sizeof(*page->values));
        page->checked = d_xcalloc(layout->control_count + 1, sizeof(*page->checked));
        for (size_t i = 0; i < layout->control_count; ++i) {
            page->values[i] = d_strdup(layout->controls[i].value);
            page->checked[i] = layout->controls[i].checked;
        }
    } else {
        reader_layout_free(old);
    }
    page->layout = layout;
    for (size_t i = 0; i < layout->control_count; ++i)
        reader_layout_paint_control(layout, i, page->values[i], page->checked[i]);
    /* Keep the reader where they were, as a proportion of the page. */
    if (old_rows > 0) page->scroll = (int)((long)old_scroll * layout->rows / old_rows);
    /* Focus is on a link or a control, not on a position: find it again. */
    bool had_focus = page->focus >= 0 && (size_t)page->focus < page->focus_count;
    Focus kept = had_focus ? page->focusables[page->focus] : (Focus){0};
    build_focus(page);
    page->focus = -1;
    for (size_t i = 0; had_focus && i < page->focus_count; ++i)
        if (page->focusables[i].kind == kept.kind && page->focusables[i].index == kept.index)
            page->focus = (int)i;
}

static void clamp_scroll(void)
{
    if (!tui.has_page || !tui.page.layout) return;
    int most = tui.page.layout->rows - view_height();
    if (most < 0) most = 0;
    if (tui.page.scroll > most) tui.page.scroll = most;
    if (tui.page.scroll < 0) tui.page.scroll = 0;
}

static void scroll_by(int rows)
{
    tui.page.scroll += rows;
    clamp_scroll();
}

static void reveal_row(int row)
{
    int height = view_height();
    if (row < tui.page.scroll || row >= tui.page.scroll + height)
        tui.page.scroll = row - height / 3;
    clamp_scroll();
}

static bool show_page_file(const char *url, const char *template_path)
{
    size_t length = 0;
    char *html = d_read_file(template_path, &length);
    if (!html) return false;
    page_free_state(&tui.page);
    tui.page.url = d_strdup(url);
    tui.page.html = html;
    tui.page.palette = palette_of(html);
    tui.page.focus = -1;
    relayout(&tui.page);
    tui.has_page = tui.page.layout != NULL;
    tui.page.scroll = 0;
    return tui.has_page;
}

/* ---- the engine, once per navigation ------------------------------------ */

static void dispose_history_page(DAppPage *page)
{
    if (page->template_path) unlink(page->template_path);
}

static void nav_cleanup(void)
{
    if (tui.nav.out_fd >= 0) close(tui.nav.out_fd);
    if (tui.nav.err_fd >= 0) close(tui.nav.err_fd);
    if (tui.nav.body_path) { unlink(tui.nav.body_path); free(tui.nav.body_path); }
    dbuf_free(&tui.nav.pending);
    dbuf_free(&tui.nav.diagnostics);
    free(tui.nav.url);
    free(tui.nav.template_path);
    free(tui.nav.location);
    tui.nav = (Navigation){.out_fd = -1, .err_fd = -1};
    tui.loading = false;
}

static void nav_cancel(void)
{
    if (!tui.loading) return;
    if (tui.nav.pid > 0) {
        kill(tui.nav.pid, SIGTERM);
        while (waitpid(tui.nav.pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (tui.nav.template_path) unlink(tui.nav.template_path);
    nav_cleanup();
    set_status("Stopped");
}

/* A request body may hold a password: it goes to the engine in a private
   file, never on its command line, where the process table can read it. */
static char *write_private_body(const char *data)
{
    char *path = private_temp("distill-body");
    if (!path) return NULL;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    bool ok = fd >= 0;
    size_t left = strlen(data);
    while (ok && left) {
        ssize_t written = write(fd, data, left);
        if (written <= 0) { if (written < 0 && errno == EINTR) continue; ok = false; break; }
        data += written;
        left -= (size_t)written;
    }
    if (fd >= 0) close(fd);
    if (!ok) { unlink(path); free(path); return NULL; }
    return path;
}

static void start_navigation(const char *url, const char *method, const char *body,
                             bool https_guessed, NavKind kind)
{
    if (tui.loading) nav_cancel();
    char *template_path = private_temp("distill-template");
    if (!template_path) { set_status("Unable to create a temporary file"); return; }
    char *body_path = NULL;
    if (body && *body) {
        body_path = write_private_body(body);
        if (!body_path) {
            unlink(template_path);
            free(template_path);
            set_status("Unable to stage the request body");
            return;
        }
    }
    char zoom[16];
    snprintf(zoom, sizeof(zoom), "%d", tui.zoom);
    const char *argv[32];
    int argc = 0;
    argv[argc++] = tui.engine;
    argv[argc++] = "--url"; argv[argc++] = url;
    argv[argc++] = "--template"; argv[argc++] = template_path;
    argv[argc++] = "--theme"; argv[argc++] = tui.theme;
    argv[argc++] = "--zoom"; argv[argc++] = zoom;
    argv[argc++] = "--progress";
    if (https_guessed) argv[argc++] = "--https-optional";
    if (method && *method) { argv[argc++] = "--request-method"; argv[argc++] = method; }
    if (body_path) { argv[argc++] = "--request-data-file"; argv[argc++] = body_path; }
    if (tui.session_file) { argv[argc++] = "--session-file"; argv[argc++] = tui.session_file; }
    argv[argc++] = "--script-cap-ms"; argv[argc++] = "5000";
    argv[argc] = NULL;

    int out[2], err[2];
    if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
        set_status("Unable to start the engine: %s", strerror(errno));
        unlink(template_path); free(template_path);
        if (body_path) { unlink(body_path); free(body_path); }
        return;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, tui.engine, &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    close(err[1]);
    if (spawned != 0) {
        close(out[0]); close(err[0]);
        set_status("Unable to start the engine %s: %s", tui.engine, strerror(spawned));
        unlink(template_path); free(template_path);
        if (body_path) { unlink(body_path); free(body_path); }
        return;
    }
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
    fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL) | O_NONBLOCK);
    tui.nav = (Navigation){.pid = pid, .out_fd = out[0], .err_fd = err[0], .url = d_strdup(url),
                           .template_path = template_path, .body_path = body_path, .kind = kind};
    dbuf_init(&tui.nav.pending);
    dbuf_init(&tui.nav.diagnostics);
    snprintf(tui.nav.phase, sizeof(tui.nav.phase), "fetch");
    tui.loading = true;
    set_status("Loading %s", url);
}

static void navigate(const char *url, const char *method, const char *body, bool https_guessed)
{
    if (!d_page_uri_allowed(url)) {
        set_status("Blocked: not an address a page can be loaded from");
        return;
    }
    start_navigation(url, method, body, https_guessed, NAV_NEW);
}

/* One record per line: "progress PERCENT PHASE" while the page loads,
   "location URL" once the engine knows where the page really came from. */
static void consume_progress(void)
{
    char *start = tui.nav.pending.data;
    if (!start) return;
    char *newline;
    while ((newline = memchr(start, '\n', (size_t)(tui.nav.pending.data + tui.nav.pending.length - start)))) {
        *newline = '\0';
        int percent = 0;
        char phase[16] = "";
        if (sscanf(start, "progress %d %15s", &percent, phase) == 2) {
            tui.nav.percent = percent;
            snprintf(tui.nav.phase, sizeof(tui.nav.phase), "%s", phase);
        } else if (!strncmp(start, "location ", 9) && start[9]) {
            free(tui.nav.location);
            tui.nav.location = d_strdup(start + 9);
        }
        start = newline + 1;
    }
    size_t rest = (size_t)(tui.nav.pending.data + tui.nav.pending.length - start);
    memmove(tui.nav.pending.data, start, rest);
    tui.nav.pending.length = rest;
}

static bool drain(int *fd, DBuffer *into)
{
    char buffer[4096];
    for (;;) {
        ssize_t got = read(*fd, buffer, sizeof(buffer));
        if (got > 0) {
            if (into->length < 256 * 1024) dbuf_append(into, buffer, (size_t)got);
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return true;        /* EAGAIN: still open */
        close(*fd);
        *fd = -1;
        return false;
    }
}

static char *last_line(const DBuffer *text)
{
    if (!text->length) return d_strdup("the engine failed");
    size_t end = text->length;
    while (end && (text->data[end - 1] == '\n' || text->data[end - 1] == '\r')) --end;
    size_t start = end;
    while (start && text->data[start - 1] != '\n') --start;
    /* The run's summary line says nothing to a reader. */
    while (start && (!strncmp(text->data + start, "phase", 5) || end == start)) {
        end = start ? start - 1 : 0;
        while (end && (text->data[end - 1] == '\n')) --end;
        start = end;
        while (start && text->data[start - 1] != '\n') --start;
    }
    return end > start ? d_strndup(text->data + start, end - start) : d_strdup("the engine failed");
}

static void nav_finished(void)
{
    int status = 0;
    while (waitpid(tui.nav.pid, &status, 0) < 0 && errno == EINTR) {}
    tui.nav.pid = 0;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const char *final_url = tui.nav.location && d_page_uri_allowed(tui.nav.location)
                          ? tui.nav.location : tui.nav.url;
    if (code == 0 || code == 4) {
        char *url = d_strdup(final_url);
        char *template_path = tui.nav.template_path;
        tui.nav.template_path = NULL;
        bool ok = tui.nav.kind == NAV_RELOAD
            ? d_app_history_replace_current(&tui.history, url, template_path, tui.theme, tui.zoom,
                                            dispose_history_page)
            : d_app_history_push(&tui.history, url, template_path, tui.theme, tui.zoom,
                                 dispose_history_page);
        if (ok && show_page_file(url, template_path)) {
            if (code == 4)
                set_status("This site answered with a bot challenge; Distill shows what it sent and does not solve those");
            else
                set_status("%s", tui.page.layout->title[0] ? tui.page.layout->title : "Ready");
        } else {
            set_status("The reader page could not be read");
        }
        free(template_path);
        free(url);
    } else {
        char *why = last_line(&tui.nav.diagnostics);
        set_status("Could not load %s: %s", tui.nav.url, why);
        free(why);
        if (tui.nav.template_path) unlink(tui.nav.template_path);
    }
    nav_cleanup();
}

static void nav_service(void)
{
    if (!tui.loading) return;
    if (tui.nav.out_fd >= 0) {
        drain(&tui.nav.out_fd, &tui.nav.pending);
        consume_progress();
    }
    if (tui.nav.err_fd >= 0) drain(&tui.nav.err_fd, &tui.nav.diagnostics);
    if (tui.nav.out_fd < 0 && tui.nav.err_fd < 0) nav_finished();
}

static void show_history_current(void)
{
    const DAppPage *page = d_app_history_current(&tui.history);
    if (!page) return;
    if (show_page_file(page->url, page->template_path)) set_status("%s", page->url);
}

/* ---- things the page asks the shell to do -------------------------------- */

static void reap_externals(void)
{
    for (size_t i = 0; i < tui.external_count;) {
        if (waitpid(tui.externals[i], NULL, WNOHANG) != 0) {
            tui.externals[i] = tui.externals[--tui.external_count];
            continue;
        }
        ++i;
    }
}

/* Somewhere the reader does not go: a video's player, a mail address. On a
   desktop the system has a handler for it; over SSH there is nothing to hand
   it to, and the honest thing is to show the address. */
static void open_external(const char *uri)
{
    if (!d_external_uri_allowed(uri)) {
        set_status("Blocked: not an address that can be handed to the system");
        return;
    }
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if ((display && *display) || (wayland && *wayland)) {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        char *argv[] = {"xdg-open", (char *)uri, NULL};
        int spawned = posix_spawnp(&pid, "xdg-open", &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned == 0) {
            if (tui.external_count < sizeof(tui.externals) / sizeof(tui.externals[0]))
                tui.externals[tui.external_count++] = pid;
            set_status("Opened with the system handler: %s", uri);
            return;
        }
    }
    set_status("Open outside the terminal: %s", uri);
}

static void transfer_cancel(void)
{
    if (!tui.transfer) return;
    d_media_transfer_cancel(tui.transfer);
    tui.transfer = NULL;
    set_status("Transfer cancelled");
}

static void transfer_start(const char *url, const char *destination, int kind)
{
    if (tui.transfer) transfer_cancel();
    const char *engine_kind = kind == TRANSFER_AUDIO ? "audio" : kind == TRANSFER_IMAGE ? "image" : "any";
    char *error = NULL;
    tui.transfer = d_media_transfer_start(tui.engine, url, destination, engine_kind, &error);
    if (!tui.transfer) {
        set_status("Could not start the fetch: %s", error ? error : "unknown error");
        free(error);
        return;
    }
    tui.transfer_kind = kind;
    set_status("%s", kind == TRANSFER_AUDIO ? "Fetching audio\xe2\x80\xa6"
                   : kind == TRANSFER_IMAGE ? "Fetching the image\xe2\x80\xa6" : "Downloading\xe2\x80\xa6");
}

/* What to call a track: the words on the row before its Play button, which
   is how an audio list names each file; else the heading of the card above;
   else the file name. */
static char *row_words(ReaderLayout *layout, int row, int end_column)
{
    DBuffer out;
    dbuf_init(&out);
    for (int x = layout->left; x < end_column && x < layout->columns; ++x) {
        ReaderCell *cell = reader_layout_cell(layout, row, x);
        if (!cell->width || cell->role == READER_ROLE_BORDER || cell->link >= 0 || cell->control >= 0) continue;
        size_t length = 0;
        const char *text = reader_cell_text(layout, cell, &length);
        if (length) dbuf_append(&out, text, length);
        else if (out.length && out.data[out.length - 1] != ' ') dbuf_puts(&out, " ");
    }
    while (out.length && out.data[out.length - 1] == ' ') --out.length;
    if (!out.data) dbuf_puts(&out, "");
    else out.data[out.length] = '\0';
    return dbuf_take(&out);
}

static char *track_title(size_t link, const char *url)
{
    ReaderLayout *layout = tui.page.layout;
    if (layout && link < layout->link_count && layout->links[link].row >= 0) {
        int row = layout->links[link].row;
        char *words = row_words(layout, row, layout->links[link].column);
        if (*words) return words;
        free(words);
        for (int r = row - 1; r >= 0 && r >= row - 12; --r) {
            bool heading = false;
            for (int x = layout->left; x < layout->left + layout->measure && !heading; ++x)
                heading = reader_layout_cell(layout, r, x)->role == READER_ROLE_HEADING;
            if (heading) return row_words(layout, r, layout->left + layout->measure);
        }
    }
    const char *tail = strrchr(url, '/');
    return d_strdup(tail && tail[1] ? tail + 1 : url);
}

static void audio_release(void)
{
    media_audio_close();
    if (tui.audio_file) { unlink(tui.audio_file); free(tui.audio_file); tui.audio_file = NULL; }
    free(tui.audio_title);
    tui.audio_title = NULL;
}

static void play(const char *url, size_t link)
{
    if (!d_external_uri_allowed(url)) { set_status("Playback blocked: not a web address"); return; }
    MediaAudioState state;
    media_audio_snapshot(&state);
    /* Asking for what is already loaded is a transport command, not a refetch. */
    if (state.phase != MEDIA_AUDIO_STOPPED && state.phase != MEDIA_AUDIO_ERROR && !strcmp(state.item, url)) {
        if (state.phase == MEDIA_AUDIO_PLAYING) media_audio_pause();
        else media_audio_play();
        return;
    }
    audio_release();
    tui.audio_title = track_title(link, url);
    transfer_start(url, NULL, TRANSFER_AUDIO);
}

static char *suggested_path(const char *url)
{
    const char *tail = url ? strrchr(url, '/') : NULL;
    char *name = d_strdup(tail && tail[1] ? tail + 1 : "download");
    char *cut = strpbrk(name, "?#");
    if (cut) *cut = '\0';
    for (char *at = name; *at; ++at)
        if (*at == '/' || *at == '\\' || *at == ':') *at = '-';
    if (!*name || name[0] == '.') { char *safe = d_cat2("download", name); free(name); name = safe; }
    const char *home = getenv("HOME");
    char *directory = home && *home ? d_cat2(home, "/Downloads") : d_strdup(".");
    struct stat info;
    if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) { free(directory); directory = d_strdup("."); }
    char *path = d_cat3(directory, "/", name);
    free(directory);
    free(name);
    return path;
}

static void prompt_open(PromptKind kind, const char *label, const char *initial, bool secret)
{
    tui.prompt.kind = kind;
    snprintf(tui.prompt.label, sizeof(tui.prompt.label), "%s", label);
    dbuf_free(&tui.prompt.text);
    dbuf_init(&tui.prompt.text);
    dbuf_puts(&tui.prompt.text, initial ? initial : "");
    tui.prompt.cursor = tui.prompt.text.length;
    tui.prompt.secret = secret;
}

static void prompt_close(void)
{
    tui.prompt.kind = PROMPT_NONE;
    dbuf_free(&tui.prompt.text);
    free(tui.prompt.url);
    tui.prompt.url = NULL;
    tb_hide_cursor();
}

static void download(const char *url)
{
    if (!d_external_uri_allowed(url)) { set_status("Download blocked: not a web address"); return; }
    char *path = suggested_path(url);
    prompt_open(PROMPT_SAVE_AS, "Save as: ", path, false);
    tui.prompt.url = d_strdup(url);
    free(path);
}

static void viewer_free_image(void)
{
    if (tui.viewer.loaded) d_image_free(&tui.viewer.image);
    tui.viewer.loaded = false;
    if (tui.viewer.file) { unlink(tui.viewer.file); free(tui.viewer.file); tui.viewer.file = NULL; }
    free(tui.viewer.failure);
    tui.viewer.failure = NULL;
}

static void viewer_fetch(void)
{
    viewer_free_image();
    tui.viewer.drawn = false;
    transfer_start(tui.viewer.urls[tui.viewer.index], NULL, TRANSFER_IMAGE);
}

/* Viewer mode is a state of this surface, not a second window: the page's
   images in order, one at a time. */
static void viewer_enter(const char *url)
{
    ReaderLayout *layout = tui.page.layout;
    for (size_t i = 0; i < tui.viewer.count; ++i) free(tui.viewer.urls[i]);
    free(tui.viewer.urls);
    tui.viewer.urls = NULL;
    tui.viewer.count = 0;
    tui.viewer.index = 0;
    for (size_t i = 0; layout && i < layout->link_count; ++i) {
        const char *href = layout->links[i].href;
        if (!d_starts(href, MEDIA_SCHEME)) continue;
        const char *target = href + strlen(MEDIA_SCHEME);
        bool seen = false;
        for (size_t k = 0; k < tui.viewer.count; ++k) seen |= !strcmp(tui.viewer.urls[k], target);
        if (seen) continue;
        if (!strcmp(target, url)) tui.viewer.index = tui.viewer.count;
        tui.viewer.urls = d_xrealloc(tui.viewer.urls, (tui.viewer.count + 1) * sizeof(char *));
        tui.viewer.urls[tui.viewer.count++] = d_strdup(target);
    }
    if (!tui.viewer.count) {
        tui.viewer.urls = d_xmalloc(sizeof(char *));
        tui.viewer.urls[tui.viewer.count++] = d_strdup(url);
    }
    tui.viewer.active = true;
    viewer_fetch();
}

static void viewer_leave(void)
{
    if (!tui.viewer.active) return;
    if (tui.transfer && tui.transfer_kind == TRANSFER_IMAGE) transfer_cancel();
    viewer_free_image();
    tui.viewer.active = false;
    if (tui.graphics == GRAPHICS_KITTY) {
        const char *clear = d_term_image_kitty_clear();
        tb_send(clear, strlen(clear));
    }
    d_screen_invalidate(&tui.screen);
    set_status("%s", tui.page.url ? tui.page.url : "");
}

static void viewer_step(int delta)
{
    if (!tui.viewer.active || tui.viewer.count < 2) return;
    if (delta < 0) tui.viewer.index = tui.viewer.index ? tui.viewer.index - 1 : tui.viewer.count - 1;
    else tui.viewer.index = (tui.viewer.index + 1) % tui.viewer.count;
    if (tui.graphics == GRAPHICS_KITTY) {
        const char *clear = d_term_image_kitty_clear();
        tb_send(clear, strlen(clear));
    }
    d_screen_invalidate(&tui.screen);
    viewer_fetch();
}

static void transfer_service(void)
{
    if (!tui.transfer) return;
    DTransferState state = d_media_transfer_poll(tui.transfer);
    if (state == D_TRANSFER_RUNNING) return;
    DMediaTransfer *transfer = tui.transfer;
    tui.transfer = NULL;
    if (state == D_TRANSFER_FAILED) {
        if (tui.transfer_kind == TRANSFER_IMAGE && tui.viewer.active) {
            tui.viewer.failure = d_strdup(d_media_transfer_error(transfer));
            d_screen_invalidate(&tui.screen);
        }
        set_status("Could not fetch the media: %s", d_media_transfer_error(transfer));
    } else if (tui.transfer_kind == TRANSFER_DOWNLOAD) {
        set_status("Saved to %s", d_media_transfer_path(transfer));
    } else if (tui.transfer_kind == TRANSFER_AUDIO) {
        tui.audio_file = d_strdup(d_media_transfer_path(transfer));
        if (!media_audio_open(tui.audio_file, d_media_transfer_url(transfer))) {
            MediaAudioState audio;
            media_audio_snapshot(&audio);
            set_status("Cannot play this file: %s",
                       audio.error_message[0] ? audio.error_message : "no decoder accepted it");
        } else if (media_audio_play()) {
            set_status("Playing \xe2\x80\x94 Space pauses, s stops, \xe2\x86\x90/\xe2\x86\x92 seek, [ ] volume, x closes");
        }
    } else if (tui.transfer_kind == TRANSFER_IMAGE && tui.viewer.active) {
        tui.viewer.file = d_strdup(d_media_transfer_path(transfer));
        if (d_image_load_file(tui.viewer.file, &tui.viewer.image)) {
            tui.viewer.loaded = true;
            set_status("Image %zu of %zu \xe2\x80\x94 \xe2\x86\x90/\xe2\x86\x92 step, Esc closes",
                       tui.viewer.index + 1, tui.viewer.count);
        } else {
            tui.viewer.failure = d_strdup(d_image_last_error());
            set_status("The image could not be decoded: %s", tui.viewer.failure);
        }
        tui.viewer.drawn = false;
        d_screen_invalidate(&tui.screen);
    } else {
        unlink(d_media_transfer_path(transfer));
    }
    d_media_transfer_free(transfer);
}

static void jump_to_section(const char *id)
{
    ReaderLayout *layout = tui.page.layout;
    for (size_t i = 0; layout && i < layout->anchor_count; ++i) {
        if (strcmp(layout->anchors[i].id, id)) continue;
        tui.page.scroll = layout->anchors[i].row;
        clamp_scroll();
        return;
    }
    if (!strcmp(id, "top")) { tui.page.scroll = 0; return; }
    set_status("No section %s on this page", id);
}

static void follow_link(size_t index)
{
    ReaderLayout *layout = tui.page.layout;
    if (!layout || index >= layout->link_count) return;
    const char *href = layout->links[index].href;
    char *unwrapped = d_address_unwrap_redirect(href);
    const char *uri = unwrapped ? unwrapped : href;
    if (d_starts(uri, SECTION_SCHEME)) jump_to_section(uri + strlen(SECTION_SCHEME));
    else if (d_starts(uri, MEDIA_SCHEME)) viewer_enter(uri + strlen(MEDIA_SCHEME));
    else if (d_starts(uri, PLAY_SCHEME)) play(uri + strlen(PLAY_SCHEME), index);
    else if (d_starts(uri, DOWNLOAD_SCHEME)) download(uri + strlen(DOWNLOAD_SCHEME));
    else if (d_starts(uri, OPEN_SCHEME)) open_external(uri + strlen(OPEN_SCHEME));
    else if (d_starts(uri, "distill-")) { /* a desktop affordance with nothing to do here */ }
    else if (!strncasecmp(uri, "mailto:", 7) || !strncasecmp(uri, "tel:", 4)) open_external(uri);
    else if (d_page_uri_allowed(uri)) navigate(uri, NULL, NULL, false);
    else open_external(uri);
    free(unwrapped);
}

/* ---- forms --------------------------------------------------------------- */

static bool control_type(const ReaderLayoutControl *control, const char *type)
{
    return !strcasecmp(control->type, type);
}

static bool control_is_button(const ReaderLayoutControl *control)
{
    return !strcmp(control->tag, "button") || control_type(control, "submit") ||
           control_type(control, "reset") || control_type(control, "button") ||
           control_type(control, "image");
}

static void submit_form(size_t submitter)
{
    ReaderLayout *layout = tui.page.layout;
    size_t count = layout->control_count;
    DFormField *fields = d_xcalloc(count ? count : 1, sizeof(*fields));
    for (size_t i = 0; i < count; ++i) {
        ReaderLayoutControl *control = &layout->controls[i];
        bool choice = control_type(control, "checkbox") || control_type(control, "radio");
        fields[i] = (DFormField){
            .form_index = control->in_form ? control->form_index : UINT_MAX,
            .tag = control->tag, .type = control->type, .name = control->name,
            .value = choice ? control->value : tui.page.values[i],
            .checked = choice && tui.page.checked[i],
            .disabled = control->disabled, .required = control->required,
        };
    }
    ReaderLayoutControl *source = &layout->controls[submitter];
    if (!source->in_form) { free(fields); set_status("This control belongs to no form"); return; }
    DFormRequest request;
    DFormStatus status = d_form_build(fields, count, submitter, source->action,
                                      *source->method ? source->method : "get", &request);
    free(fields);
    if (status == D_FORM_MISSING_REQUIRED) set_status("Complete required field: %s", request.missing);
    else if (status == D_FORM_BLOCKED) set_status("Form blocked: unsupported action URI scheme");
    else if (status == D_FORM_OK) {
        bool post = !strcmp(request.method, "POST");
        navigate(request.target, post ? "POST" : NULL, post ? request.body : NULL, false);
    }
    d_form_request_free(&request);
}

static void paint(size_t control)
{
    reader_layout_paint_control(tui.page.layout, control, tui.page.values[control], tui.page.checked[control]);
}

static void popup_open(size_t control)
{
    ReaderLayoutControl *c = &tui.page.layout->controls[control];
    Popup popup = {.control = control};
    for (const char *record = c->options; record && *record;) {
        const char *end = strchr(record, '\x1e');
        if (!end) break;
        const char *unit = memchr(record, '\x1f', (size_t)(end - record));
        const char *label = unit ? unit + 1 : record;
        const char *label_end = unit ? memchr(label, '\x1f', (size_t)(end - label)) : end;
        if (!label_end) label_end = end;
        popup.values = d_xrealloc(popup.values, (popup.count + 1) * sizeof(char *));
        popup.labels = d_xrealloc(popup.labels, (popup.count + 1) * sizeof(char *));
        popup.values[popup.count] = d_strndup(record, unit ? (size_t)(unit - record) : (size_t)(end - record));
        popup.labels[popup.count] = d_strndup(label, (size_t)(label_end - label));
        if (!strcmp(popup.values[popup.count], tui.page.values[control])) popup.selected = popup.count;
        ++popup.count;
        record = end + 1;
    }
    if (!popup.count) { set_status("This list has no choices"); return; }
    tui.popup = popup;
    tui.popup_open = true;
}

static void popup_close(bool choose)
{
    if (!tui.popup_open) return;
    if (choose) {
        size_t control = tui.popup.control;
        free(tui.page.values[control]);
        tui.page.values[control] = d_strdup(tui.popup.values[tui.popup.selected]);
        paint(control);
    }
    for (size_t i = 0; i < tui.popup.count; ++i) { free(tui.popup.values[i]); free(tui.popup.labels[i]); }
    free(tui.popup.values);
    free(tui.popup.labels);
    tui.popup = (Popup){0};
    tui.popup_open = false;
}

static void activate_control(size_t index)
{
    ReaderLayout *layout = tui.page.layout;
    ReaderLayoutControl *control = &layout->controls[index];
    if (control->disabled) return;
    if (control_type(control, "checkbox")) {
        tui.page.checked[index] = !tui.page.checked[index];
        paint(index);
    } else if (control_type(control, "radio")) {
        for (size_t i = 0; i < layout->control_count; ++i) {
            ReaderLayoutControl *other = &layout->controls[i];
            if (i != index && control_type(other, "radio") && other->form_index == control->form_index &&
                !strcmp(other->name, control->name) && tui.page.checked[i]) {
                tui.page.checked[i] = false;
                paint(i);
            }
        }
        tui.page.checked[index] = true;
        paint(index);
    } else if (!strcmp(control->tag, "select")) {
        popup_open(index);
    } else if (control_type(control, "reset")) {
        for (size_t i = 0; i < layout->control_count; ++i) {
            if (layout->controls[i].form_index != control->form_index) continue;
            free(tui.page.values[i]);
            tui.page.values[i] = d_strdup(layout->controls[i].value);
            tui.page.checked[i] = layout->controls[i].checked;
            paint(i);
        }
    } else if (control_type(control, "button")) {
        set_status("This button only does something with the page's own scripts");
    } else if (control_is_button(control)) {
        submit_form(index);
    } else {
        const char *label = *control->placeholder ? control->placeholder : control->name;
        char prompt[160];
        snprintf(prompt, sizeof(prompt), "%s: ", *label ? label : "Text");
        prompt_open(PROMPT_FIELD, prompt, tui.page.values[index], control_type(control, "password"));
        tui.prompt.control = index;
    }
}

static void activate_focus(void)
{
    if (tui.page.focus < 0) return;
    Focus *focus = &tui.page.focusables[tui.page.focus];
    if (focus->kind == FOCUS_LINK) follow_link(focus->index);
    else activate_control(focus->index);
}

static void move_focus(int direction)
{
    Page *page = &tui.page;
    if (!page->focus_count) return;
    if (page->focus < 0) {
        /* Start from what is on screen, not from the top of the page. */
        int first = -1;
        for (size_t i = 0; i < page->focus_count; ++i) {
            int row = page->focusables[i].row;
            if (row >= page->scroll && row < page->scroll + view_height()) {
                if (direction > 0 && first < 0) first = (int)i;
                if (direction < 0) first = (int)i;
            }
        }
        page->focus = first >= 0 ? first : (direction > 0 ? 0 : (int)page->focus_count - 1);
    } else {
        page->focus = (page->focus + direction + (int)page->focus_count) % (int)page->focus_count;
    }
    reveal_row(page->focusables[page->focus].row);
}

/* ---- drawing ------------------------------------------------------------- */

typedef struct {
    DColor fg, bg;
    uint8_t attrs;
} Look;

static bool themed(void)
{
    return tui.screen.colors == D_SCREEN_COLORS_TRUE && tui.has_page && tui.page.palette.valid;
}

static Look look_for(uint8_t role, uint8_t cell_attrs, bool in_column, bool focused)
{
    Look look = {D_COLOR_DEFAULT, D_COLOR_DEFAULT, 0};
    if (cell_attrs & READER_CELL_BOLD) look.attrs |= D_SCREEN_BOLD;
    if (cell_attrs & READER_CELL_ITALIC) look.attrs |= D_SCREEN_ITALIC;
    if (cell_attrs & READER_CELL_UNDERLINE) look.attrs |= D_SCREEN_UNDERLINE;
    if (themed()) {
        const Palette *p = &tui.page.palette;
        look.fg = D_COLOR_RGB(p->text);
        look.bg = D_COLOR_RGB(in_column ? p->main_bg : p->page_bg);
        switch (role) {
        case READER_ROLE_HEADING: look.fg = D_COLOR_RGB(p->heading); break;
        case READER_ROLE_LINK: look.fg = D_COLOR_RGB(p->link); break;
        case READER_ROLE_BUTTON: look.fg = D_COLOR_RGB(p->main_bg); look.bg = D_COLOR_RGB(p->link);
                                 look.attrs |= D_SCREEN_BOLD; break;
        case READER_ROLE_MUTED: look.fg = D_COLOR_RGB(p->muted); break;
        case READER_ROLE_CODE: look.bg = D_COLOR_RGB(p->pre_bg); break;
        case READER_ROLE_RULE:
        case READER_ROLE_BORDER: look.fg = D_COLOR_RGB(p->border); break;
        case READER_ROLE_FIELD: look.bg = D_COLOR_RGB(p->card_bg); break;
        default: break;
        }
    } else {
        switch (role) {
        case READER_ROLE_HEADING: look.attrs |= D_SCREEN_BOLD; break;
        case READER_ROLE_LINK: look.fg = D_COLOR_ANSI(6); break;
        case READER_ROLE_BUTTON: look.attrs |= D_SCREEN_REVERSE | D_SCREEN_BOLD; break;
        case READER_ROLE_MUTED:
        case READER_ROLE_RULE:
        case READER_ROLE_BORDER: look.attrs |= D_SCREEN_DIM; break;
        default: break;
        }
    }
    if (focused) look.attrs ^= D_SCREEN_REVERSE;
    return look;
}

static Look bar_look(void)
{
    if (themed()) {
        const Palette *p = &tui.page.palette;
        return (Look){D_COLOR_RGB(p->text), D_COLOR_RGB(p->card_bg), 0};
    }
    return (Look){D_COLOR_DEFAULT, D_COLOR_DEFAULT, D_SCREEN_REVERSE};
}

static void draw_page(void)
{
    int top = view_top(), height = view_height();
    ReaderLayout *layout = tui.page.layout;
    Look margin = look_for(READER_ROLE_TEXT, 0, false, false);
    Look column = look_for(READER_ROLE_TEXT, 0, true, false);
    for (int y = 0; y < height; ++y) {
        d_screen_fill(&tui.screen, 0, top + y, tui.width, margin.fg, margin.bg, 0);
        /* The reading column runs the height of the window, as it does on
           the desktop, not only as far as the page has words. */
        if (layout) d_screen_fill(&tui.screen, layout->left, top + y, layout->left + layout->measure,
                                  column.fg, column.bg, 0);
    }
    if (!layout) return;
    int focused_link = -1, focused_control = -1;
    if (tui.page.focus >= 0) {
        Focus *focus = &tui.page.focusables[tui.page.focus];
        if (focus->kind == FOCUS_LINK) focused_link = (int)focus->index;
        else focused_control = (int)focus->index;
    }
    for (int y = 0; y < height; ++y) {
        int row = tui.page.scroll + y;
        if (row >= layout->rows) break;
        for (int x = 0; x < layout->columns && x < tui.width; ++x) {
            ReaderCell *cell = reader_layout_cell(layout, row, x);
            if (!cell->width) continue;
            bool in_column = x >= layout->left && x < layout->left + layout->measure;
            bool focused = (focused_link >= 0 && cell->link == focused_link) ||
                           (focused_control >= 0 && cell->control == focused_control);
            Look look = look_for(cell->role, cell->attrs, in_column, focused);
            size_t length = 0;
            const char *text = reader_cell_text(layout, cell, &length);
            if (length) d_screen_put(&tui.screen, x, top + y, text, length, cell->width, look.fg, look.bg, look.attrs);
            else d_screen_put(&tui.screen, x, top + y, " ", 1, 1, look.fg, look.bg, look.attrs);
        }
    }
}

static void format_clock(char *out, size_t size, double seconds)
{
    if (seconds < 0 || seconds != seconds) seconds = 0;
    long total = (long)(seconds + 0.5);
    long hours = total / 3600, minutes = (total % 3600) / 60, rest = total % 60;
    if (hours) snprintf(out, size, "%ld:%02ld:%02ld", hours, minutes, rest);
    else snprintf(out, size, "%ld:%02ld", minutes, rest);
}

/* The transport: where in the track, a line to see it on, and a head to
   drag -- the same controls the desktop draws, in cells. */
static void draw_transport(void)
{
    if (tui.transport_row < 0) return;
    int y = tui.transport_row;
    Look bar = bar_look();
    d_screen_fill(&tui.screen, 0, y, tui.width, bar.fg, bar.bg, bar.attrs);
    MediaAudioState state;
    media_audio_snapshot(&state);
    char now[16], total[16];
    format_clock(now, sizeof(now), state.position_seconds);
    format_clock(total, sizeof(total), state.duration_seconds);
    const char *symbol = state.phase == MEDIA_AUDIO_PLAYING ? "\xe2\x96\xb6" : "\xe2\x8f\xb8";  /* ▶ ⏸ */
    if (state.phase == MEDIA_AUDIO_FINISHED) symbol = "\xe2\x96\xa0";                          /* ■ */
    int x = d_screen_text(&tui.screen, 1, y, tui.width, symbol, bar.fg, bar.bg, bar.attrs | D_SCREEN_BOLD);
    x = d_screen_text(&tui.screen, x + 1, y, tui.width, now, bar.fg, bar.bg, bar.attrs);
    char tail[256];
    snprintf(tail, sizeof(tail), " %s  vol %d%%  %s", total, (int)(state.volume * 100 + 0.5),
             tui.audio_title ? tui.audio_title : "");
    int tail_width = reader_text_cells(tail, strlen(tail));
    int bar_width = tui.width - x - 2 - (tail_width < tui.width / 2 ? tail_width : tui.width / 2);
    if (bar_width < 4) bar_width = 4;
    tui.bar_start = x + 1;
    tui.bar_end = x + 1 + bar_width;
    double fraction = state.duration_seconds > 0 ? state.position_seconds / state.duration_seconds : 0;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int head = tui.bar_start + (int)(fraction * (bar_width - 1) + 0.5);
    for (int i = tui.bar_start; i < tui.bar_end; ++i) {
        const char *glyph = i == head ? "\xe2\x97\x8f" : i < head ? "\xe2\x94\x81" : "\xe2\x94\x80";  /* ● ━ ─ */
        d_screen_put(&tui.screen, i, y, glyph, 3, 1, bar.fg, bar.bg, bar.attrs | (i == head ? D_SCREEN_BOLD : 0));
    }
    d_screen_text(&tui.screen, tui.bar_end, y, tui.width, tail, bar.fg, bar.bg, bar.attrs);
}

static void draw_top(void)
{
    Look bar = bar_look();
    d_screen_fill(&tui.screen, 0, 0, tui.width, bar.fg, bar.bg, bar.attrs);
    if (tui.prompt.kind == PROMPT_ADDRESS) return;   /* drawn by the prompt */
    bool back = d_app_history_can_back(&tui.history), forward = d_app_history_can_forward(&tui.history);
    int x = d_screen_text(&tui.screen, 1, 0, tui.width, "\xe2\x80\xb9", bar.fg, bar.bg, bar.attrs | (back ? D_SCREEN_BOLD : D_SCREEN_DIM));
    x = d_screen_text(&tui.screen, x + 1, 0, tui.width, "\xe2\x80\xba", bar.fg, bar.bg, bar.attrs | (forward ? D_SCREEN_BOLD : D_SCREEN_DIM));
    const char *title = tui.has_page && tui.page.layout && *tui.page.layout->title ? tui.page.layout->title : "Distill";
    x = d_screen_text(&tui.screen, x + 2, 0, tui.width - 1, title, bar.fg, bar.bg, bar.attrs | D_SCREEN_BOLD);
    if (tui.has_page && tui.page.url) {
        x = d_screen_text(&tui.screen, x, 0, tui.width - 1, "  ", bar.fg, bar.bg, bar.attrs);
        d_screen_text(&tui.screen, x, 0, tui.width - 1, tui.page.url, bar.fg, bar.bg, bar.attrs | D_SCREEN_DIM);
    }
}

static void draw_prompt(int y)
{
    Look bar = bar_look();
    d_screen_fill(&tui.screen, 0, y, tui.width, bar.fg, bar.bg, bar.attrs);
    int x = d_screen_text(&tui.screen, 1, y, tui.width, tui.prompt.label, bar.fg, bar.bg, bar.attrs | D_SCREEN_BOLD);
    DBuffer shown;
    dbuf_init(&shown);
    const char *text = tui.prompt.text.data ? tui.prompt.text.data : "";
    size_t cursor = tui.prompt.cursor;
    if (tui.prompt.secret) {
        int before = reader_text_cells(text, cursor);
        int all = reader_text_cells(text, strlen(text));
        for (int i = 0; i < all; ++i) dbuf_puts(&shown, "\xe2\x80\xa2");
        cursor = (size_t)before * 3;
    } else {
        dbuf_puts(&shown, text);
    }
    const char *display = shown.data ? shown.data : "";
    int room = tui.width - x - 2;
    int before = reader_text_cells(display, cursor);
    /* Keep the cursor in view: scroll the text left by whole clusters. */
    size_t skip = 0;
    while (before > room && skip < cursor) {
        size_t step = 1;
        while (skip + step < cursor && ((unsigned char)display[skip + step] & 0xc0) == 0x80) ++step;
        before -= reader_text_cells(display + skip, step);
        skip += step;
    }
    int cursor_x = x + reader_text_cells(display + skip, cursor - skip);
    d_screen_text(&tui.screen, x, y, tui.width - 1, display + skip, bar.fg, bar.bg, bar.attrs);
    dbuf_free(&shown);
    tb_set_cursor(cursor_x < tui.width ? cursor_x : tui.width - 1, y);
}

static void draw_status(void)
{
    int y = tui.height - 1;
    if (tui.prompt.kind == PROMPT_FIELD || tui.prompt.kind == PROMPT_SAVE_AS) { draw_prompt(y); return; }
    Look bar = bar_look();
    d_screen_fill(&tui.screen, 0, y, tui.width, bar.fg, bar.bg, bar.attrs);
    char text[1100];
    if (tui.loading)
        snprintf(text, sizeof(text), "Loading %d%% %s \xe2\x80\x94 Esc stops", tui.nav.percent, tui.nav.phase);
    else if (tui.transfer)
        snprintf(text, sizeof(text), "%s \xe2\x80\x94 Esc cancels",
                 tui.transfer_kind == TRANSFER_AUDIO ? "Fetching audio\xe2\x80\xa6"
                 : tui.transfer_kind == TRANSFER_IMAGE ? "Fetching the image\xe2\x80\xa6" : "Downloading\xe2\x80\xa6");
    else
        snprintf(text, sizeof(text), "%s", tui.status ? tui.status : "");
    d_screen_text(&tui.screen, 1, y, tui.width - 8, text, bar.fg, bar.bg, bar.attrs);
    d_screen_text(&tui.screen, tui.width - 7, y, tui.width, " ? help", bar.fg, bar.bg, bar.attrs | D_SCREEN_DIM);
}

static void draw_box(int x0, int y0, int w, int h, const char *title)
{
    Look bar = bar_look();
    for (int y = y0; y < y0 + h && y < tui.height; ++y)
        d_screen_fill(&tui.screen, x0, y, x0 + w, bar.fg, bar.bg, bar.attrs);
    if (title) d_screen_text(&tui.screen, x0 + 2, y0, x0 + w - 1, title, bar.fg, bar.bg, bar.attrs | D_SCREEN_BOLD);
}

static void draw_popup(void)
{
    if (!tui.popup_open) return;
    int width = 20;
    for (size_t i = 0; i < tui.popup.count; ++i) {
        int w = reader_text_cells(tui.popup.labels[i], strlen(tui.popup.labels[i])) + 6;
        if (w > width) width = w;
    }
    if (width > tui.width - 4) width = tui.width - 4;
    int visible = (int)tui.popup.count;
    if (visible > view_height() - 2) visible = view_height() - 2;
    if (visible < 1) visible = 1;
    int x0 = (tui.width - width) / 2, y0 = view_top() + (view_height() - visible - 2) / 2;
    draw_box(x0, y0, width, visible + 2, "Choose");
    size_t first = tui.popup.selected >= (size_t)visible ? tui.popup.selected - (size_t)visible + 1 : 0;
    Look bar = bar_look();
    for (int i = 0; i < visible && first + (size_t)i < tui.popup.count; ++i) {
        size_t index = first + (size_t)i;
        uint8_t attrs = bar.attrs ^ (index == tui.popup.selected ? D_SCREEN_REVERSE : 0);
        d_screen_fill(&tui.screen, x0 + 1, y0 + 1 + i, x0 + width - 1, bar.fg, bar.bg, attrs);
        d_screen_text(&tui.screen, x0 + 2, y0 + 1 + i, x0 + width - 1, tui.popup.labels[index], bar.fg, bar.bg, attrs);
    }
}

typedef struct {
    const char *keys, *what;
} HelpEntry;

typedef struct {
    const char *title;
    HelpEntry entries[8];
} HelpGroup;

/* In reading order for one column; help_left and help_right arrange them
   in two when the terminal is wide enough. */
static const HelpGroup help_groups[] = {
    {"Reading", {{"j k \xe2\x86\x93 \xe2\x86\x91", "scroll a line"},
                 {"Space PgDn", "page down"},
                 {"b PgUp", "page up"},
                 {"g Home", "top"},
                 {"G End", "bottom"},
                 {"Alt-1\xe2\x80\xa6" "9", "jump to a section"},
                 {"< > \xe2\x86\x90 \xe2\x86\x92", "scroll a wide table"}}},
    {"Links and fields", {{"Tab", "next link or field"},
                          {"S-Tab", "previous one"},
                          {"Enter", "follow, press, edit"},
                          {"Esc", "let go of it"}}},
    {"Going places", {{"o Ctrl-L", "address or search"},
                      {"Backspace H", "back"},
                      {"L", "forward"},
                      {"r F5", "reload"},
                      {"Esc", "stop loading"}}},
    {"Audio", {{"Space", "play, pause"},
               {"s", "stop"},
               {"\xe2\x86\x90 \xe2\x86\x92", "seek 5 seconds"},
               {"[ ]", "volume"},
               {"x", "close the player"}}},
    {"Pictures", {{"Enter", "open"},
                  {"\xe2\x86\x90 \xe2\x86\x92", "previous, next"},
                  {"d", "save"},
                  {"Esc", "close"}}},
    {"Distill", {{"t", "change the theme"},
                 {"?", "these keys"},
                 {"q", "quit"}}},
};

static const size_t help_left[] = {0, 1, 5}, help_right[] = {2, 3, 4},
                    help_single[] = {0, 1, 2, 3, 4, 5};

enum { HELP_GROUPS = sizeof(help_groups) / sizeof(help_groups[0]), HELP_KEY_GAP = 2, HELP_COLUMN_GAP = 4 };

/* One laid-out row of a help column: a group title or an entry. */
typedef struct {
    const HelpGroup *group;
    const HelpEntry *entry;
} HelpRow;

static int help_entry_count(const HelpGroup *group)
{
    int n = 0;
    while (n < 8 && group->entries[n].keys) ++n;
    return n;
}

/* Lay the given groups out as one column, a blank row between groups.
   Returns the row count. */
static int help_column(const size_t *order, size_t count, HelpRow *rows)
{
    int n = 0;
    for (size_t i = 0; i < count; ++i) {
        const HelpGroup *group = &help_groups[order[i]];
        if (i) rows[n++] = (HelpRow){0};
        rows[n++] = (HelpRow){group, NULL};
        for (int e = 0; e < help_entry_count(group); ++e) rows[n++] = (HelpRow){group, &group->entries[e]};
    }
    return n;
}

/* A horizontal border row: corners, a rule, and optional text set into it. */
static void help_border(int x0, int y, int width, bool top, const char *label, bool label_right,
                        Look edge, Look title)
{
    d_screen_text(&tui.screen, x0, y, x0 + 1, top ? "\xe2\x95\xad" : "\xe2\x95\xb0", edge.fg, edge.bg, edge.attrs);
    for (int x = x0 + 1; x < x0 + width - 1; ++x)
        d_screen_text(&tui.screen, x, y, x + 1, "\xe2\x94\x80", edge.fg, edge.bg, edge.attrs);
    d_screen_text(&tui.screen, x0 + width - 1, y, x0 + width, top ? "\xe2\x95\xae" : "\xe2\x95\xaf",
                  edge.fg, edge.bg, edge.attrs);
    if (!label) return;
    int cells = reader_text_cells(label, strlen(label)) + 2;
    if (cells > width - 6) return;
    int x = label_right ? x0 + width - 3 - cells : x0 + 3;
    d_screen_text(&tui.screen, x, y, x + cells, " ", title.fg, title.bg, title.attrs);
    d_screen_text(&tui.screen, x + 1, y, x + cells - 1, label, title.fg, title.bg, title.attrs);
    d_screen_text(&tui.screen, x + cells - 1, y, x + cells, " ", title.fg, title.bg, title.attrs);
}

static void draw_help(void)
{
    if (!tui.help) return;
    int key_width = 0, what_width = 0;
    for (size_t g = 0; g < HELP_GROUPS; ++g) {
        int title = reader_text_cells(help_groups[g].title, strlen(help_groups[g].title));
        if (title > what_width) what_width = title;
        for (int e = 0; e < help_entry_count(&help_groups[g]); ++e) {
            const HelpEntry *entry = &help_groups[g].entries[e];
            int k = reader_text_cells(entry->keys, strlen(entry->keys));
            int w = reader_text_cells(entry->what, strlen(entry->what));
            if (k > key_width) key_width = k;
            if (w > what_width) what_width = w;
        }
    }
    int column = key_width + HELP_KEY_GAP + what_width;
    int room_x = tui.width - 2, room_y = view_height();
    /* Border plus side padding of three cells, or one when space is short. */
    int pad = 3;
    bool two = 2 + 2 * pad + 2 * column + HELP_COLUMN_GAP <= room_x;
    if (!two && 2 + 2 * 2 + 2 * column + HELP_COLUMN_GAP <= room_x) { two = true; pad = 2; }
    HelpRow left[64], right[64];
    int left_rows = two ? help_column(help_left, 3, left) : help_column(help_single, HELP_GROUPS, left);
    int right_rows = two ? help_column(help_right, 3, right) : 0;
    int content = left_rows > right_rows ? left_rows : right_rows;

    int inner = two ? 2 * column + HELP_COLUMN_GAP : column;
    if (!two) pad = 2 + inner + 2 * 3 <= room_x ? 3 : 1;
    int width = 2 + 2 * pad + inner;
    if (width > room_x) width = room_x;
    /* A blank row above and below the keys, dropped before anything is cut. */
    int vpad = content + 4 <= room_y ? 1 : 0;
    int height = content + 2 + 2 * vpad;
    if (height > room_y) height = room_y;
    int visible = height - 2 - 2 * vpad;
    if (visible < 0) visible = 0;
    int most = content - visible > 0 ? content - visible : 0;
    if (tui.help_scroll > most) tui.help_scroll = most;
    if (tui.help_scroll < 0) tui.help_scroll = 0;
    tui.help_most = most;

    int x0 = (tui.width - width) / 2, y0 = view_top() + (room_y - height) / 2;
    Look box = bar_look();
    Look edge = look_for(READER_ROLE_BORDER, 0, true, false);
    Look title = look_for(READER_ROLE_HEADING, 0, true, false);
    Look keys = look_for(READER_ROLE_LINK, 0, true, false);
    Look muted = look_for(READER_ROLE_MUTED, 0, true, false);
    edge.bg = title.bg = keys.bg = muted.bg = box.bg;
    title.attrs |= D_SCREEN_BOLD;
    keys.attrs |= D_SCREEN_BOLD;
    if (!themed()) {
        /* Reverse video carries the box on a plain terminal; colours would
           fight it, so keep only the weights. A dim rule vanishes in
           reverse, so the border is drawn at full strength. */
        edge.fg = title.fg = keys.fg = muted.fg = box.fg;
        edge.attrs = box.attrs;
        title.attrs |= box.attrs;
        keys.attrs |= box.attrs;
        muted.attrs |= box.attrs;
    }

    for (int y = y0; y < y0 + height; ++y) {
        d_screen_fill(&tui.screen, x0, y, x0 + width, box.fg, box.bg, box.attrs);
        if (y == y0 || y == y0 + height - 1) continue;
        d_screen_text(&tui.screen, x0, y, x0 + 1, "\xe2\x94\x82", edge.fg, edge.bg, edge.attrs);
        d_screen_text(&tui.screen, x0 + width - 1, y, x0 + width, "\xe2\x94\x82", edge.fg, edge.bg, edge.attrs);
    }
    const char *hint = most == 0 ? "any key closes"
                     : tui.help_scroll < most ? "\xe2\x86\x93 more \xc2\xb7 any other key closes"
                     : "\xe2\x86\x91 back \xc2\xb7 any other key closes";
    help_border(x0, y0, width, true, "Keys", false, edge, title);
    help_border(x0, y0 + height - 1, width, false, hint, true, edge, muted);

    int limit = x0 + width - 1;
    for (int c = 0; c < (two ? 2 : 1); ++c) {
        const HelpRow *rows = c ? right : left;
        int count = c ? right_rows : left_rows;
        int x = x0 + 1 + pad + c * (column + HELP_COLUMN_GAP);
        for (int i = 0; i < visible && tui.help_scroll + i < count; ++i) {
            const HelpRow *row = &rows[tui.help_scroll + i];
            int y = y0 + 1 + vpad + i;
            if (!row->group) continue;
            if (!row->entry) {
                d_screen_text(&tui.screen, x, y, limit, row->group->title, title.fg, title.bg, title.attrs);
                continue;
            }
            d_screen_text(&tui.screen, x, y, limit, row->entry->keys, keys.fg, keys.bg, keys.attrs);
            d_screen_text(&tui.screen, x + key_width + HELP_KEY_GAP, y, limit, row->entry->what,
                          box.fg, box.bg, box.attrs);
        }
    }
}

static void draw_viewer(DBuffer *after)
{
    int top = view_top(), height = view_height();
    Look margin = look_for(READER_ROLE_TEXT, 0, false, false);
    for (int y = 0; y < height; ++y) d_screen_fill(&tui.screen, 0, top + y, tui.width, margin.fg, margin.bg, 0);
    const char *url = tui.viewer.urls[tui.viewer.index];
    if (!tui.viewer.loaded) {
        const char *what = tui.viewer.failure ? tui.viewer.failure : "Fetching\xe2\x80\xa6";
        d_screen_text(&tui.screen, 2, top + 1, tui.width - 1, what, margin.fg, margin.bg, 0);
        d_screen_text(&tui.screen, 2, top + 2, tui.width - 1, url, margin.fg, margin.bg, D_SCREEN_DIM);
        return;
    }
    if (tui.graphics == GRAPHICS_NONE) {
        char line[128];
        snprintf(line, sizeof(line), "[image %d \xc3\x97 %d]  This terminal shows no images.",
                 tui.viewer.image.width, tui.viewer.image.height);
        d_screen_text(&tui.screen, 2, top + 1, tui.width - 1, line, margin.fg, margin.bg, 0);
        d_screen_text(&tui.screen, 2, top + 2, tui.width - 1, url, margin.fg, margin.bg, D_SCREEN_DIM);
        return;
    }
    if (tui.viewer.drawn) return;
    /* The pixels go after the cells, at a cursor placed for them, and the
       cell model is told nothing: the image is a layer the next full redraw
       paints over (sixel) or explicitly deletes (kitty). */
    int columns = tui.width - 2, rows = height - 1;
    uint32_t background = themed() ? tui.page.palette.page_bg : 0x000000;
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    if (!d_term_image_fit(&tui.viewer.image, columns * tui.cell_width, rows * tui.cell_height,
                          background, &rgb, &w, &h))
        return;
    int used_columns = (w + tui.cell_width - 1) / tui.cell_width;
    int used_rows = (h + tui.cell_height - 1) / tui.cell_height;
    int x0 = (tui.width - used_columns) / 2, y0 = top + (height - used_rows) / 2;
    dbuf_printf(after, "\033[%d;%dH", y0 + 1, x0 + 1);
    char *bytes = tui.graphics == GRAPHICS_KITTY
        ? d_term_image_kitty(rgb, w, h, used_columns, used_rows, 1)
        : d_term_image_sixel(rgb, w, h);
    dbuf_puts(after, bytes);
    free(bytes);
    free(rgb);
    tui.viewer.drawn = true;
}

static void render(void)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    bool audio = state.phase != MEDIA_AUDIO_STOPPED || state.item[0];
    tui.transport_row = audio ? tui.height - 2 : -1;
    clamp_scroll();
    d_screen_clear(&tui.screen, D_COLOR_DEFAULT, D_COLOR_DEFAULT);
    DBuffer after;
    dbuf_init(&after);
    draw_top();
    if (tui.viewer.active) draw_viewer(&after);
    else draw_page();
    draw_transport();
    draw_status();
    draw_popup();
    draw_help();
    if (tui.prompt.kind == PROMPT_ADDRESS) draw_prompt(0);
    else if (tui.prompt.kind == PROMPT_NONE) tb_hide_cursor();
    DBuffer out;
    dbuf_init(&out);
    d_screen_flush(&tui.screen, &out);
    if (after.length) dbuf_append(&out, after.data, after.length);
    if (out.length) tb_send(out.data, out.length);
    tb_present();
    dbuf_free(&out);
    dbuf_free(&after);
}

/* ---- input --------------------------------------------------------------- */

static void prompt_insert(uint32_t ch)
{
    char bytes[8];
    int n = tb_utf8_unicode_to_char(bytes, ch);
    if (n <= 0) return;
    DBuffer *text = &tui.prompt.text;
    size_t at = tui.prompt.cursor;
    dbuf_reserve(text, (size_t)n);
    memmove(text->data + at + n, text->data + at, text->length - at);
    memcpy(text->data + at, bytes, (size_t)n);
    text->length += (size_t)n;
    text->data[text->length] = '\0';
    tui.prompt.cursor += (size_t)n;
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

static void prompt_delete(size_t from, size_t to)
{
    DBuffer *text = &tui.prompt.text;
    if (to <= from) return;
    memmove(text->data + from, text->data + to, text->length - to);
    text->length -= to - from;
    text->data[text->length] = '\0';
    tui.prompt.cursor = from;
}

static void prompt_commit(bool submit, bool advance)
{
    const char *text = tui.prompt.text.data ? tui.prompt.text.data : "";
    PromptKind kind = tui.prompt.kind;
    if (kind == PROMPT_ADDRESS) {
        bool guessed = false;
        char *url = d_address_resolve(text, &guessed);
        prompt_close();
        if (!url) { set_status("Type an address or a search"); return; }
        navigate(url, NULL, NULL, guessed);
        free(url);
    } else if (kind == PROMPT_FIELD) {
        size_t control = tui.prompt.control;
        free(tui.page.values[control]);
        tui.page.values[control] = d_strdup(text);
        paint(control);
        prompt_close();
        /* Enter in a text field submits its form, with the field as the
           submitter: what the desktop does, and what a browser does. */
        if (submit) submit_form(control);
        else if (advance) move_focus(1);
    } else if (kind == PROMPT_SAVE_AS) {
        char *url = tui.prompt.url ? d_strdup(tui.prompt.url) : NULL;
        char *path = d_strdup(text);
        struct stat info;
        if (!*path) { free(url); free(path); return; }
        if (stat(path, &info) == 0) {
            set_status("%s exists; choose another name", path);
            free(url); free(path);
            return;
        }
        prompt_close();
        if (url) transfer_start(url, path, TRANSFER_DOWNLOAD);
        free(url); free(path);
    }
}

static void prompt_key(const struct tb_event *event)
{
    DBuffer *text = &tui.prompt.text;
    const char *data = text->data ? text->data : "";
    switch (event->key) {
    case TB_KEY_ESC: prompt_close(); return;
    case TB_KEY_ENTER: prompt_commit(true, false); return;
    case TB_KEY_TAB: prompt_commit(false, true); return;
    case TB_KEY_BACKSPACE:
    case TB_KEY_BACKSPACE2:
        prompt_delete(previous_char(data, tui.prompt.cursor), tui.prompt.cursor);
        return;
    case TB_KEY_DELETE:
        prompt_delete(tui.prompt.cursor, next_char(data, text->length, tui.prompt.cursor));
        return;
    case TB_KEY_ARROW_LEFT: tui.prompt.cursor = previous_char(data, tui.prompt.cursor); return;
    case TB_KEY_ARROW_RIGHT: tui.prompt.cursor = next_char(data, text->length, tui.prompt.cursor); return;
    case TB_KEY_HOME:
    case TB_KEY_CTRL_A: tui.prompt.cursor = 0; return;
    case TB_KEY_END:
    case TB_KEY_CTRL_E: tui.prompt.cursor = text->length; return;
    case TB_KEY_CTRL_U: prompt_delete(0, tui.prompt.cursor); return;
    case TB_KEY_CTRL_W: {
        size_t at = tui.prompt.cursor;
        while (at && data[at - 1] == ' ') --at;
        while (at && data[at - 1] != ' ') --at;
        prompt_delete(at, tui.prompt.cursor);
        return;
    }
    default: break;
    }
    if (event->ch) prompt_insert(event->ch);
}

static void popup_key(const struct tb_event *event)
{
    if (event->key == TB_KEY_ESC) popup_close(false);
    else if (event->key == TB_KEY_ENTER || event->ch == ' ') popup_close(true);
    else if ((event->key == TB_KEY_ARROW_UP || event->ch == 'k') && tui.popup.selected) --tui.popup.selected;
    else if ((event->key == TB_KEY_ARROW_DOWN || event->ch == 'j') && tui.popup.selected + 1 < tui.popup.count)
        ++tui.popup.selected;
}

static bool audio_loaded(void)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    return state.phase != MEDIA_AUDIO_STOPPED || state.item[0];
}

static void seek_relative(double seconds)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    if (!state.seekable) { set_status("This track cannot seek"); return; }
    double target = state.position_seconds + seconds;
    if (target < 0) target = 0;
    if (state.duration_seconds > 0 && target > state.duration_seconds) target = state.duration_seconds;
    media_audio_seek(target);
}

static bool audio_key(const struct tb_event *event)
{
    if (!audio_loaded()) return false;
    MediaAudioState state;
    media_audio_snapshot(&state);
    if (event->ch == ' ') {
        if (state.phase == MEDIA_AUDIO_PLAYING) media_audio_pause(); else media_audio_play();
    } else if (event->ch == 's') {
        media_audio_stop();
    } else if (event->key == TB_KEY_ARROW_LEFT) {
        seek_relative(-5);
    } else if (event->key == TB_KEY_ARROW_RIGHT) {
        seek_relative(5);
    } else if (event->ch == '[' || event->ch == ']') {
        float volume = state.volume + (event->ch == ']' ? 0.1f : -0.1f);
        media_audio_set_volume(volume < 0 ? 0 : volume > 1 ? 1 : volume);
    } else if (event->ch == 'x') {
        audio_release();
        set_status("Closed the track");
    } else {
        return false;
    }
    return true;
}

static ReaderLayoutTable *visible_table(void)
{
    ReaderLayout *layout = tui.page.layout;
    for (size_t i = 0; layout && i < layout->table_count; ++i) {
        ReaderLayoutTable *table = &layout->tables[i];
        if (table->full_width <= table->visible) continue;
        if (table->row + table->rows > tui.page.scroll && table->row < tui.page.scroll + view_height())
            return table;
    }
    return NULL;
}

static void scroll_table(int direction)
{
    ReaderLayoutTable *table = visible_table();
    if (!table) return;
    int step = table->visible / 2 > 4 ? table->visible / 2 : 4;
    reader_layout_scroll_table(tui.page.layout, (size_t)(table - tui.page.layout->tables),
                               table->offset + direction * step);
}

static void reload(void)
{
    const DAppPage *page = d_app_history_current(&tui.history);
    if (page) start_navigation(page->url, NULL, NULL, false, NAV_RELOAD);
}

static void page_key(const struct tb_event *event)
{
    int page = view_height() - 1 > 1 ? view_height() - 1 : 1;
    if (event->mod & TB_MOD_ALT) {
        if (event->ch >= '1' && event->ch <= '9' && tui.page.layout) {
            size_t index = event->ch - '1';
            if (index < tui.page.layout->anchor_count) jump_to_section(tui.page.layout->anchors[index].id);
        } else if (event->key == TB_KEY_ARROW_LEFT) {
            if (d_app_history_back(&tui.history)) show_history_current();
        } else if (event->key == TB_KEY_ARROW_RIGHT) {
            if (d_app_history_forward(&tui.history)) show_history_current();
        }
        return;
    }
    if (audio_key(event)) return;
    switch (event->key) {
    case TB_KEY_ARROW_DOWN: scroll_by(1); return;
    case TB_KEY_ARROW_UP: scroll_by(-1); return;
    case TB_KEY_PGDN:
    case TB_KEY_CTRL_F: scroll_by(page); return;
    case TB_KEY_PGUP:
    case TB_KEY_CTRL_B: scroll_by(-page); return;
    case TB_KEY_HOME: tui.page.scroll = 0; return;
    case TB_KEY_END: tui.page.scroll = INT_MAX / 2; clamp_scroll(); return;
    case TB_KEY_TAB: move_focus(1); return;
    case TB_KEY_BACK_TAB: move_focus(-1); return;
    case TB_KEY_ENTER: activate_focus(); return;
    case TB_KEY_ARROW_LEFT: scroll_table(-1); return;
    case TB_KEY_ARROW_RIGHT: scroll_table(1); return;
    case TB_KEY_BACKSPACE:
    case TB_KEY_BACKSPACE2:
        if (d_app_history_back(&tui.history)) show_history_current();
        return;
    case TB_KEY_CTRL_L:
    case TB_KEY_F6:
        prompt_open(PROMPT_ADDRESS, "Go to: ", tui.page.url ? tui.page.url : "", false);
        return;
    case TB_KEY_F5: reload(); return;
    case TB_KEY_ESC:
        if (tui.loading) nav_cancel();
        else if (tui.transfer) transfer_cancel();
        else tui.page.focus = -1;
        return;
    default: break;
    }
    switch (event->ch) {
    case 'j': scroll_by(1); break;
    case 'k': scroll_by(-1); break;
    case ' ': scroll_by(page); break;
    case 'b': scroll_by(-page); break;
    case 'g': tui.page.scroll = 0; break;
    case 'G': tui.page.scroll = INT_MAX / 2; clamp_scroll(); break;
    case 'o': prompt_open(PROMPT_ADDRESS, "Go to: ", "", false); break;
    case 'H': if (d_app_history_back(&tui.history)) show_history_current(); break;
    case 'L': if (d_app_history_forward(&tui.history)) show_history_current(); break;
    case 'r': reload(); break;
    case '<': scroll_table(-1); break;
    case '>': scroll_table(1); break;
    case 't': {
        const char *next = !strcmp(tui.theme, "warm") ? "night" : !strcmp(tui.theme, "night") ? "paper" : "warm";
        free(tui.theme);
        tui.theme = d_strdup(next);
        save_theme();
        set_status("Theme: %s", tui.theme);
        reload();
        break;
    }
    case '?': tui.help = true; tui.help_scroll = 0; break;
    case 'q': tui.quit = true; break;
    default: break;
    }
}

static void viewer_key(const struct tb_event *event)
{
    if (event->key == TB_KEY_ESC || event->key == TB_KEY_BACKSPACE || event->key == TB_KEY_BACKSPACE2 ||
        event->ch == 'q')
        viewer_leave();
    else if (event->key == TB_KEY_ARROW_LEFT || event->ch == 'p') viewer_step(-1);
    else if (event->key == TB_KEY_ARROW_RIGHT || event->ch == 'n') viewer_step(1);
    else if (event->ch == 'd') download(tui.viewer.urls[tui.viewer.index]);
}

/* The page cell under a screen position, as link and control indices. */
static void hit(int x, int y, int *link, int *control)
{
    *link = *control = -1;
    ReaderLayout *layout = tui.page.layout;
    if (!layout || tui.viewer.active || y < view_top() || y >= view_top() + view_height()) return;
    int row = tui.page.scroll + y - view_top();
    if (row >= layout->rows || x >= layout->columns) return;
    ReaderCell *cell = reader_layout_cell(layout, row, x);
    while (!cell->width && x > 0) cell = reader_layout_cell(layout, row, --x);
    *link = cell->link;
    *control = cell->control;
}

static void focus_on(FocusKind kind, size_t index)
{
    for (size_t i = 0; i < tui.page.focus_count; ++i)
        if (tui.page.focusables[i].kind == kind && tui.page.focusables[i].index == index)
            tui.page.focus = (int)i;
}

static void seek_to_column(int x)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    if (!state.seekable || state.duration_seconds <= 0 || tui.bar_end <= tui.bar_start + 1) return;
    double fraction = (double)(x - tui.bar_start) / (tui.bar_end - tui.bar_start - 1);
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    media_audio_seek(fraction * state.duration_seconds);
}

static void mouse(const struct tb_event *event)
{
    if (event->key == TB_KEY_MOUSE_WHEEL_UP) { if (!tui.viewer.active) scroll_by(-3); return; }
    if (event->key == TB_KEY_MOUSE_WHEEL_DOWN) { if (!tui.viewer.active) scroll_by(3); return; }
    if (event->key == TB_KEY_MOUSE_RELEASE) { tui.seeking = false; return; }
    if (event->key != TB_KEY_MOUSE_LEFT) return;
    /* The position bar: press to jump, drag to scrub. */
    if (tui.transport_row >= 0 && (event->y == tui.transport_row || tui.seeking) &&
        (tui.seeking || (event->x >= tui.bar_start && event->x < tui.bar_end))) {
        tui.seeking = true;
        seek_to_column(event->x);
        return;
    }
    if (event->mod & TB_MOD_MOTION) return;
    /* A click away from an open list dismisses it; choosing is by key. */
    if (tui.popup_open) { popup_close(false); return; }
    if (event->y == 0 && !tui.prompt.kind) {
        if (event->x <= 2) { if (d_app_history_back(&tui.history)) show_history_current(); }
        else if (event->x <= 4) { if (d_app_history_forward(&tui.history)) show_history_current(); }
        else prompt_open(PROMPT_ADDRESS, "Go to: ", tui.page.url ? tui.page.url : "", false);
        return;
    }
    int link, control;
    hit(event->x, event->y, &link, &control);
    if (control >= 0) { focus_on(FOCUS_CONTROL, (size_t)control); activate_control((size_t)control); }
    else if (link >= 0) { focus_on(FOCUS_LINK, (size_t)link); follow_link((size_t)link); }
}

/* The key list scrolls when it does not fit; any other key closes it. */
static void help_key(const struct tb_event *event)
{
    if (!tui.help_most) tui.help = false;
    else if (event->key == TB_KEY_ARROW_DOWN || event->ch == 'j') ++tui.help_scroll;
    else if (event->key == TB_KEY_ARROW_UP || event->ch == 'k') --tui.help_scroll;
    else if (event->key == TB_KEY_PGDN || event->ch == ' ') tui.help_scroll += view_height() / 2;
    else if (event->key == TB_KEY_PGUP || event->ch == 'b') tui.help_scroll -= view_height() / 2;
    else tui.help = false;
}

static void handle(const struct tb_event *event)
{
    if (event->type == TB_EVENT_RESIZE) {
        tui.width = tb_width();
        tui.height = tb_height();
        cell_pixels();
        d_screen_resize(&tui.screen, tui.width, tui.height);
        if (tui.has_page) relayout(&tui.page);
        tui.viewer.drawn = false;
        return;
    }
    if (event->type == TB_EVENT_MOUSE) { mouse(event); return; }
    if (event->type != TB_EVENT_KEY) return;
    if (event->key == TB_KEY_CTRL_C || event->key == TB_KEY_CTRL_Q) { tui.quit = true; return; }
    if (tui.help) { help_key(event); return; }
    if (tui.prompt.kind) { prompt_key(event); return; }
    if (tui.popup_open) { popup_key(event); return; }
    if (tui.viewer.active) { viewer_key(event); return; }
    page_key(event);
}

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [URL]\n"
            "  Distill in a terminal. With no URL, opens the start page from settings.\n"
            "  DISTILL_TUI_GRAPHICS=kitty|sixel|none overrides the image probe;\n"
            "  DISTILL_TUI_COLORS=16|256|true overrides colour detection.\n", program);
}

int main(int argc, char **argv)
{
    const char *url_argument = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        if (argv[i][0] == '-') { usage(argv[0]); return 2; }
        url_argument = argv[i];
    }
    tui.transport_row = -1;
    tui.page.focus = -1;
    tui.nav = (Navigation){.out_fd = -1, .err_fd = -1};
    d_app_history_init(&tui.history);
    char *start = NULL;
    load_settings(&start);
    tui.engine = engine_path();
    /* Cookies for this run only, in a file only this user can read: the
       engine runs once per navigation, so a session has to live on disk. */
    const char *outer = getenv("TMPDIR");
    snprintf(run_directory, sizeof(run_directory), "%s/distill-tui-XXXXXX", outer && *outer ? outer : "/tmp");
    if (!mkdtemp(run_directory)) { fprintf(stderr, "distill-tui: no private directory: %s\n", strerror(errno)); return 1; }
    setenv("TMPDIR", run_directory, 1);
    tui.session_file = private_temp("distill-session");
    if (tui.session_file) snprintf(session_path, sizeof(session_path), "%s", tui.session_file);
    if (!terminal_open()) { free(start); return 1; }

    if (url_argument) {
        bool guessed = false;
        char *url = d_address_resolve(url_argument, &guessed);
        if (url) navigate(url, NULL, NULL, guessed);
        free(url);
    } else if (start) {
        navigate(start, NULL, NULL, false);
    } else {
        set_status("o goes to an address \xe2\x80\x94 ? for keys");
    }
    free(start);

    int tty_fd = -1, resize_fd = -1;
    tb_get_fds(&tty_fd, &resize_fd);
    while (!tui.quit) {
        render();
        struct pollfd fds[5];
        int count = 0;
        fds[count++] = (struct pollfd){tty_fd, POLLIN, 0};
        fds[count++] = (struct pollfd){resize_fd, POLLIN, 0};
        int nav_out = -1, nav_err = -1, transfer = -1;
        if (tui.loading && tui.nav.out_fd >= 0) { nav_out = count; fds[count++] = (struct pollfd){tui.nav.out_fd, POLLIN, 0}; }
        if (tui.loading && tui.nav.err_fd >= 0) { nav_err = count; fds[count++] = (struct pollfd){tui.nav.err_fd, POLLIN, 0}; }
        if (tui.transfer) { transfer = count; fds[count++] = (struct pollfd){d_media_transfer_fd(tui.transfer), POLLIN, 0}; }
        MediaAudioState state;
        media_audio_snapshot(&state);
        int timeout = state.phase == MEDIA_AUDIO_PLAYING ? 250 : tui.external_count ? 1000 : -1;
        int ready = poll(fds, (nfds_t)count, timeout);
        if (ready < 0 && errno != EINTR) break;
        if ((nav_out >= 0 && fds[nav_out].revents) || (nav_err >= 0 && fds[nav_err].revents)) nav_service();
        if (transfer >= 0 && fds[transfer].revents) transfer_service();
        if (fds[0].revents || fds[1].revents) {
            /* A terminal sends Alt+key as Escape and the key in one write, and
               a pressed Escape on its own: an Escape with a key right behind
               it in the same batch is the Alt chord. */
            struct tb_event event, next;
            while (!tui.quit && tb_peek_event(&event, 0) == TB_OK) {
                if (event.type == TB_EVENT_KEY && event.key == TB_KEY_ESC &&
                    tb_peek_event(&next, 0) == TB_OK) {
                    if (next.type == TB_EVENT_KEY) { next.mod |= TB_MOD_ALT; handle(&next); }
                    else { handle(&event); handle(&next); }
                    continue;
                }
                handle(&event);
            }
        }
        reap_externals();
    }

    if (tui.loading) nav_cancel();
    if (tui.transfer) transfer_cancel();
    audio_release();
    viewer_free_image();
    terminal_close();
    page_free_state(&tui.page);
    d_app_history_clear(&tui.history, dispose_history_page);
    if (tui.session_file) { unlink(tui.session_file); free(tui.session_file); }
    session_path[0] = '\0';
    /* Empty by now if nothing leaked; a failure here is a leak to find. */
    if (rmdir(run_directory) != 0)
        fprintf(stderr, "distill-tui: %s was not empty on exit\n", run_directory);
    free(tui.engine);
    free(tui.theme);
    free(tui.status);
    return 0;
}
