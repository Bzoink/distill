/* Distill on a framebuffer.

   The same process model as the other shells: the engine runs once per
   navigation and writes the reader page to a file. This lays it out with
   reader_layout (page.h), paints it with the software painter, draws its own
   chrome and form controls over it with the same calls, and hands the pixels
   to a display sink (ddisplay.h) -- the console's framebuffer, a window, or
   the memory sink the gates drive. It never knows which.

   Everything a shell shares with the others comes from the same code: the
   engine run and its progress (shell_nav.h), line editing (line_edit.h), the
   form rules, the address line, settings, history, audio and transfers. So a
   form submitted here sends what the desktop and the terminal send. */

#include "address.h"
#include "app_state.h"
#include "cutil.h"
#include "datadir.h"
#include "dclock.h"
#include "ddisplay.h"
#include "dfs.h"
#include "distill_version.h"
#include "dproc.h"
#include "engine_request.h"
#include "form_submit.h"
#include "image.h"
#include "line_edit.h"
#include "measure.h"
#include "media_audio.h"
#include "media_transfer.h"
#include "page.h"
#include "render.h"
#include "settings.h"
#include "shell_nav.h"

#include <limits.h>
#include <math.h>

#define SECTION_SCHEME "distill-section:"
#define MEDIA_SCHEME "distill-media:"
#define PLAY_SCHEME "distill-play:"
#define DOWNLOAD_SCHEME "distill-download:"
#define OPEN_SCHEME "distill-open:"

enum { BUTTONS_MAX = 48, EXTERNALS_MAX = 16, HANDLES_MAX = 16 };

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    DRenderColor page_bg, text, main_bg, heading, card_bg, border, link, pre_bg, muted;
} Palette;

typedef enum { FOCUS_LINK, FOCUS_CONTROL } FocusKind;
typedef struct {
    FocusKind kind;
    size_t index;
    int x, y;
} Focus;

typedef struct {
    char *url;
    char *html;
    char *title;
    DPage *doc;
    int doc_width, doc_surface;   /* what the document was laid out for */
    int height;
    int body_px;                  /* the reader's type size, for the controls drawn over it */
    DLinkBox *links;
    size_t link_count;
    DFormBox *controls;
    size_t control_count;
    char **values;                /* what each control holds now */
    bool *checked;
    Focus *focusables;
    size_t focus_count;
    int focus;                    /* index into focusables, or -1 */
    int scroll;
    Palette palette;
} Page;

typedef enum { PROMPT_NONE, PROMPT_ADDRESS, PROMPT_SAVE_AS, PROMPT_EXPORT_HTML, PROMPT_EXPORT_MARKDOWN } PromptKind;

typedef enum {
    ACTION_NONE, ACTION_BACK, ACTION_FORWARD, ACTION_RELOAD, ACTION_ADDRESS, ACTION_MENU,
    ACTION_MENU_ITEM, ACTION_HELP, ACTION_PLAY_PAUSE, ACTION_CLOSE_AUDIO,
    ACTION_SEEK, ACTION_VIEWER_PREVIOUS, ACTION_VIEWER_NEXT, ACTION_VIEWER_SAVE,
    ACTION_VIEWER_CLOSE, ACTION_POPUP_OPTION, ACTION_SCROLLBAR,
} Action;

/* The desktop's two menu buttons and their submenus, in its order. */
typedef enum {
    MENU_NONE, MENU_SECTIONS, MENU_SETTINGS, MENU_EXPORT, MENU_THEME, MENU_ZOOM, MENU_START,
} MenuId;

typedef enum {
    ITEM_SUBMENU, ITEM_SECTION, ITEM_OPEN_EXTERNAL, ITEM_EXPORT, ITEM_THEME, ITEM_ZOOM, ITEM_START,
} ItemKind;

enum { MENU_ITEMS_MAX = 8 };

typedef struct {
    const char *label;
    const char *shortcut;
    ItemKind kind;
    int arg;
    bool enabled;
    int radio;                    /* -1 for none, else whether it is the chosen one */
} MenuItem;

/* One open list: the top menu is level 0, its open submenu level 1. */
typedef struct {
    MenuId id;
    int selected;                 /* -1 for none */
    DRenderRect rect;             /* where it was last drawn */
    DRenderRect rows[MENU_ITEMS_MAX];
    size_t count;
} MenuLevel;

typedef struct {
    DRenderRect rect;
    Action action;
    int arg;
} Button;

typedef struct {
    size_t control;
    char **values;
    char **labels;
    size_t count;
    size_t selected;
} Popup;

typedef struct {
    bool active;
    char **urls;
    size_t count;
    size_t index;
    DImage image;
    bool loaded;
    char *file;
    char *failure;
    double zoom;                  /* 1 fits the image to the view */
    int pan_x, pan_y;
    DRenderContext *layer;        /* the image drawn once, composited per frame */
    bool layer_valid;
} Viewer;

typedef struct {
    DRenderFont *font;
    DRenderFontMetrics metrics;
} Font;

typedef enum { TRANSFER_AUDIO, TRANSFER_DOWNLOAD, TRANSFER_IMAGE } TransferKind;
typedef enum { DRAG_NONE, DRAG_SCROLLBAR, DRAG_SEEK } Drag;

static struct {
    DDisplay *display;
    DDisplayMode mode;
    DRenderContext *screen;
    int screen_width, screen_height;
    const char *font_dir;
    Font ui, bold, symbol, control, control_bold;
    int ui_px, control_px;
    int bar_height, status_height, transport_height, scrollbar_width;

    Page page;
    bool has_page;
    DAppHistory history;
    char *theme;
    int zoom;
    char *engine;
    char *session_file;
    DShellNav nav;

    PromptKind prompt;
    char prompt_label[64];
    DLineEdit prompt_edit;
    char *prompt_url;
    /* A text control with focus is being edited in place. */
    bool editing;
    DLineEdit field;
    size_t field_control;

    bool popup_open;
    Popup popup;
    MenuLevel menu[2];            /* menu[0].id is MENU_NONE when no menu is open */
    bool in_submenu;              /* the keys move in menu[1] */
    DRenderRect sections_button, settings_button;
    bool help;
    Viewer viewer;

    DMediaTransfer *transfer;
    TransferKind transfer_kind;
    char *audio_file;
    char *audio_title;

    DProc *externals[EXTERNALS_MAX];
    size_t external_count;

    char *status;
    char hover[2048];
    Button buttons[BUTTONS_MAX];
    size_t button_count;
    Drag drag;
    int drag_offset;
    DRenderRect seek_track;
    int pointer_x, pointer_y;
    bool dirty, page_dirty, suspended, quit;
} fb;

static void set_status(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    free(fb.status);
    fb.status = d_strdup(buffer);
    fb.dirty = true;
}

/* Everything this run writes goes in one private directory, removed on a
   clean exit; a crash can still remove the live cookies, by a path fixed in
   advance, and put the console back. */
static char run_directory[D_FS_PATH_MAX];
static char session_path[D_FS_PATH_MAX];

static void emergency_restore(void)
{
    if (session_path[0]) d_fs_remove(session_path);
    d_display_emergency_restore();
}

/* ---- colours and type ----------------------------------------------------- */

static DRenderColor rgb(uint32_t value)
{
    return (DRenderColor){(uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value, 255};
}

/* The reader's colours, read from the page the engine wrote: the theme is
   the page's, and a second table here would drift from it. */
static DRenderColor css_color(const char *html, const char *name, uint32_t fallback)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "--%s:#", name);
    const char *at = html ? strstr(html, needle) : NULL;
    if (!at) return rgb(fallback);
    at += strlen(needle);
    uint32_t value = 0;
    for (int i = 0; i < 6; ++i) {
        char c = at[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return rgb(fallback);
        value = value << 4 | (uint32_t)digit;
    }
    return rgb(value);
}

static Palette palette_of(const char *html)
{
    return (Palette){
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
}

static const Palette *palette(void)
{
    static Palette fallback;
    static bool made;
    if (fb.has_page) return &fb.page.palette;
    if (!made) { fallback = palette_of(NULL); made = true; }
    return &fallback;
}

static DRenderColor with_alpha(DRenderColor color, uint8_t alpha)
{
    color.alpha = alpha;
    return color;
}

static void font_free(Font *font)
{
    if (font->font) d_render_font_destroy(font->font);
    *font = (Font){0};
}

static void font_make(Font *font, const char *face, int px, int weight)
{
    font_free(font);
    font->font = d_render_font_create(fb.screen, face, px, weight, false, 0, &font->metrics);
}

static void make_fonts(void)
{
    font_make(&fb.ui, "Source Sans 3", fb.ui_px, 400);
    font_make(&fb.bold, "Source Sans 3", fb.ui_px, 700);
    font_make(&fb.symbol, "DejaVu Sans", fb.ui_px, 400);
    fb.control_px = 0;
}

static void make_control_fonts(int px)
{
    if (px == fb.control_px) return;
    fb.control_px = px;
    font_make(&fb.control, "Source Sans 3", px, 400);
    font_make(&fb.control_bold, "Source Sans 3", px, 700);
}

static int text_width(const Font *font, const char *text)
{
    return font->font && text && *text ? d_render_text_width(font->font, text) : 0;
}

/* Text with its box vertically centred on `middle`; returns where it ends. */
static int draw_text(DRenderContext *context, const Font *font, int x, int middle, const char *text,
                     DRenderColor color)
{
    if (!font->font || !text || !*text) return x;
    int width = d_render_text_width(font->font, text);
    DRenderRect rect = {x, middle - font->metrics.height / 2, width, font->metrics.height};
    d_render_draw_text(context, text, font->font, color, rect);
    return x + width;
}

static void add_button(DRenderRect rect, Action action, int arg)
{
    if (fb.button_count < BUTTONS_MAX) fb.buttons[fb.button_count++] = (Button){rect, action, arg};
}

static bool inside(DRenderRect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

/* ---- the frame ------------------------------------------------------------ */

static bool audio_loaded(void)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    return state.phase != MEDIA_AUDIO_STOPPED || state.item[0];
}

static int view_top(void) { return fb.bar_height; }

static int view_height(void)
{
    int height = fb.mode.height - fb.bar_height - fb.status_height - (audio_loaded() ? fb.transport_height : 0);
    return height > 1 ? height : 1;
}

/* The tallest the view can be: the page's paint surface, so the transport
   appearing and going needs no new layout. */
static int surface_height(void)
{
    int height = fb.mode.height - fb.bar_height - fb.status_height;
    return height > 1 ? height : 1;
}

static int line_step(void)
{
    int px = fb.page.body_px > 0 ? fb.page.body_px : 18;
    return px * 3 / 2;
}

static void metrics_for_mode(void)
{
    int px = fb.mode.height / 56;
    fb.ui_px = px < 13 ? 13 : px > 28 ? 28 : px;
    fb.bar_height = fb.ui_px * 12 / 5;
    fb.status_height = fb.ui_px * 9 / 5;
    fb.transport_height = fb.ui_px * 11 / 5;
    fb.scrollbar_width = fb.ui_px / 2 > 8 ? fb.ui_px / 2 : 8;
}

/* ---- the page ------------------------------------------------------------- */

static int max_scroll(void)
{
    int most = fb.page.height - view_height();
    return most > 0 ? most : 0;
}

static void clamp_scroll(void)
{
    if (fb.page.scroll > max_scroll()) fb.page.scroll = max_scroll();
    if (fb.page.scroll < 0) fb.page.scroll = 0;
}

static void scroll_to(int y)
{
    int before = fb.page.scroll;
    fb.page.scroll = y;
    clamp_scroll();
    if (fb.page.scroll != before) fb.page_dirty = true;
    fb.dirty = true;
}

static void scroll_by(int pixels)
{
    scroll_to(fb.page.scroll + pixels);
}

static void reveal(int y, int height)
{
    int view = view_height(), margin = line_step();
    if (y < fb.page.scroll) scroll_to(y - margin);
    else if (y + height > fb.page.scroll + view) scroll_to(y + height - view + margin);
}

static char *unescape_title(const char *text, size_t length)
{
    DBuffer out;
    dbuf_init(&out);
    static const struct { const char *entity; const char *text; } entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"},
    };
    for (size_t i = 0; i < length;) {
        bool replaced = false;
        for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]) && !replaced; ++e) {
            size_t n = strlen(entities[e].entity);
            if (i + n <= length && !strncmp(text + i, entities[e].entity, n)) {
                dbuf_puts(&out, entities[e].text);
                i += n;
                replaced = true;
            }
        }
        if (!replaced) {
            char c = text[i++];
            dbuf_append(&out, (c == '\n' || c == '\t') ? " " : &c, 1);
        }
    }
    return dbuf_take(&out);
}

static char *title_of(const char *html)
{
    const char *open = strstr(html, "<title>");
    const char *close = open ? strstr(open, "</title>") : NULL;
    if (!close) return d_strdup("");
    open += 7;
    return unescape_title(open, (size_t)(close - open));
}

static int body_px_of(const char *html)
{
    const char *body = strstr(html, "body{font-family");
    const char *size = body ? strstr(body, "font-size:") : NULL;
    int px = size ? (int)strtol(size + 10, NULL, 10) : 0;
    return px >= 8 && px <= 96 ? px : 18;
}

static bool control_type(const DFormBox *box, const char *type)
{
    return !d_casecmp(box->type, type);
}

static bool control_is_button(const DFormBox *box)
{
    return !strcmp(box->tag, "button") || control_type(box, "submit") || control_type(box, "reset") ||
           control_type(box, "button") || control_type(box, "image");
}

static bool control_is_choice(const DFormBox *box)
{
    return control_type(box, "checkbox") || control_type(box, "radio");
}

static bool control_is_text(const DFormBox *box)
{
    if (!strcmp(box->tag, "textarea")) return true;
    if (strcmp(box->tag, "input")) return false;
    return !control_is_choice(box) && !control_is_button(box) && !control_type(box, "hidden") &&
           !control_type(box, "file");
}

static bool control_visible(const DFormBox *box)
{
    return box->width > 0 && box->height > 0 && !control_type(box, "hidden");
}

static int compare_focus(const void *a, const void *b)
{
    const Focus *x = a, *y = b;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->x < y->x ? -1 : x->x > y->x;
}

static void build_focus(Page *page)
{
    free(page->focusables);
    page->focusables = NULL;
    page->focus_count = 0;
    size_t capacity = page->link_count + page->control_count;
    if (!capacity) return;
    page->focusables = d_xmalloc(capacity * sizeof(*page->focusables));
    /* A link too small to see is a marker -- the reader template's section
       anchors are 1px -- not somewhere to stop. */
    for (size_t i = 0; i < page->link_count; ++i)
        if (page->links[i].width > 2 && page->links[i].height > 2)
            page->focusables[page->focus_count++] = (Focus){FOCUS_LINK, i, page->links[i].x, page->links[i].y};
    for (size_t i = 0; i < page->control_count; ++i)
        if (control_visible(&page->controls[i]) && !page->controls[i].disabled)
            page->focusables[page->focus_count++] = (Focus){FOCUS_CONTROL, i, page->controls[i].x,
                                                            page->controls[i].y};
    qsort(page->focusables, page->focus_count, sizeof(*page->focusables), compare_focus);
}

/* Lay the page out at the display's width. Control state survives a relayout
   -- a resize must not throw away what the reader typed -- and so does the
   focus, which is on a link or a control, not a position. */
static bool layout_page(Page *page)
{
    int width = fb.mode.width, height = surface_height();
    DPage *doc = d_page_create(page->html, width, height, fb.font_dir);
    if (!doc) return false;
    int old_height = page->height;
    bool had_focus = page->focus >= 0 && (size_t)page->focus < page->focus_count;
    Focus kept = had_focus ? page->focusables[page->focus] : (Focus){0};
    if (page->doc) d_page_destroy(page->doc);
    page->doc = doc;
    page->doc_width = width;
    page->doc_surface = height;
    page->height = d_page_height(doc);
    free(page->links);
    page->link_count = d_page_link_boxes(doc, NULL, 0);
    page->links = d_xcalloc(page->link_count ? page->link_count : 1, sizeof(*page->links));
    d_page_link_boxes(doc, page->links, page->link_count);
    size_t controls = d_page_form_boxes(doc, NULL, 0);
    DFormBox *boxes = d_xcalloc(controls ? controls : 1, sizeof(*boxes));
    d_page_form_boxes(doc, boxes, controls);
    if (!page->values || controls != page->control_count) {
        for (size_t i = 0; page->values && i < page->control_count; ++i) free(page->values[i]);
        free(page->values);
        free(page->checked);
        page->values = d_xcalloc(controls + 1, sizeof(*page->values));
        page->checked = d_xcalloc(controls + 1, sizeof(*page->checked));
        for (size_t i = 0; i < controls; ++i) {
            page->values[i] = d_strdup(boxes[i].value);
            page->checked[i] = boxes[i].checked;
        }
    }
    free(page->controls);
    page->controls = boxes;
    page->control_count = controls;
    if (old_height > 0) page->scroll = (int)((long long)page->scroll * page->height / old_height);
    build_focus(page);
    page->focus = -1;
    for (size_t i = 0; had_focus && i < page->focus_count; ++i)
        if (page->focusables[i].kind == kept.kind && page->focusables[i].index == kept.index)
            page->focus = (int)i;
    fb.page_dirty = fb.dirty = true;
    return true;
}

static void page_free(Page *page)
{
    if (page->doc) d_page_destroy(page->doc);
    for (size_t i = 0; page->values && i < page->control_count; ++i) free(page->values[i]);
    free(page->values);
    free(page->checked);
    free(page->links);
    free(page->controls);
    free(page->focusables);
    free(page->html);
    free(page->title);
    free(page->url);
    *page = (Page){.focus = -1};
}

static void stop_editing(void);

static bool show_page_file(const char *url, const char *template_path)
{
    char *html = d_read_file(template_path, NULL);
    if (!html) return false;
    stop_editing();
    page_free(&fb.page);
    fb.page.url = d_strdup(url);
    fb.page.html = html;
    fb.page.title = title_of(html);
    fb.page.palette = palette_of(html);
    fb.page.body_px = body_px_of(html);
    fb.page.focus = -1;
    fb.has_page = layout_page(&fb.page);
    fb.page.scroll = 0;
    fb.hover[0] = '\0';
    return fb.has_page;
}

/* ---- the engine, once per navigation -------------------------------------- */

static void dispose_history_page(DAppPage *page)
{
    if (page->template_path) d_fs_remove(page->template_path);
}

static void nav_cancel(void)
{
    if (!fb.nav.active) return;
    d_nav_cancel(&fb.nav);
    set_status("Stopped");
}

static void start_navigation(const char *url, const char *method, const char *body,
                             bool https_guessed, DNavKind kind)
{
    DNavOptions options = {.engine = fb.engine, .theme = fb.theme, .zoom = fb.zoom,
                           .session_file = fb.session_file, .width = fb.mode.width,
                           .native_forms = true};
    char *error = NULL;
    if (!d_nav_start(&fb.nav, &options, url, method, body, https_guessed, kind, &error)) {
        set_status("%s", error);
        free(error);
        return;
    }
    set_status("Loading %s", url);
}

static void navigate(const char *url, const char *method, const char *body, bool https_guessed)
{
    if (!d_page_uri_allowed(url)) {
        set_status("Blocked: not an address a page can be loaded from");
        return;
    }
    start_navigation(url, method, body, https_guessed, D_NAV_NEW);
}

static void nav_service(void)
{
    if (!fb.nav.active) return;
    DNavOutcome outcome = d_nav_service(&fb.nav);
    fb.dirty = true;
    if (outcome == D_NAV_RUNNING) return;
    if (outcome == D_NAV_LOADED || outcome == D_NAV_CHALLENGE) {
        char *url = d_strdup(d_nav_final_url(&fb.nav));
        char *template_path = d_nav_take_template(&fb.nav);
        bool ok = fb.nav.kind == D_NAV_RELOAD
            ? d_app_history_replace_current(&fb.history, url, template_path, fb.theme, fb.zoom,
                                            dispose_history_page)
            : d_app_history_push(&fb.history, url, template_path, fb.theme, fb.zoom,
                                 dispose_history_page);
        if (ok && show_page_file(url, template_path)) {
            if (outcome == D_NAV_CHALLENGE)
                set_status("This site answered with a bot challenge; Distill shows what it sent and does not solve those");
            else
                set_status("%s", *fb.page.title ? fb.page.title : "Ready");
        } else {
            set_status("The reader page could not be read");
        }
        free(template_path);
        free(url);
    } else {
        char *why = d_nav_failure(&fb.nav);
        set_status("Could not load %s: %s", fb.nav.url, why);
        free(why);
    }
    d_nav_cleanup(&fb.nav);
}

static void show_history_current(void)
{
    const DAppPage *page = d_app_history_current(&fb.history);
    if (page && show_page_file(page->url, page->template_path)) set_status("%s", page->url);
}

static void go_back(void)
{
    if (d_app_history_back(&fb.history)) show_history_current();
}

static void go_forward(void)
{
    if (d_app_history_forward(&fb.history)) show_history_current();
}

static void reload(void)
{
    const DAppPage *page = d_app_history_current(&fb.history);
    if (page) start_navigation(page->url, NULL, NULL, false, D_NAV_RELOAD);
}

/* ---- settings ------------------------------------------------------------- */

static const char *const theme_ids[] = {"warm", "night", "paper"};
static const char *const theme_labels[] = {"Warm (Light)", "Night (Dark)", "Paper (Light)"};
enum { THEME_COUNT = sizeof(theme_ids) / sizeof(theme_ids[0]) };

/* The desktop's levels, so a zoom saved by one shell is valid in the other. */
static const int zoom_levels[] = {75, 90, 100, 110, 125, 150, 175, 200};
static const char *const zoom_labels[] = {"75%", "90%", "100%", "110%", "125%", "150%", "175%", "200%"};
enum { ZOOM_COUNT = sizeof(zoom_levels) / sizeof(zoom_levels[0]), ZOOM_DEFAULT = 100 };

static bool valid_theme(const char *theme)
{
    for (size_t i = 0; theme && i < THEME_COUNT; ++i)
        if (!strcmp(theme, theme_ids[i])) return true;
    return false;
}

static int zoom_index(int zoom)
{
    for (int i = 0; i < ZOOM_COUNT; ++i)
        if (zoom_levels[i] == zoom) return i;
    return -1;
}

static char *welcome_page(void)
{
    const char *configured = getenv("DISTILL_WELCOME_PAGE");
    if (configured && *configured && d_fs_readable(configured)) return d_cat2("file://", configured);
    char *installed = d_data_path("welcome.html", NULL);
    if (!installed) return NULL;
    char *url = d_cat2("file://", installed);
    free(installed);
    return url;
}

static void load_settings(char **start)
{
    DSettings *settings = d_settings_load(NULL);
    const char *theme = d_settings_get(settings, "browser", "theme");
    fb.theme = d_strdup(valid_theme(theme) ? theme : "warm");
    int zoom = d_settings_get_int(settings, "browser", "zoom", 100);
    fb.zoom = zoom_index(zoom) >= 0 ? zoom : ZOOM_DEFAULT;
    const char *value = d_settings_get(settings, "browser", "start-page");
    if (!value || !*value || !strcmp(value, "welcome")) *start = welcome_page();
    else if (!strcmp(value, "blank")) *start = NULL;
    else *start = d_strdup(value);
    d_settings_free(settings);
}

static void save_settings(void)
{
    DSettings *settings = d_settings_load(NULL);
    d_settings_set(settings, "browser", "theme", fb.theme);
    d_settings_set_int(settings, "browser", "zoom", fb.zoom);
    if (!d_settings_save(settings)) set_status("Could not save the settings file");
    d_settings_free(settings);
}

static void set_theme(size_t index)
{
    if (index >= THEME_COUNT || !strcmp(fb.theme, theme_ids[index])) return;
    free(fb.theme);
    fb.theme = d_strdup(theme_ids[index]);
    save_settings();
    set_status("Theme: %s", theme_labels[index]);
    reload();
}

static void cycle_theme(void)
{
    size_t at = 0;
    while (at < THEME_COUNT && strcmp(fb.theme, theme_ids[at])) ++at;
    set_theme(at + 1 < THEME_COUNT ? at + 1 : 0);
}

static void set_zoom(int zoom)
{
    if (zoom_index(zoom) < 0 || zoom == fb.zoom) return;
    fb.zoom = zoom;
    save_settings();
    set_status("Zoom %d%%", fb.zoom);
    reload();
}

/* Through the offered levels, as the desktop's Ctrl+= and Ctrl+- go, so the
   keys and the menu never disagree. The ends hold rather than wrap. */
static void step_zoom(int direction)
{
    int at = zoom_index(fb.zoom);
    if (at < 0) at = zoom_index(ZOOM_DEFAULT);
    at += direction;
    if (at < 0) at = 0;
    if (at >= ZOOM_COUNT) at = ZOOM_COUNT - 1;
    set_zoom(zoom_levels[at]);
}

/* What the browser opens with: the page in view, or one of the two fixed
   answers that are the way back from it. The desktop's three choices. */
static void set_start_page(int choice)
{
    const DAppPage *page = d_app_history_current(&fb.history);
    if (choice == 0 && (!page || !page->url || !*page->url)) {
        set_status("No page to start with yet");
        return;
    }
    const char *value = choice == 0 ? page->url : choice == 1 ? "welcome" : "blank";
    DSettings *settings = d_settings_load(NULL);
    d_settings_set(settings, "browser", "start-page", value);
    bool saved = d_settings_save(settings);
    d_settings_free(settings);
    if (!saved) set_status("Could not save the settings file");
    else if (choice == 0) set_status("Start page: %s", value);
    else set_status("Start page: %s", choice == 1 ? "the welcome page" : "blank");
}

/* ---- things the page asks the shell to do --------------------------------- */

static void reap_externals(void)
{
    for (size_t i = 0; i < fb.external_count;) {
        if (d_proc_poll_exit(fb.externals[i], NULL)) {
            d_proc_free(fb.externals[i]);
            fb.externals[i] = fb.externals[--fb.external_count];
            continue;
        }
        ++i;
    }
}

/* Somewhere the reader does not go: a video's player, a mail address. On a
   console there is usually nothing to hand it to, and the honest thing is to
   show the address. */
static void open_external(const char *uri)
{
    if (!d_external_uri_allowed(uri)) {
        set_status("Blocked: not an address that can be handed to the system");
        return;
    }
    DProc *handler = d_proc_open_with_system(uri);
    if (handler) {
        if (fb.external_count < EXTERNALS_MAX) fb.externals[fb.external_count++] = handler;
        else d_proc_free(handler);
        set_status("Opened with the system handler: %s", uri);
        return;
    }
    set_status("Open elsewhere: %s", uri);
}

static void transfer_cancel(void)
{
    if (!fb.transfer) return;
    d_media_transfer_cancel(fb.transfer);
    fb.transfer = NULL;
    set_status("Transfer cancelled");
}

static void transfer_start(const char *url, const char *destination, TransferKind kind)
{
    if (fb.transfer) transfer_cancel();
    const char *engine_kind = kind == TRANSFER_AUDIO ? "audio" : kind == TRANSFER_IMAGE ? "image" : "any";
    char *error = NULL;
    fb.transfer = d_media_transfer_start(fb.engine, url, destination, engine_kind, &error);
    if (!fb.transfer) {
        set_status("Could not start the fetch: %s", error ? error : "unknown error");
        free(error);
        return;
    }
    fb.transfer_kind = kind;
    set_status("%s", kind == TRANSFER_AUDIO ? "Fetching audio\xe2\x80\xa6"
                   : kind == TRANSFER_IMAGE ? "Fetching the image\xe2\x80\xa6" : "Downloading\xe2\x80\xa6");
}

/* What to call a track: the words on the line before its Play button, which
   is how an audio list names each file; else the file name. */
static char *track_title(const char *href, const char *url)
{
    Page *page = &fb.page;
    const DLinkBox *link = NULL;
    for (size_t i = 0; i < page->link_count && !link; ++i)
        if (!strcmp(page->links[i].href, href)) link = &page->links[i];
    if (link && page->doc) {
        size_t count = d_page_text_boxes(page->doc, NULL, 0);
        DTextBox *runs = d_xcalloc(count ? count : 1, sizeof(*runs));
        d_page_text_boxes(page->doc, runs, count);
        DBuffer words;
        dbuf_init(&words);
        int middle = link->y + link->height / 2;
        for (size_t i = 0; i < count; ++i) {
            const DTextBox *run = &runs[i];
            if (middle < run->y || middle >= run->y + run->height || run->x + run->width > link->x) continue;
            if (words.length) dbuf_puts(&words, " ");
            dbuf_puts(&words, run->text);
        }
        free(runs);
        char *title = dbuf_take(&words);
        char *trimmed = title;
        while (*trimmed == ' ') ++trimmed;
        if (*trimmed) {
            char *result = d_strdup(trimmed);
            free(title);
            return result;
        }
        free(title);
    }
    const char *tail = strrchr(url, '/');
    return d_strdup(tail && tail[1] ? tail + 1 : url);
}

static void audio_release(void)
{
    media_audio_close();
    if (fb.audio_file) { d_fs_remove(fb.audio_file); free(fb.audio_file); fb.audio_file = NULL; }
    free(fb.audio_title);
    fb.audio_title = NULL;
    fb.dirty = true;
}

static void play(const char *url, const char *href)
{
    if (!d_external_uri_allowed(url)) { set_status("Playback blocked: not a web address"); return; }
    MediaAudioState state;
    media_audio_snapshot(&state);
    /* Asking for what is already loaded is a transport command, not a refetch. */
    if (state.phase != MEDIA_AUDIO_STOPPED && state.phase != MEDIA_AUDIO_ERROR && !strcmp(state.item, url)) {
        if (state.phase == MEDIA_AUDIO_PLAYING) media_audio_pause();
        else media_audio_play();
        fb.dirty = true;
        return;
    }
    audio_release();
    fb.audio_title = track_title(href, url);
    transfer_start(url, NULL, TRANSFER_AUDIO);
}

static void toggle_play(void)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    if (state.phase == MEDIA_AUDIO_PLAYING) media_audio_pause();
    else media_audio_play();
    fb.dirty = true;
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
    fb.dirty = true;
}

static void seek_to_x(int x)
{
    MediaAudioState state;
    media_audio_snapshot(&state);
    DRenderRect track = fb.seek_track;
    if (!state.seekable || state.duration_seconds <= 0 || track.width <= 1) return;
    double fraction = (double)(x - track.x) / (track.width - 1);
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    media_audio_seek(fraction * state.duration_seconds);
    fb.dirty = true;
}

/* Where to save `url` by default: its last path segment in ~/Downloads, or
   `fallback`. With `extension`, the segment's own extension is replaced. */
static char *suggested_path(const char *url, const char *fallback, const char *extension)
{
    const char *tail = url ? strrchr(url, '/') : NULL;
    char *name = d_strdup(tail && tail[1] ? tail + 1 : fallback);
    char *cut = strpbrk(name, "?#");
    if (cut) *cut = '\0';
    for (char *at = name; *at; ++at)
        if (*at == '/' || *at == '\\' || *at == ':') *at = '-';
    if (!*name || name[0] == '.') { char *safe = d_cat2(fallback, name); free(name); name = safe; }
    if (extension) {
        char *dot = strrchr(name, '.');
        if (dot && dot != name) *dot = '\0';
        char *named = d_cat3(name, ".", extension);
        free(name);
        name = named;
    }
    const char *home = getenv("HOME");
    char *directory = home && *home ? d_cat2(home, "/Downloads") : d_strdup(".");
    if (!d_fs_is_directory(directory)) { free(directory); directory = d_strdup("."); }
    char *path = d_cat3(directory, "/", name);
    free(directory);
    free(name);
    return path;
}

static void prompt_open(PromptKind kind, const char *label, const char *initial)
{
    fb.prompt = kind;
    snprintf(fb.prompt_label, sizeof(fb.prompt_label), "%s", label);
    d_line_edit_free(&fb.prompt_edit);
    d_line_edit_init(&fb.prompt_edit, initial);
    fb.dirty = true;
}

static void prompt_close(void)
{
    fb.prompt = PROMPT_NONE;
    d_line_edit_free(&fb.prompt_edit);
    free(fb.prompt_url);
    fb.prompt_url = NULL;
    fb.dirty = true;
}

static void download(const char *url)
{
    if (!d_external_uri_allowed(url)) { set_status("Download blocked: not a web address"); return; }
    char *path = suggested_path(url, "download", NULL);
    prompt_open(PROMPT_SAVE_AS, "Save as: ", path);
    fb.prompt_url = d_strdup(url);
    free(path);
}

/* Export the page the reader is showing, as the desktop does. The reader
   page is already a self-contained document, so HTML is a copy of it, and
   Markdown is the engine's conversion of that same file: fetching the
   address again would export a different reading of it. */
static void export_page(bool as_markdown)
{
    const DAppPage *page = d_app_history_current(&fb.history);
    if (!page || !page->template_path) {
        set_status("There is no page to export yet");
        return;
    }
    char *path = suggested_path(page->url, "page", as_markdown ? "md" : "html");
    prompt_open(as_markdown ? PROMPT_EXPORT_MARKDOWN : PROMPT_EXPORT_HTML,
                as_markdown ? "Export as Markdown: " : "Export as HTML: ", path);
    free(path);
}

static void export_to(bool as_markdown, const char *destination)
{
    const DAppPage *page = d_app_history_current(&fb.history);
    if (!page || !page->template_path) {
        set_status("There is no page to export yet");
        return;
    }
    bool ok = false;
    if (!as_markdown) {
        size_t length = 0;
        char *html = d_read_file(page->template_path, &length);
        ok = html && d_fs_replace(destination, html, length);
        free(html);
    } else {
        DRequestFile request;
        d_request_file_init(&request);
        d_request_file_add(&request, "markdown-from", page->template_path);
        d_request_file_add(&request, "markdown", destination);
        d_request_file_add(&request, "url", page->url);
        char *request_path = d_request_file_write(&request);
        const char *argv[] = {fb.engine, "--request", request_path, NULL};
        char *error = NULL;
        DProc *engine = request_path ? d_proc_spawn(argv, D_PROC_DISCARD, D_PROC_DISCARD, &error) : NULL;
        if (engine) {
            DProcStatus status = d_proc_wait(engine);
            ok = status.exited && status.code == 0;
            d_proc_free(engine);
        }
        free(error);
        if (request_path) d_fs_remove(request_path);
        free(request_path);
    }
    if (ok) set_status("Exported to %s", destination);
    else set_status("Could not export to %s", destination);
}

/* ---- viewer mode ---------------------------------------------------------- */

static void viewer_free_image(void)
{
    if (fb.viewer.loaded) d_image_free(&fb.viewer.image);
    fb.viewer.loaded = false;
    if (fb.viewer.file) { d_fs_remove(fb.viewer.file); free(fb.viewer.file); fb.viewer.file = NULL; }
    free(fb.viewer.failure);
    fb.viewer.failure = NULL;
    fb.viewer.layer_valid = false;
}

static void viewer_fetch(void)
{
    viewer_free_image();
    fb.viewer.zoom = 1;
    fb.viewer.pan_x = fb.viewer.pan_y = 0;
    transfer_start(fb.viewer.urls[fb.viewer.index], NULL, TRANSFER_IMAGE);
}

/* Viewer mode is a state of this surface, not a second window: the page's
   images in order, one at a time. */
static void viewer_enter(const char *url)
{
    for (size_t i = 0; i < fb.viewer.count; ++i) free(fb.viewer.urls[i]);
    free(fb.viewer.urls);
    fb.viewer.urls = NULL;
    fb.viewer.count = 0;
    fb.viewer.index = 0;
    for (size_t i = 0; fb.has_page && i < fb.page.link_count; ++i) {
        const char *href = fb.page.links[i].href;
        if (!d_starts(href, MEDIA_SCHEME)) continue;
        const char *target = href + strlen(MEDIA_SCHEME);
        bool seen = false;
        for (size_t k = 0; k < fb.viewer.count; ++k) seen |= !strcmp(fb.viewer.urls[k], target);
        if (seen) continue;
        if (!strcmp(target, url)) fb.viewer.index = fb.viewer.count;
        fb.viewer.urls = d_xrealloc(fb.viewer.urls, (fb.viewer.count + 1) * sizeof(char *));
        fb.viewer.urls[fb.viewer.count++] = d_strdup(target);
    }
    if (!fb.viewer.count) {
        fb.viewer.urls = d_xmalloc(sizeof(char *));
        fb.viewer.urls[fb.viewer.count++] = d_strdup(url);
    }
    fb.viewer.active = true;
    fb.hover[0] = '\0';
    fb.dirty = true;
    viewer_fetch();
}

static void viewer_leave(void)
{
    if (!fb.viewer.active) return;
    if (fb.transfer && fb.transfer_kind == TRANSFER_IMAGE) transfer_cancel();
    viewer_free_image();
    fb.viewer.active = false;
    fb.dirty = fb.page_dirty = true;
    set_status("%s", fb.page.url ? fb.page.url : "");
}

static void viewer_step(int delta)
{
    if (!fb.viewer.active || fb.viewer.count < 2) return;
    if (delta < 0) fb.viewer.index = fb.viewer.index ? fb.viewer.index - 1 : fb.viewer.count - 1;
    else fb.viewer.index = (fb.viewer.index + 1) % fb.viewer.count;
    fb.dirty = true;
    viewer_fetch();
}

static void viewer_zoom(double factor)
{
    if (!fb.viewer.loaded) return;
    double zoom = factor <= 0 ? 1 : fb.viewer.zoom * factor;
    if (zoom < 0.25) zoom = 0.25;
    if (zoom > 16) zoom = 16;
    fb.viewer.zoom = zoom;
    if (factor <= 0) fb.viewer.pan_x = fb.viewer.pan_y = 0;
    fb.viewer.layer_valid = false;
    fb.dirty = true;
}

static void viewer_pan(int dx, int dy)
{
    fb.viewer.pan_x += dx;
    fb.viewer.pan_y += dy;
    fb.viewer.layer_valid = false;
    fb.dirty = true;
}

static void transfer_service(void)
{
    if (!fb.transfer) return;
    DTransferState state = d_media_transfer_poll(fb.transfer);
    if (state == D_TRANSFER_RUNNING) return;
    DMediaTransfer *transfer = fb.transfer;
    fb.transfer = NULL;
    fb.dirty = true;
    if (state == D_TRANSFER_FAILED) {
        if (fb.transfer_kind == TRANSFER_IMAGE && fb.viewer.active) {
            fb.viewer.failure = d_strdup(d_media_transfer_error(transfer));
            fb.viewer.layer_valid = false;
        }
        set_status("Could not fetch the media: %s", d_media_transfer_error(transfer));
    } else if (fb.transfer_kind == TRANSFER_DOWNLOAD) {
        set_status("Saved to %s", d_media_transfer_path(transfer));
    } else if (fb.transfer_kind == TRANSFER_AUDIO) {
        fb.audio_file = d_strdup(d_media_transfer_path(transfer));
        if (!media_audio_open(fb.audio_file, d_media_transfer_url(transfer))) {
            MediaAudioState audio;
            media_audio_snapshot(&audio);
            set_status("Cannot play this file: %s",
                       audio.error_message[0] ? audio.error_message : "no decoder accepted it");
        } else if (media_audio_play()) {
            set_status("Playing \xe2\x80\x94 Space pauses, s stops, \xe2\x86\x90/\xe2\x86\x92 seek, [ ] volume, x closes");
        }
        fb.page_dirty = true;
    } else if (fb.transfer_kind == TRANSFER_IMAGE && fb.viewer.active) {
        fb.viewer.file = d_strdup(d_media_transfer_path(transfer));
        if (d_image_load_file(fb.viewer.file, &fb.viewer.image)) {
            fb.viewer.loaded = true;
            set_status("Image %zu of %zu \xe2\x80\x94 \xe2\x86\x90/\xe2\x86\x92 step, + \xe2\x88\x92 zoom, Esc closes",
                       fb.viewer.index + 1, fb.viewer.count);
        } else {
            fb.viewer.failure = d_strdup(d_image_last_error());
            set_status("The image could not be decoded: %s", fb.viewer.failure);
        }
        fb.viewer.layer_valid = false;
    } else {
        d_fs_remove(d_media_transfer_path(transfer));
    }
    d_media_transfer_free(transfer);
}

/* ---- links ---------------------------------------------------------------- */

/* The sections the desktop's Alt+0 to Alt+6 and its sections menu go to,
   in its order. */
static const char *const section_ids[] = {"top", "images", "tables", "forms", "videos", "results", "links"};
static const char *const section_labels[] = {"Top", "Images", "Tables", "Forms", "Videos", "Results", "Links"};
static const char *const section_keys[] = {"Alt+0", "Alt+1", "Alt+2", "Alt+3", "Alt+4", "Alt+5", "Alt+6"};
enum { SECTION_COUNT = sizeof(section_ids) / sizeof(section_ids[0]) };

/* Where the template's marker for a section is, as the desktop finds it;
   else an element with that id. The top is the top of the page, as on the
   desktop, not its marker. -1 for none. */
static int section_y(const char *id)
{
    if (!fb.has_page || !fb.page.doc) return -1;
    if (!strcmp(id, "top")) return 0;
    char href[96];
    snprintf(href, sizeof(href), "%s%s", SECTION_SCHEME, id);
    for (size_t i = 0; i < fb.page.link_count; ++i)
        if (!strcmp(fb.page.links[i].href, href)) return fb.page.links[i].y;
    return d_page_anchor_y(fb.page.doc, id);
}

static void jump_to_section(const char *id)
{
    int y = section_y(id);
    if (y >= 0) scroll_to(y ? y - line_step() / 2 : 0);
    else set_status("No section %s on this page", id);
}

static void follow_href(const char *href)
{
    char *unwrapped = d_address_unwrap_redirect(href);
    const char *uri = unwrapped ? unwrapped : href;
    if (d_starts(uri, SECTION_SCHEME)) jump_to_section(uri + strlen(SECTION_SCHEME));
    else if (d_starts(uri, MEDIA_SCHEME)) viewer_enter(uri + strlen(MEDIA_SCHEME));
    else if (d_starts(uri, PLAY_SCHEME)) play(uri + strlen(PLAY_SCHEME), href);
    else if (d_starts(uri, DOWNLOAD_SCHEME)) download(uri + strlen(DOWNLOAD_SCHEME));
    else if (d_starts(uri, OPEN_SCHEME)) open_external(uri + strlen(OPEN_SCHEME));
    else if (d_starts(uri, "distill-")) { /* a desktop affordance with nothing to do here */ }
    else if (!d_ncasecmp(uri, "mailto:", 7) || !d_ncasecmp(uri, "tel:", 4)) open_external(uri);
    else if (uri[0] == '#') jump_to_section(uri + 1);
    else if (d_page_uri_allowed(uri)) navigate(uri, NULL, NULL, false);
    else open_external(uri);
    free(unwrapped);
}

/* ---- forms ---------------------------------------------------------------- */

static void stop_editing(void)
{
    if (!fb.editing) return;
    fb.editing = false;
    d_line_edit_free(&fb.field);
    fb.page_dirty = fb.dirty = true;
}

static void store_field(void)
{
    if (!fb.editing) return;
    size_t control = fb.field_control;
    free(fb.page.values[control]);
    fb.page.values[control] = d_strdup(d_line_edit_text(&fb.field));
    fb.page_dirty = fb.dirty = true;
}

static void focus_changed(void)
{
    stop_editing();
    Page *page = &fb.page;
    if (page->focus >= 0) {
        Focus *focus = &page->focusables[page->focus];
        if (focus->kind == FOCUS_CONTROL && control_is_text(&page->controls[focus->index])) {
            fb.editing = true;
            fb.field_control = focus->index;
            d_line_edit_init(&fb.field, page->values[focus->index]);
        }
        if (focus->kind == FOCUS_LINK) reveal(page->links[focus->index].y, page->links[focus->index].height);
        else reveal(page->controls[focus->index].y, page->controls[focus->index].height);
    }
    fb.page_dirty = fb.dirty = true;
}

static void focus_on(FocusKind kind, size_t index)
{
    for (size_t i = 0; i < fb.page.focus_count; ++i)
        if (fb.page.focusables[i].kind == kind && fb.page.focusables[i].index == index) {
            if (fb.page.focus != (int)i) { fb.page.focus = (int)i; focus_changed(); }
            return;
        }
}

static void unfocus(void)
{
    if (fb.page.focus < 0) return;
    fb.page.focus = -1;
    focus_changed();
}

static void move_focus(int direction)
{
    Page *page = &fb.page;
    if (!page->focus_count) return;
    if (page->focus < 0) {
        /* Start from what is on screen, not from the top of the page. */
        int first = -1;
        for (size_t i = 0; i < page->focus_count; ++i) {
            int y = page->focusables[i].y;
            if (y >= page->scroll && y < page->scroll + view_height()) {
                if (direction > 0 && first < 0) first = (int)i;
                if (direction < 0) first = (int)i;
            }
        }
        page->focus = first >= 0 ? first : (direction > 0 ? 0 : (int)page->focus_count - 1);
    } else {
        page->focus = (page->focus + direction + (int)page->focus_count) % (int)page->focus_count;
    }
    focus_changed();
}

static void submit_form(size_t submitter)
{
    Page *page = &fb.page;
    size_t count = page->control_count;
    DFormField *fields = d_xcalloc(count ? count : 1, sizeof(*fields));
    for (size_t i = 0; i < count; ++i) {
        DFormBox *box = &page->controls[i];
        bool choice = control_is_choice(box);
        fields[i] = (DFormField){
            .form_index = box->form_index, .tag = box->tag, .type = box->type, .name = box->name,
            .value = choice ? box->value : page->values[i],
            .checked = choice && page->checked[i],
            .disabled = box->disabled, .required = box->required,
        };
    }
    DFormBox *source = &page->controls[submitter];
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

static void popup_open(size_t control)
{
    DFormBox *box = &fb.page.controls[control];
    Popup popup = {.control = control};
    for (const char *record = box->options; *record;) {
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
        if (!strcmp(popup.values[popup.count], fb.page.values[control])) popup.selected = popup.count;
        ++popup.count;
        record = end + 1;
    }
    if (!popup.count) { set_status("This list has no choices"); return; }
    fb.popup = popup;
    fb.popup_open = true;
    fb.dirty = true;
}

static void popup_close(bool choose)
{
    if (!fb.popup_open) return;
    if (choose) {
        size_t control = fb.popup.control;
        free(fb.page.values[control]);
        fb.page.values[control] = d_strdup(fb.popup.values[fb.popup.selected]);
        fb.page_dirty = true;
    }
    for (size_t i = 0; i < fb.popup.count; ++i) { free(fb.popup.values[i]); free(fb.popup.labels[i]); }
    free(fb.popup.values);
    free(fb.popup.labels);
    fb.popup = (Popup){0};
    fb.popup_open = false;
    fb.dirty = true;
}

static const char *option_label(const DFormBox *box, const char *value)
{
    static char label[512];
    label[0] = '\0';
    for (const char *record = box->options; *record;) {
        const char *end = strchr(record, '\x1e');
        if (!end) break;
        const char *unit = memchr(record, '\x1f', (size_t)(end - record));
        size_t value_length = unit ? (size_t)(unit - record) : (size_t)(end - record);
        if (strlen(value) == value_length && !strncmp(record, value, value_length)) {
            const char *text = unit ? unit + 1 : record;
            const char *text_end = unit ? memchr(text, '\x1f', (size_t)(end - text)) : end;
            if (!text_end) text_end = end;
            size_t length = (size_t)(text_end - text);
            if (length >= sizeof(label)) length = sizeof(label) - 1;
            memcpy(label, text, length);
            label[length] = '\0';
            return label;
        }
        record = end + 1;
    }
    return value;
}

static void activate_control(size_t index)
{
    Page *page = &fb.page;
    DFormBox *box = &page->controls[index];
    if (box->disabled) return;
    fb.page_dirty = fb.dirty = true;
    if (control_type(box, "checkbox")) {
        page->checked[index] = !page->checked[index];
    } else if (control_type(box, "radio")) {
        for (size_t i = 0; i < page->control_count; ++i) {
            DFormBox *other = &page->controls[i];
            if (i != index && control_type(other, "radio") && other->form_index == box->form_index &&
                !strcmp(other->name, box->name))
                page->checked[i] = false;
        }
        page->checked[index] = true;
    } else if (!strcmp(box->tag, "select")) {
        popup_open(index);
    } else if (control_type(box, "reset")) {
        for (size_t i = 0; i < page->control_count; ++i) {
            if (page->controls[i].form_index != box->form_index) continue;
            free(page->values[i]);
            page->values[i] = d_strdup(page->controls[i].value);
            page->checked[i] = page->controls[i].checked;
        }
        if (fb.editing) d_line_edit_set(&fb.field, page->values[fb.field_control]);
    } else if (control_type(box, "button")) {
        set_status("This button only does something with the page's own scripts");
    } else if (control_is_button(box)) {
        store_field();
        submit_form(index);
    }
}

static void activate_focus(void)
{
    if (fb.page.focus < 0) return;
    Focus *focus = &fb.page.focusables[fb.page.focus];
    if (focus->kind == FOCUS_LINK) follow_href(fb.page.links[focus->index].href);
    else activate_control(focus->index);
}

/* ---- menus ---------------------------------------------------------------- */

/* The desktop's sections and settings menus, item for item. The keys do
   what they did before; the menus are the way to find them. */

static const char *const menu_names[] = {"", "sections", "settings", "export", "theme", "zoom", "start"};

static size_t menu_items(MenuId menu, MenuItem *items)
{
    size_t n = 0;
    const DAppPage *page = d_app_history_current(&fb.history);
#define ITEM(label_, shortcut_, kind_, arg_, enabled_, radio_) \
    items[n++] = (MenuItem){(label_), (shortcut_), (kind_), (arg_), (enabled_), (radio_)}
    switch (menu) {
    case MENU_SECTIONS:
        for (int i = 0; i < SECTION_COUNT; ++i)
            ITEM(section_labels[i], section_keys[i], ITEM_SECTION, i,
                 !fb.viewer.active && section_y(section_ids[i]) >= 0, -1);
        break;
    case MENU_SETTINGS:
        ITEM("Open externally", NULL, ITEM_OPEN_EXTERNAL, 0, page != NULL, -1);
        ITEM("Export page", NULL, ITEM_SUBMENU, MENU_EXPORT, true, -1);
        ITEM("Theme", "t", ITEM_SUBMENU, MENU_THEME, true, -1);
        ITEM("Zoom", "+ \xe2\x88\x92", ITEM_SUBMENU, MENU_ZOOM, true, -1);
        ITEM("Start page", NULL, ITEM_SUBMENU, MENU_START, true, -1);
        break;
    case MENU_EXPORT:
        ITEM("As HTML\xe2\x80\xa6", NULL, ITEM_EXPORT, 0, true, -1);
        ITEM("As Markdown\xe2\x80\xa6", NULL, ITEM_EXPORT, 1, true, -1);
        break;
    case MENU_THEME:
        for (int i = 0; i < THEME_COUNT; ++i)
            ITEM(theme_labels[i], NULL, ITEM_THEME, i, true, !strcmp(fb.theme, theme_ids[i]));
        break;
    case MENU_ZOOM:
        for (int i = 0; i < ZOOM_COUNT; ++i)
            ITEM(zoom_labels[i], zoom_levels[i] == ZOOM_DEFAULT ? "0" : NULL, ITEM_ZOOM, i, true,
                 zoom_levels[i] == fb.zoom);
        break;
    case MENU_START:
        ITEM("Use the current page", NULL, ITEM_START, 0, page != NULL, -1);
        ITEM("Use the welcome page", NULL, ITEM_START, 1, true, -1);
        ITEM("Start blank", NULL, ITEM_START, 2, true, -1);
        break;
    default: break;
    }
#undef ITEM
    return n;
}

/* The first enabled item from `from` in `direction`, wrapping; -1 for none. */
static int menu_step(MenuId menu, int from, int direction)
{
    MenuItem items[MENU_ITEMS_MAX];
    int count = (int)menu_items(menu, items);
    for (int step = 1; step <= count; ++step) {
        int at = ((from + direction * step) % count + count) % count;
        if (items[at].enabled) return at;
    }
    return -1;
}

static void menu_close(void)
{
    fb.menu[0].id = fb.menu[1].id = MENU_NONE;
    fb.in_submenu = false;
    fb.dirty = true;
}

static void menu_open(MenuId menu, bool select_first)
{
    stop_editing();
    if (fb.popup_open) popup_close(false);
    if (fb.prompt == PROMPT_ADDRESS) prompt_close();
    fb.menu[0] = (MenuLevel){.id = menu, .selected = select_first ? menu_step(menu, -1, 1) : -1};
    fb.menu[1].id = MENU_NONE;
    fb.in_submenu = false;
    fb.dirty = true;
}

static void submenu_open(MenuId menu, bool select)
{
    if (fb.menu[1].id != menu) {
        MenuItem items[MENU_ITEMS_MAX];
        size_t count = menu_items(menu, items);
        int chosen = -1;
        for (size_t i = 0; i < count; ++i)
            if (items[i].radio > 0) chosen = (int)i;
        fb.menu[1] = (MenuLevel){.id = menu, .selected = chosen >= 0 ? chosen : menu_step(menu, -1, 1)};
    }
    if (select) fb.in_submenu = true;
    fb.dirty = true;
}

static void menu_run(const MenuItem *item)
{
    menu_close();
    switch (item->kind) {
    case ITEM_SECTION:
        jump_to_section(section_ids[item->arg]);
        set_status("%s", section_labels[item->arg]);
        break;
    case ITEM_OPEN_EXTERNAL: {
        const DAppPage *page = d_app_history_current(&fb.history);
        if (page && page->url) open_external(page->url);
        break;
    }
    case ITEM_EXPORT: export_page(item->arg == 1); break;
    case ITEM_THEME: set_theme((size_t)item->arg); break;
    case ITEM_ZOOM: set_zoom(zoom_levels[item->arg]); break;
    case ITEM_START: set_start_page(item->arg); break;
    default: break;
    }
}

/* Choose item `index` of the menu at `level`: a submenu opens, anything
   else is done and the menu closes. */
static void menu_choose(int level, int index)
{
    MenuItem items[MENU_ITEMS_MAX];
    size_t count = menu_items(fb.menu[level].id, items);
    if (index < 0 || (size_t)index >= count || !items[index].enabled) return;
    fb.menu[level].selected = index;
    if (items[index].kind == ITEM_SUBMENU) submenu_open((MenuId)items[index].arg, true);
    else menu_run(&items[index]);
}

/* ---- drawing -------------------------------------------------------------- */

static void fill(DRenderContext *context, int x, int y, int width, int height, DRenderColor color)
{
    if (width > 0 && height > 0) d_render_fill(context, (DRenderRect){x, y, width, height}, color);
}

/* Bullets for a password, one per character. */
static char *masked(const char *text, size_t *cursor)
{
    DBuffer out;
    dbuf_init(&out);
    size_t shown_cursor = 0;
    for (size_t i = 0; text[i];) {
        if (cursor && i == *cursor) shown_cursor = out.length;
        size_t step = 1;
        while (text[i + step] && ((unsigned char)text[i + step] & 0xc0) == 0x80) ++step;
        dbuf_puts(&out, "\xe2\x80\xa2");
        i += step;
    }
    if (cursor) *cursor = *cursor >= strlen(text) ? out.length : shown_cursor;
    return dbuf_take(&out);
}

static void draw_text_control(DRenderContext *context, size_t index, DRenderRect r, bool focused)
{
    const Palette *p = palette();
    DFormBox *box = &fb.page.controls[index];
    bool editing = fb.editing && fb.field_control == index;
    bool area = !strcmp(box->tag, "textarea");
    fill(context, r.x, r.y, r.width, r.height, box->disabled ? p->card_bg : p->main_bg);
    d_render_stroke(context, r, focused ? 2 : 1, focused ? p->link : p->border);
    int pad = fb.control_px / 2;
    DRenderRect inner = {r.x + pad, r.y + 1, r.width - 2 * pad, r.height - 2};
    DRenderColor ink = box->disabled ? p->muted : p->text;
    if (inner.width <= 0 || inner.height <= 0) return;
    const char *value = editing ? d_line_edit_text(&fb.field) : fb.page.values[index];
    size_t cursor = editing ? fb.field.cursor : 0;
    bool secret = control_type(box, "password");
    char *shown = secret ? masked(value, &cursor) : d_strdup(value);
    d_render_set_clip(context, inner);
    const Font *font = &fb.control;
    if (!*shown && !editing && *box->placeholder) {
        draw_text(context, font, inner.x, area ? inner.y + font->metrics.height / 2 + pad / 2
                                                 : r.y + r.height / 2, box->placeholder, p->muted);
    } else if (!area) {
        /* One line, scrolled so the cursor stays in view. */
        int caret = font->font ? d_render_text_x_at(font->font, shown, cursor) : 0;
        int shift = caret > inner.width - 2 ? caret - (inner.width - 2) : 0;
        draw_text(context, font, inner.x - shift, r.y + r.height / 2, shown, ink);
        if (editing)
            fill(context, inner.x - shift + caret, r.y + r.height / 2 - font->metrics.height / 2,
                 2, font->metrics.height, p->text);
    } else {
        /* Lines as typed, each wrapped to the box. */
        int line_height = font->metrics.height;
        int y = inner.y + pad / 2, caret_x = inner.x, caret_y = y;
        size_t start = 0;
        for (;;) {
            size_t end = start;
            while (shown[end] && shown[end] != '\n') ++end;
            char *line = d_strndup(shown + start, end - start);
            DRenderTextLine lines[64];
            size_t count = *line && font->font ? d_render_wrap_text(font->font, line, inner.width, lines, 64) : 0;
            if (!count) { lines[0] = (DRenderTextLine){0, 0}; count = 1; }
            for (size_t l = 0; l < count && l < 64; ++l) {
                char *piece = d_strndup(line + lines[l].begin, lines[l].end - lines[l].begin);
                draw_text(context, font, inner.x, y + line_height / 2, piece, ink);
                size_t piece_start = start + lines[l].begin, piece_end = start + lines[l].end;
                bool last = l + 1 == count || l + 1 == 64;
                if (editing && cursor >= piece_start && (cursor < piece_end || (last && cursor <= end))) {
                    caret_x = inner.x + (font->font ? d_render_text_x_at(font->font, piece, cursor - piece_start) : 0);
                    caret_y = y;
                }
                free(piece);
                y += line_height;
            }
            free(line);
            if (!shown[end]) break;
            start = end + 1;
        }
        if (editing) fill(context, caret_x, caret_y, 2, line_height, p->text);
    }
    d_render_del_clip(context);
    free(shown);
}

static void draw_control(DRenderContext *context, size_t index, int scroll)
{
    const Palette *p = palette();
    Page *page = &fb.page;
    DFormBox *box = &page->controls[index];
    if (!control_visible(box)) return;
    DRenderRect r = {box->x, box->y - scroll, box->width, box->height};
    if (r.y + r.height < 0 || r.y > page->doc_surface) return;
    bool focused = page->focus >= 0 && page->focusables[page->focus].kind == FOCUS_CONTROL &&
                   page->focusables[page->focus].index == index;
    if (control_is_text(box)) { draw_text_control(context, index, r, focused); return; }
    if (control_is_choice(box)) {
        int side = r.width < r.height ? r.width : r.height;
        DRenderRect square = {r.x, r.y + (r.height - side) / 2, side, side};
        double cx = square.x + side / 2.0, cy = square.y + side / 2.0;
        if (control_type(box, "radio")) {
            d_render_disc(context, cx, cy, side / 2.0, focused ? p->link : p->border);
            d_render_disc(context, cx, cy, side / 2.0 - (focused ? 2.0 : 1.2), p->main_bg);
            if (page->checked[index]) d_render_disc(context, cx, cy, side * 0.24, p->link);
        } else {
            fill(context, square.x, square.y, side, side, p->main_bg);
            d_render_stroke(context, square, focused ? 2 : 1, focused ? p->link : p->border);
            if (page->checked[index]) {
                int inset = side / 4;
                fill(context, square.x + inset, square.y + inset, side - 2 * inset, side - 2 * inset, p->link);
            }
        }
        return;
    }
    if (!strcmp(box->tag, "select")) {
        fill(context, r.x, r.y, r.width, r.height, p->main_bg);
        d_render_stroke(context, r, focused ? 2 : 1, focused ? p->link : p->border);
        int pad = fb.control_px / 2;
        d_render_set_clip(context, (DRenderRect){r.x + pad, r.y, r.width - 2 * pad, r.height});
        draw_text(context, &fb.control, r.x + pad, r.y + r.height / 2, option_label(box, page->values[index]), p->text);
        d_render_del_clip(context);
        int arrow = text_width(&fb.symbol, "\xe2\x96\xbe");
        draw_text(context, &fb.symbol, r.x + r.width - pad - arrow, r.y + r.height / 2, "\xe2\x96\xbe", p->muted);
        return;
    }
    /* A button: the reader's link colour, as the page's own buttons are. */
    const char *caption = !strcmp(box->tag, "button") && *box->caption ? box->caption
                        : *box->value ? box->value : control_type(box, "reset") ? "Reset" : "Submit";
    int width = text_width(&fb.control_bold, caption) + fb.control_px * 3 / 2;
    DRenderRect face = {r.x, r.y, width > r.width ? width : r.width, r.height};
    if (!strcmp(box->tag, "button")) {
        /* The layout reports a button's content box and the template keeps
           its padding (.55em .85em): the face is the padded box. */
        int pad_x = fb.control_px * 85 / 100, pad_y = fb.control_px * 55 / 100;
        face = (DRenderRect){r.x - pad_x, r.y - pad_y, r.width + 2 * pad_x, r.height + 2 * pad_y};
    }
    fill(context, face.x, face.y, face.width, face.height, box->disabled ? p->border : p->link);
    if (focused) d_render_stroke(context, (DRenderRect){face.x - 3, face.y - 3, face.width + 6, face.height + 6}, 2, p->heading);
    int text = text_width(&fb.control_bold, caption);
    draw_text(context, &fb.control_bold, face.x + (face.width - text) / 2, face.y + face.height / 2, caption, p->main_bg);
}

/* The page band: painted by the layout, then the controls and the focus
   drawn over it in the same pixels. Repainted only when it changed. */
static void paint_page_layer(void)
{
    Page *page = &fb.page;
    if (!page->doc) return;
    const Palette *p = palette();
    make_control_fonts(page->body_px);
    d_page_paint(page->doc, page->scroll);
    DRenderContext *context = d_page_context(page->doc);
    int bottom = page->height - page->scroll;
    if (bottom < page->doc_surface) fill(context, 0, bottom, page->doc_width, page->doc_surface - bottom, p->page_bg);
    for (size_t i = 0; i < page->control_count; ++i) draw_control(context, i, page->scroll);
    if (page->focus >= 0 && page->focusables[page->focus].kind == FOCUS_LINK) {
        DLinkBox *link = &page->links[page->focusables[page->focus].index];
        d_render_stroke(context, (DRenderRect){link->x - 3, link->y - page->scroll - 2, link->width + 6, link->height + 4},
                        2, p->link);
    }
    fb.page_dirty = false;
}

static void draw_scrollbar(void)
{
    int most = max_scroll();
    if (!most) return;
    const Palette *p = palette();
    int top = view_top(), view = view_height(), width = fb.scrollbar_width;
    DRenderRect track = {fb.mode.width - width - 2, top, width + 2, view};
    int thumb = (int)((long long)view * view / fb.page.height);
    if (thumb < fb.ui_px * 2) thumb = fb.ui_px * 2;
    if (thumb > view) thumb = view;
    int y = top + (int)((long long)(view - thumb) * fb.page.scroll / most);
    fill(fb.screen, track.x + 2, y + 2, width - 2, thumb - 4, with_alpha(p->muted, 150));
    add_button(track, ACTION_SCROLLBAR, y - top);
}

static void paint_viewer_layer(void)
{
    const Palette *p = palette();
    int width = fb.mode.width, height = surface_height();
    if (fb.viewer.layer) {
        DRenderPixels pixels;
        d_render_pixels(fb.viewer.layer, &pixels);
        if (pixels.width != width || pixels.height != height) {
            d_render_destroy(fb.viewer.layer);
            fb.viewer.layer = NULL;
        }
    }
    if (!fb.viewer.layer) fb.viewer.layer = d_render_create(width, height, fb.font_dir);
    if (!fb.viewer.layer) return;
    DRenderContext *layer = fb.viewer.layer;
    int view = view_height() - fb.status_height;
    fill(layer, 0, 0, width, height, p->page_bg);
    const char *url = fb.viewer.urls[fb.viewer.index];
    if (!fb.viewer.loaded) {
        const char *what = fb.viewer.failure ? fb.viewer.failure : "Fetching\xe2\x80\xa6";
        draw_text(layer, &fb.bold, fb.ui_px * 2, fb.ui_px * 2, what, p->text);
        draw_text(layer, &fb.ui, fb.ui_px * 2, fb.ui_px * 4, url, p->muted);
    } else {
        DImage *image = &fb.viewer.image;
        double across = (double)(width - 2 * fb.ui_px) / image->width, down = (double)(view - 2 * fb.ui_px) / image->height;
        double fit = across < down ? across : down;
        if (fit > 1) fit = 1;
        if (fit <= 0) fit = 1;
        double scale = fit * fb.viewer.zoom;
        int w = (int)(image->width * scale + 0.5), h = (int)(image->height * scale + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        int x = (width - w) / 2 + fb.viewer.pan_x, y = (view - h) / 2 + fb.viewer.pan_y;
        d_render_draw_image(layer, (DRenderRect){x, y, w, h}, image->pixels, image->width, image->height);
    }
    fb.viewer.layer_valid = true;
}

static int draw_chip(int x, int middle, const Font *font, const char *text, Action action, int arg,
                     bool enabled, bool pressed)
{
    const Palette *p = palette();
    int pad = fb.ui_px / 2;
    int width = text_width(font, text) + 2 * pad;
    int height = fb.bar_height - fb.ui_px / 2;
    DRenderRect rect = {x, middle - height / 2, width, height};
    if (pressed) fill(fb.screen, rect.x, rect.y, rect.width, rect.height, with_alpha(p->link, 40));
    draw_text(fb.screen, font, x + pad, middle, text, enabled ? p->text : with_alpha(p->muted, 140));
    if (enabled) add_button(rect, action, arg);
    return x + width;
}

/* The desktop's two menu buttons, drawn rather than taken from a font that
   may not have the symbols: a list for the sections, a cog for settings. */
static DRenderRect draw_menu_button(int x, int middle, MenuId menu)
{
    const Palette *p = palette();
    int side = fb.bar_height - fb.ui_px / 2;
    DRenderRect rect = {x, middle - side / 2, side, side};
    bool open = fb.menu[0].id == menu;
    if (open) fill(fb.screen, rect.x, rect.y, rect.width, rect.height, with_alpha(p->link, 40));
    double cx = x + side / 2.0, cy = middle, unit = fb.ui_px / 16.0;
    if (menu == MENU_SECTIONS) {
        int bar = (int)(2 * unit + 0.5) > 1 ? (int)(2 * unit + 0.5) : 1;
        int gap = (int)(5 * unit + 0.5), left = (int)(cx - 7 * unit), dot = bar + 1;
        for (int row = -1; row <= 1; ++row) {
            int y = (int)cy + row * gap - bar / 2;
            fill(fb.screen, left, y - (dot - bar) / 2, dot, dot, p->text);
            fill(fb.screen, left + dot + (int)(2 * unit + 0.5), y, (int)(11 * unit), bar, p->text);
        }
    } else {
        double body = 5.2 * unit, tooth = 1.9 * unit;
        for (int i = 0; i < 8; ++i) {
            double angle = i * 3.14159265358979 / 4;
            d_render_disc(fb.screen, cx + cos(angle) * 6.4 * unit, cy + sin(angle) * 6.4 * unit, tooth, p->text);
        }
        d_render_disc(fb.screen, cx, cy, body + 0.8 * unit, p->text);
        /* The hole shows the bar, and the button's tint while its menu is open. */
        d_render_disc(fb.screen, cx, cy, 2.4 * unit, p->card_bg);
        if (open) d_render_disc(fb.screen, cx, cy, 2.4 * unit, with_alpha(p->link, 40));
    }
    add_button(rect, ACTION_MENU, menu);
    return rect;
}

static void draw_top_bar(void)
{
    const Palette *p = palette();
    int width = fb.mode.width, height = fb.bar_height, middle = height / 2;
    fill(fb.screen, 0, 0, width, height, p->card_bg);
    fill(fb.screen, 0, height - 1, width, 1, p->border);
    int x = fb.ui_px / 2;
    x = draw_chip(x, middle, &fb.symbol, "\xe2\x86\x90", ACTION_BACK, 0, d_app_history_can_back(&fb.history), false);
    x = draw_chip(x, middle, &fb.symbol, "\xe2\x86\x92", ACTION_FORWARD, 0, d_app_history_can_forward(&fb.history), false);
    x = draw_chip(x, middle, &fb.symbol, fb.nav.active ? "\xe2\x9c\x95" : "\xe2\x86\xbb", ACTION_RELOAD, 0, true, false);
    /* The menu buttons first, at the right as on the desktop, so the address
       field takes what is left. */
    int pad = fb.ui_px / 2;
    int right = width - fb.ui_px / 2;
    int side = fb.bar_height - fb.ui_px / 2;
    int field_x = x + fb.ui_px / 2, field_w = right - 2 * side - pad - fb.ui_px - field_x;
    fb.sections_button = draw_menu_button(right - 2 * side - pad / 2, middle, MENU_SECTIONS);
    fb.settings_button = draw_menu_button(right - side, middle, MENU_SETTINGS);

    if (field_w > fb.ui_px * 4) {
        int field_h = height - fb.ui_px * 2 / 3;
        DRenderRect field = {field_x, middle - field_h / 2, field_w, field_h};
        bool editing = fb.prompt == PROMPT_ADDRESS;
        fill(fb.screen, field.x, field.y, field.width, field.height, p->main_bg);
        d_render_stroke(fb.screen, field, editing ? 2 : 1, editing ? p->link : p->border);
        DRenderRect inner = {field.x + pad, field.y, field.width - 2 * pad, field.height};
        d_render_set_clip(fb.screen, inner);
        if (editing) {
            const char *text = d_line_edit_text(&fb.prompt_edit);
            int caret = fb.ui.font ? d_render_text_x_at(fb.ui.font, text, fb.prompt_edit.cursor) : 0;
            int shift = caret > inner.width - 2 ? caret - (inner.width - 2) : 0;
            draw_text(fb.screen, &fb.ui, inner.x - shift, middle, text, p->text);
            fill(fb.screen, inner.x - shift + caret, middle - fb.ui.metrics.height / 2, 2, fb.ui.metrics.height, p->text);
        } else {
            const char *url = fb.has_page && fb.page.url ? fb.page.url : "";
            draw_text(fb.screen, &fb.ui, inner.x, middle, *url ? url : "Type an address or a search (o)",
                      *url ? p->text : p->muted);
        }
        d_render_del_clip(fb.screen);
        add_button(field, ACTION_ADDRESS, 0);
    }
    if (fb.nav.active) {
        int done = (int)((long long)width * (fb.nav.percent < 3 ? 3 : fb.nav.percent) / 100);
        fill(fb.screen, 0, height - 3, done, 3, p->link);
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

/* The transport: the same controls the desktop draws, in the same place a
   reader looks for them, above the status line. */
static void draw_transport(void)
{
    if (!audio_loaded()) { fb.seek_track = (DRenderRect){0}; return; }
    const Palette *p = palette();
    int y0 = fb.mode.height - fb.status_height - fb.transport_height, height = fb.transport_height;
    int middle = y0 + height / 2, width = fb.mode.width;
    fill(fb.screen, 0, y0, width, height, p->card_bg);
    fill(fb.screen, 0, y0, width, 1, p->border);
    MediaAudioState state;
    media_audio_snapshot(&state);
    int x = fb.ui_px / 2;
    int pad = fb.ui_px / 2;
    int button = fb.ui_px + 2 * pad;
    DRenderRect play_rect = {x, y0 + 2, button, height - 4};
    if (state.phase == MEDIA_AUDIO_PLAYING) {
        int bar = fb.ui_px / 4 > 2 ? fb.ui_px / 4 : 2;
        fill(fb.screen, x + pad + bar / 2, middle - fb.ui_px / 2, bar, fb.ui_px, p->text);
        fill(fb.screen, x + pad + bar * 5 / 2, middle - fb.ui_px / 2, bar, fb.ui_px, p->text);
    } else {
        draw_text(fb.screen, &fb.symbol, x + pad, middle, "\xe2\x96\xb6", p->text);
    }
    add_button(play_rect, ACTION_PLAY_PAUSE, 0);
    x += button + pad;
    char now[16], total[16];
    format_clock(now, sizeof(now), state.position_seconds);
    format_clock(total, sizeof(total), state.duration_seconds);
    x = draw_text(fb.screen, &fb.ui, x, middle, now, p->text) + pad;
    char tail[256];
    snprintf(tail, sizeof(tail), "%s  vol %d%%", total, (int)(state.volume * 100 + 0.5));
    /* From the right: the close button, the title, the time left; the
       position bar takes what remains. */
    int close_w = text_width(&fb.symbol, "\xc3\x97") + 2 * pad;
    int right = width - close_w - fb.ui_px / 2;
    draw_chip(right, middle, &fb.symbol, "\xc3\x97", ACTION_CLOSE_AUDIO, 0, true, false);
    right -= pad;
    int title_w = fb.audio_title ? text_width(&fb.bold, fb.audio_title) : 0;
    if (title_w > width / 4) title_w = width / 4;
    if (title_w) {
        d_render_set_clip(fb.screen, (DRenderRect){right - title_w, y0, title_w, height});
        draw_text(fb.screen, &fb.bold, right - title_w, middle, fb.audio_title, p->text);
        d_render_del_clip(fb.screen);
        right -= title_w + pad;
    }
    int tail_w = text_width(&fb.ui, tail);
    draw_text(fb.screen, &fb.ui, right - tail_w, middle, tail, p->muted);
    right -= tail_w + pad;
    int track_w = right - x;
    if (track_w < fb.ui_px * 2) track_w = fb.ui_px * 2;
    fb.seek_track = (DRenderRect){x, middle - fb.ui_px / 2, track_w, fb.ui_px};
    double fraction = state.duration_seconds > 0 ? state.position_seconds / state.duration_seconds : 0;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    fill(fb.screen, x, middle - 1, track_w, 3, p->border);
    fill(fb.screen, x, middle - 1, (int)(track_w * fraction), 3, p->link);
    d_render_disc(fb.screen, x + track_w * fraction, middle + 0.5, fb.ui_px / 3.0, p->link);
    add_button(fb.seek_track, ACTION_SEEK, 0);
}

static void draw_status(void)
{
    const Palette *p = palette();
    int y0 = fb.mode.height - fb.status_height, height = fb.status_height, middle = y0 + height / 2;
    int width = fb.mode.width, pad = fb.ui_px / 2;
    fill(fb.screen, 0, y0, width, height, p->card_bg);
    fill(fb.screen, 0, y0, width, 1, p->border);
    int hint_w = text_width(&fb.ui, "? keys");
    DRenderRect clip = {pad, y0, width - hint_w - 3 * pad, height};
    d_render_set_clip(fb.screen, clip);
    if (fb.prompt != PROMPT_NONE && fb.prompt != PROMPT_ADDRESS) {
        int x = draw_text(fb.screen, &fb.bold, pad, middle, fb.prompt_label, p->text);
        const char *text = d_line_edit_text(&fb.prompt_edit);
        int caret = fb.ui.font ? d_render_text_x_at(fb.ui.font, text, fb.prompt_edit.cursor) : 0;
        int room = clip.x + clip.width - x;
        int shift = caret > room - 2 ? caret - (room - 2) : 0;
        draw_text(fb.screen, &fb.ui, x - shift, middle, text, p->text);
        fill(fb.screen, x - shift + caret, middle - fb.ui.metrics.height / 2, 2, fb.ui.metrics.height, p->text);
    } else {
        char text[1200];
        if (fb.hover[0]) snprintf(text, sizeof(text), "%s", fb.hover);
        else if (fb.nav.active)
            snprintf(text, sizeof(text), "Loading %d%% %s \xe2\x80\x94 Esc stops", fb.nav.percent, fb.nav.phase);
        else if (fb.transfer)
            snprintf(text, sizeof(text), "%s \xe2\x80\x94 Esc cancels",
                     fb.transfer_kind == TRANSFER_AUDIO ? "Fetching audio\xe2\x80\xa6"
                     : fb.transfer_kind == TRANSFER_IMAGE ? "Fetching the image\xe2\x80\xa6" : "Downloading\xe2\x80\xa6");
        else snprintf(text, sizeof(text), "%s", fb.status ? fb.status : "");
        draw_text(fb.screen, &fb.ui, pad, middle, text, fb.hover[0] ? p->link : p->text);
    }
    d_render_del_clip(fb.screen);
    draw_text(fb.screen, &fb.ui, width - hint_w - pad, middle, "? keys", fb.help ? p->link : p->muted);
    add_button((DRenderRect){width - hint_w - 2 * pad, y0, hint_w + 2 * pad, height}, ACTION_HELP, 0);
}

static void draw_viewer_caption(void)
{
    const Palette *p = palette();
    int y0 = view_top() + view_height() - fb.status_height, height = fb.status_height, middle = y0 + height / 2;
    int width = fb.mode.width;
    fill(fb.screen, 0, y0, width, height, with_alpha(p->card_bg, 235));
    int x = fb.ui_px / 2;
    bool several = fb.viewer.count > 1;
    x = draw_chip(x, middle, &fb.ui, "\xe2\x80\xb9 Previous", ACTION_VIEWER_PREVIOUS, 0, several, false);
    char where[64];
    snprintf(where, sizeof(where), "Image %zu of %zu", fb.viewer.index + 1, fb.viewer.count);
    x = draw_text(fb.screen, &fb.ui, x + fb.ui_px / 2, middle, where, p->muted) + fb.ui_px / 2;
    x = draw_chip(x, middle, &fb.ui, "Next \xe2\x80\xba", ACTION_VIEWER_NEXT, 0, several, false);
    int close_w = text_width(&fb.ui, "Close \xc3\x97") + fb.ui_px;
    int save_w = text_width(&fb.ui, "Save") + fb.ui_px;
    draw_chip(width - close_w - save_w - fb.ui_px, middle, &fb.ui, "Save", ACTION_VIEWER_SAVE, 0, true, false);
    draw_chip(width - close_w - fb.ui_px / 2, middle, &fb.ui, "Close \xc3\x97", ACTION_VIEWER_CLOSE, 0, true, false);
}

static void draw_popup(void)
{
    if (!fb.popup_open) return;
    const Palette *p = palette();
    DFormBox *box = &fb.page.controls[fb.popup.control];
    int row = fb.ui.metrics.height + fb.ui_px / 2, pad = fb.ui_px / 2;
    int width = box->width;
    for (size_t i = 0; i < fb.popup.count; ++i) {
        int w = text_width(&fb.ui, fb.popup.labels[i]) + 4 * pad;
        if (w > width) width = w;
    }
    if (width > fb.mode.width - 2 * pad) width = fb.mode.width - 2 * pad;
    int top = view_top(), view = view_height();
    int visible = (int)fb.popup.count, most = (view - 2 * pad) / row;
    if (visible > most) visible = most > 1 ? most : 1;
    int height = visible * row + 2;
    int x = box->x, y = box->y - fb.page.scroll + top + box->height;
    if (y + height > top + view) y = box->y - fb.page.scroll + top - height;
    if (y < top) y = top;
    if (x + width > fb.mode.width - pad) x = fb.mode.width - pad - width;
    if (x < pad) x = pad;
    fill(fb.screen, x, y, width, height, p->main_bg);
    d_render_stroke(fb.screen, (DRenderRect){x, y, width, height}, 1, p->border);
    size_t first = fb.popup.selected >= (size_t)visible ? fb.popup.selected - (size_t)visible + 1 : 0;
    for (int i = 0; i < visible && first + (size_t)i < fb.popup.count; ++i) {
        size_t index = first + (size_t)i;
        DRenderRect r = {x + 1, y + 1 + i * row, width - 2, row};
        bool selected = index == fb.popup.selected;
        if (selected) fill(fb.screen, r.x, r.y, r.width, r.height, p->link);
        d_render_set_clip(fb.screen, r);
        draw_text(fb.screen, &fb.ui, r.x + pad, r.y + row / 2, fb.popup.labels[index], selected ? p->main_bg : p->text);
        d_render_del_clip(fb.screen);
        add_button(r, ACTION_POPUP_OPTION, (int)index);
    }
}

typedef struct {
    int width, height, row, pad, edge, gutter, arrow_w;
} MenuMetrics;

static MenuMetrics menu_metrics(const MenuItem *items, size_t count)
{
    MenuMetrics m = {.pad = fb.ui_px, .row = fb.ui.metrics.height + fb.ui_px * 3 / 4, .edge = fb.ui_px / 3};
    int label_w = 0, key_w = 0;
    bool arrows = false;
    for (size_t i = 0; i < count; ++i) {
        int w = text_width(&fb.ui, items[i].label);
        if (w > label_w) label_w = w;
        if (items[i].shortcut) {
            int k = text_width(&fb.ui, items[i].shortcut);
            if (k > key_w) key_w = k;
        }
        if (items[i].radio >= 0) m.gutter = fb.ui_px * 5 / 4;
        if (items[i].kind == ITEM_SUBMENU) arrows = true;
    }
    m.arrow_w = arrows ? text_width(&fb.ui, "\xe2\x80\xba") : 0;
    int tail = key_w + m.arrow_w + (key_w && arrows ? m.pad / 2 : 0);
    m.width = m.pad + m.gutter + label_w + (tail ? 2 * m.pad + tail : 0) + m.pad;
    if (m.width < fb.ui_px * 9) m.width = fb.ui_px * 9;
    if (m.width > fb.mode.width) m.width = fb.mode.width;
    m.height = (int)count * m.row + 2 * m.edge;
    return m;
}

/* The menu at `level`, with its top left at x, y as far as the screen
   allows. Records where its rows are, for the pointer. */
static void draw_menu_level(int level, int x, int y)
{
    MenuLevel *menu = &fb.menu[level];
    MenuItem items[MENU_ITEMS_MAX];
    size_t count = menu_items(menu->id, items);
    menu->count = count;
    if (!count) return;
    const Palette *p = palette();
    MenuMetrics m = menu_metrics(items, count);
    int floor = fb.mode.height - fb.status_height;
    if (y + m.height > floor) y = floor - m.height;
    if (y < 0) y = 0;
    if (x + m.width > fb.mode.width) x = fb.mode.width - m.width;
    if (x < 0) x = 0;
    menu->rect = (DRenderRect){x, y, m.width, m.height};
    fill(fb.screen, x + 2, y + 3, m.width, m.height, with_alpha(p->text, 40));
    fill(fb.screen, x, y, m.width, m.height, p->main_bg);
    d_render_stroke(fb.screen, menu->rect, 1, p->border);
    for (size_t i = 0; i < count; ++i) {
        DRenderRect r = {x + 1, y + m.edge + (int)i * m.row, m.width - 2, m.row};
        menu->rows[i] = r;
        bool selected = (int)i == menu->selected && items[i].enabled;
        if (selected) fill(fb.screen, r.x, r.y, r.width, r.height, p->link);
        DRenderColor ink = selected ? p->main_bg : items[i].enabled ? p->text : with_alpha(p->muted, 140);
        DRenderColor quiet = selected ? p->main_bg : p->muted;
        int middle = r.y + m.row / 2;
        if (items[i].radio >= 0) {
            double cx = x + m.pad + fb.ui_px * 0.35, radius = fb.ui_px * 0.32;
            d_render_disc(fb.screen, cx, middle, radius, quiet);
            d_render_disc(fb.screen, cx, middle, radius - 1.2, selected ? p->link : p->main_bg);
            if (items[i].radio) d_render_disc(fb.screen, cx, middle, radius * 0.55, ink);
        }
        d_render_set_clip(fb.screen, r);
        draw_text(fb.screen, &fb.ui, x + m.pad + m.gutter, middle, items[i].label, ink);
        int right = x + m.width - m.pad;
        if (items[i].kind == ITEM_SUBMENU) {
            draw_text(fb.screen, &fb.ui, right - m.arrow_w, middle, "\xe2\x80\xba", ink);
            right -= m.arrow_w + m.pad / 2;
        }
        if (items[i].shortcut)
            draw_text(fb.screen, &fb.ui, right - text_width(&fb.ui, items[i].shortcut), middle,
                      items[i].shortcut, quiet);
        d_render_del_clip(fb.screen);
        add_button(r, ACTION_MENU_ITEM, level * MENU_ITEMS_MAX + (int)i);
    }
}

static void draw_menus(void)
{
    if (fb.menu[0].id == MENU_NONE) return;
    MenuItem items[MENU_ITEMS_MAX];
    MenuMetrics top = menu_metrics(items, menu_items(fb.menu[0].id, items));
    DRenderRect anchor = fb.menu[0].id == MENU_SECTIONS ? fb.sections_button : fb.settings_button;
    draw_menu_level(0, anchor.x + anchor.width - top.width, fb.bar_height);
    if (fb.menu[1].id == MENU_NONE || fb.menu[0].selected < 0) return;
    /* A submenu opens beside its item: to the left, since the menus hang
       from the right edge, or to the right when the left has no room. */
    const MenuLevel *parent = &fb.menu[0];
    MenuMetrics sub = menu_metrics(items, menu_items(fb.menu[1].id, items));
    int x = parent->rect.x + 2 - sub.width;
    if (x < 0) x = parent->rect.x + parent->rect.width - 2;
    draw_menu_level(1, x, parent->rows[parent->selected].y - top.edge);
}

typedef struct {
    const char *keys, *what;
} HelpEntry;

typedef struct {
    const char *title;
    HelpEntry entries[8];
} HelpGroup;

static const HelpGroup help_groups[] = {
    {"Reading", {{"j k \xe2\x86\x93 \xe2\x86\x91  wheel", "scroll"},
                 {"Space PgDn  b PgUp", "a page down, up"},
                 {"g Home  G End", "top, bottom"},
                 {"Alt-0\xe2\x80\xa6" "6", "top, images, tables, forms\xe2\x80\xa6"},
                 {"+ \xe2\x88\x92 0", "type size"},
                 {"t", "theme"}}},
    {"Links and fields", {{"Tab  S-Tab", "next, previous"},
                          {"Enter  click", "follow, press"},
                          {"type", "into a focused field"},
                          {"Esc", "let go of it"}}},
    {"Going places", {{"o  Ctrl-L", "address or search"},
                      {"Backspace  H", "back"},
                      {"L", "forward"},
                      {"r F5", "reload"},
                      {"Esc", "stop loading"}}},
    {"Audio", {{"Space", "play, pause"},
               {"\xe2\x86\x90 \xe2\x86\x92", "seek 5 seconds"},
               {"[ ]", "volume"},
               {"s  x", "stop, close"}}},
    {"Pictures", {{"\xe2\x86\x90 \xe2\x86\x92  n p", "previous, next"},
                  {"+ \xe2\x88\x92 0  wheel", "zoom"},
                  {"d", "save"},
                  {"Esc", "close"}}},
    {"Distill", {{"F10", "the settings menu"},
                 {"?", "these keys"},
                 {"q  Ctrl-Q", "quit"}}},
};

static void draw_help(void)
{
    if (!fb.help) return;
    const Palette *p = palette();
    enum { GROUPS = sizeof(help_groups) / sizeof(help_groups[0]) };
    int row = fb.ui.metrics.height + fb.ui_px / 3, pad = fb.ui_px;
    int key_w = 0, what_w = 0;
    for (size_t g = 0; g < GROUPS; ++g)
        for (int e = 0; e < 8 && help_groups[g].entries[e].keys; ++e) {
            int k = text_width(&fb.bold, help_groups[g].entries[e].keys);
            int w = text_width(&fb.ui, help_groups[g].entries[e].what);
            if (k > key_w) key_w = k;
            if (w > what_w) what_w = w;
        }
    int column = key_w + pad + what_w;
    bool two = 2 * column + 5 * pad <= fb.mode.width - 2 * pad;
    static const size_t left[] = {0, 1, 5}, right[] = {2, 3, 4}, single[] = {0, 1, 2, 3, 4, 5};
    int rows[2] = {0, 0};
    for (int c = 0; c < (two ? 2 : 1); ++c) {
        const size_t *order = two ? (c ? right : left) : single;
        size_t count = two ? 3 : GROUPS;
        for (size_t i = 0; i < count; ++i) {
            int n = 0;
            while (n < 8 && help_groups[order[i]].entries[n].keys) ++n;
            rows[c] += (i ? 1 : 0) + 1 + n;
        }
    }
    int content = rows[0] > rows[1] ? rows[0] : rows[1];
    int width = (two ? 2 * column + pad * 2 : column) + 2 * pad;
    int height = (content + 1) * row + 2 * pad;
    int x0 = (fb.mode.width - width) / 2, y0 = view_top() + (view_height() - height) / 2;
    if (y0 < view_top()) y0 = view_top();
    fill(fb.screen, x0, y0, width, height, p->card_bg);
    d_render_stroke(fb.screen, (DRenderRect){x0, y0, width, height}, 1, p->border);
    draw_text(fb.screen, &fb.bold, x0 + pad, y0 + pad + row / 2, "Keys", p->heading);
    for (int c = 0; c < (two ? 2 : 1); ++c) {
        const size_t *order = two ? (c ? right : left) : single;
        size_t count = two ? 3 : GROUPS;
        int x = x0 + pad + c * (column + pad * 2), y = y0 + pad + row + row / 2;
        for (size_t i = 0; i < count; ++i) {
            if (i) y += row;
            const HelpGroup *group = &help_groups[order[i]];
            draw_text(fb.screen, &fb.bold, x, y, group->title, p->heading);
            y += row;
            for (int e = 0; e < 8 && group->entries[e].keys; ++e) {
                draw_text(fb.screen, &fb.bold, x, y, group->entries[e].keys, p->link);
                draw_text(fb.screen, &fb.ui, x + key_w + pad, y, group->entries[e].what, p->text);
                y += row;
            }
        }
    }
}

static void ensure_screen(void)
{
    if (fb.screen && fb.screen_width == fb.mode.width && fb.screen_height == fb.mode.height) return;
    font_free(&fb.ui); font_free(&fb.bold); font_free(&fb.symbol);
    font_free(&fb.control); font_free(&fb.control_bold);
    if (fb.screen) d_render_destroy(fb.screen);
    fb.screen = d_render_create(fb.mode.width, fb.mode.height, fb.font_dir);
    fb.screen_width = fb.mode.width;
    fb.screen_height = fb.mode.height;
    metrics_for_mode();
    make_fonts();
}

static void render(void)
{
    ensure_screen();
    if (!fb.screen) return;
    const Palette *p = palette();
    fb.button_count = 0;
    clamp_scroll();
    int top = view_top();
    if (fb.viewer.active) {
        if (!fb.viewer.layer_valid) paint_viewer_layer();
        if (fb.viewer.layer) {
            DRenderPixels pixels;
            d_render_pixels(fb.viewer.layer, &pixels);
            pixels.height = view_height();
            d_render_blit(fb.screen, 0, top, &pixels);
        }
        draw_viewer_caption();
    } else if (fb.has_page && fb.page.doc) {
        if (fb.page_dirty) paint_page_layer();
        DRenderPixels pixels;
        if (d_page_pixels(fb.page.doc, &pixels)) {
            if (pixels.height > view_height()) pixels.height = view_height();
            d_render_blit(fb.screen, 0, top, &pixels);
        }
        draw_scrollbar();
    } else {
        fill(fb.screen, 0, top, fb.mode.width, view_height(), p->page_bg);
        draw_text(fb.screen, &fb.ui, fb.ui_px * 2, top + fb.ui_px * 2,
                  fb.nav.active ? "Loading\xe2\x80\xa6" : "o goes to an address \xe2\x80\x94 ? for keys", p->muted);
    }
    draw_top_bar();
    draw_transport();
    draw_status();
    draw_popup();
    draw_menus();
    draw_help();
    DRenderPixels screen;
    if (d_render_pixels(fb.screen, &screen)) {
        DRenderRect all = {0, 0, fb.mode.width, fb.mode.height};
        d_display_present(fb.display, screen.data, screen.stride, &all, 1);
    }
    fb.dirty = false;
}

/* ---- state, for the gates ------------------------------------------------- */

static void json_string(DBuffer *out, const char *text)
{
    dbuf_puts(out, "\"");
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') { char pair[2] = {'\\', (char)*p}; dbuf_append(out, pair, 2); }
        else if (*p < 0x20) dbuf_printf(out, "\\u%04x", *p);
        else dbuf_append(out, (const char *)p, 1);
    }
    dbuf_puts(out, "\"");
}

static const char *phase_name(MediaAudioPhase phase)
{
    switch (phase) {
    case MEDIA_AUDIO_LOADING: return "loading";
    case MEDIA_AUDIO_PLAYING: return "playing";
    case MEDIA_AUDIO_PAUSED: return "paused";
    case MEDIA_AUDIO_FINISHED: return "finished";
    case MEDIA_AUDIO_ERROR: return "error";
    default: return "stopped";
    }
}

static char *state_line(void)
{
    DBuffer out;
    dbuf_init(&out);
    Page *page = &fb.page;
    dbuf_puts(&out, "{\"url\":");
    json_string(&out, fb.has_page ? page->url : "");
    dbuf_puts(&out, ",\"title\":");
    json_string(&out, fb.has_page ? page->title : "");
    dbuf_printf(&out, ",\"loading\":%s,\"transfer\":%s,\"status\":", fb.nav.active ? "true" : "false",
                fb.transfer ? "true" : "false");
    json_string(&out, fb.status);
    dbuf_printf(&out, ",\"scroll\":%d,\"page_height\":%d,\"view_top\":%d,\"viewport\":%d,\"width\":%d,\"height\":%d",
                page->scroll, page->height, view_top(), view_height(), fb.mode.width, fb.mode.height);
    dbuf_printf(&out, ",\"theme\":\"%s\",\"zoom\":%d,\"back\":%s,\"forward\":%s", fb.theme, fb.zoom,
                d_app_history_can_back(&fb.history) ? "true" : "false",
                d_app_history_can_forward(&fb.history) ? "true" : "false");
    dbuf_printf(&out, ",\"links\":%zu,\"focusable\":%zu", fb.has_page ? page->link_count : 0,
                fb.has_page ? page->focus_count : 0);
    dbuf_puts(&out, ",\"focus\":");
    if (fb.has_page && page->focus >= 0) {
        Focus *focus = &page->focusables[page->focus];
        int x, y, w, h;
        if (focus->kind == FOCUS_LINK) {
            DLinkBox *link = &page->links[focus->index];
            dbuf_puts(&out, "{\"kind\":\"link\",\"href\":");
            json_string(&out, link->href);
            x = link->x; y = link->y; w = link->width; h = link->height;
        } else {
            DFormBox *box = &page->controls[focus->index];
            dbuf_puts(&out, "{\"kind\":\"control\",\"name\":");
            json_string(&out, box->name);
            dbuf_puts(&out, ",\"type\":");
            json_string(&out, box->type);
            x = box->x; y = box->y; w = box->width; h = box->height;
        }
        /* Document coordinates: on screen, y - scroll + view_top. */
        dbuf_printf(&out, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", x, y, w, h);
    } else {
        dbuf_puts(&out, "null");
    }
    dbuf_printf(&out, ",\"editing\":%s,\"prompt\":", fb.editing ? "true" : "false");
    if (fb.prompt) {
        static const char *const kinds[] = {"", "address", "save-as", "export-html", "export-markdown"};
        dbuf_printf(&out, "{\"kind\":\"%s\",\"text\":", kinds[fb.prompt]);
        json_string(&out, d_line_edit_text(&fb.prompt_edit));
        dbuf_puts(&out, "}");
    } else {
        dbuf_puts(&out, "null");
    }
    dbuf_puts(&out, ",\"controls\":[");
    for (size_t i = 0; fb.has_page && i < page->control_count; ++i) {
        if (i) dbuf_puts(&out, ",");
        dbuf_puts(&out, "{\"name\":");
        json_string(&out, page->controls[i].name);
        dbuf_puts(&out, ",\"value\":");
        const char *value = fb.editing && fb.field_control == i ? d_line_edit_text(&fb.field) : page->values[i];
        /* A password is not written anywhere, not even to a gate's state. */
        char *shown = control_type(&page->controls[i], "password") ? masked(value, NULL) : NULL;
        json_string(&out, shown ? shown : value);
        free(shown);
        dbuf_printf(&out, ",\"checked\":%s}", page->checked[i] ? "true" : "false");
    }
    dbuf_puts(&out, "],\"viewer\":");
    if (fb.viewer.active) {
        dbuf_printf(&out, "{\"index\":%zu,\"count\":%zu,\"loaded\":%s,\"width\":%d,\"height\":%d,\"url\":",
                    fb.viewer.index, fb.viewer.count, fb.viewer.loaded ? "true" : "false",
                    fb.viewer.loaded ? fb.viewer.image.width : 0, fb.viewer.loaded ? fb.viewer.image.height : 0);
        json_string(&out, fb.viewer.urls[fb.viewer.index]);
        dbuf_puts(&out, "}");
    } else {
        dbuf_puts(&out, "null");
    }
    dbuf_puts(&out, ",\"audio\":");
    if (audio_loaded()) {
        MediaAudioState state;
        media_audio_snapshot(&state);
        dbuf_printf(&out, "{\"phase\":\"%s\",\"title\":", phase_name(state.phase));
        json_string(&out, fb.audio_title);
        dbuf_puts(&out, "}");
    } else {
        dbuf_puts(&out, "null");
    }
    dbuf_puts(&out, ",\"menu\":");
    if (fb.menu[0].id != MENU_NONE) {
        dbuf_printf(&out, "{\"open\":\"%s\",\"submenu\":", menu_names[fb.menu[0].id]);
        if (fb.menu[1].id != MENU_NONE) dbuf_printf(&out, "\"%s\"", menu_names[fb.menu[1].id]);
        else dbuf_puts(&out, "null");
        dbuf_printf(&out, ",\"in_submenu\":%s", fb.in_submenu ? "true" : "false");
        /* Each list as drawn, so a gate can find an item by its label. */
        for (int level = 0; level < 2; ++level) {
            dbuf_puts(&out, level ? ",\"subitems\":[" : ",\"items\":[");
            MenuItem items[MENU_ITEMS_MAX];
            size_t count = fb.menu[level].id != MENU_NONE ? menu_items(fb.menu[level].id, items) : 0;
            for (size_t i = 0; i < count; ++i) {
                DRenderRect r = i < fb.menu[level].count ? fb.menu[level].rows[i] : (DRenderRect){0, 0, 0, 0};
                if (i) dbuf_puts(&out, ",");
                dbuf_puts(&out, "{\"label\":");
                json_string(&out, items[i].label);
                dbuf_printf(&out, ",\"enabled\":%s,\"checked\":%s,\"selected\":%s,"
                            "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                            items[i].enabled ? "true" : "false", items[i].radio > 0 ? "true" : "false",
                            (int)i == fb.menu[level].selected ? "true" : "false", r.x, r.y, r.width, r.height);
            }
            dbuf_puts(&out, "]");
        }
        dbuf_puts(&out, "}");
    } else {
        dbuf_puts(&out, "null");
    }
    dbuf_printf(&out, ",\"buttons\":{\"sections\":[%d,%d,%d,%d],\"settings\":[%d,%d,%d,%d]}",
                fb.sections_button.x, fb.sections_button.y, fb.sections_button.width, fb.sections_button.height,
                fb.settings_button.x, fb.settings_button.y, fb.settings_button.width, fb.settings_button.height);
    dbuf_printf(&out, ",\"help\":%s,\"popup\":%s,\"suspended\":%s,\"hover\":", fb.help ? "true" : "false",
                fb.popup_open ? "true" : "false", fb.suspended ? "true" : "false");
    json_string(&out, fb.hover);
    dbuf_puts(&out, "}");
    return dbuf_take(&out);
}

static void report(void)
{
    char *state = state_line();
    char title[600];
    snprintf(title, sizeof(title), "%s%sDistill", fb.has_page && *fb.page.title ? fb.page.title : "",
             fb.has_page && *fb.page.title ? " \xe2\x80\x94 " : "");
    d_display_report(fb.display, fb.nav.active || fb.transfer, state, title);
    free(state);
}

/* ---- input ---------------------------------------------------------------- */

static bool is_char(const DEvent *event, uint32_t c)
{
    return event->key == D_KEY_CHAR && event->codepoint == c && !(event->mods & (D_MOD_CTRL | D_MOD_ALT));
}

static bool is_ctrl(const DEvent *event, uint32_t c)
{
    return event->key == D_KEY_CHAR && event->codepoint == c && (event->mods & D_MOD_CTRL);
}

static void edit_key(DLineEdit *edit, const DEvent *event, bool lines)
{
    switch (event->key) {
    case D_KEY_BACKSPACE: d_line_edit_backspace(edit); return;
    case D_KEY_DELETE: d_line_edit_delete(edit); return;
    case D_KEY_LEFT: d_line_edit_left(edit); return;
    case D_KEY_RIGHT: d_line_edit_right(edit); return;
    case D_KEY_HOME: if (lines) d_line_edit_line_home(edit); else d_line_edit_home(edit); return;
    case D_KEY_END: if (lines) d_line_edit_line_end(edit); else d_line_edit_end(edit); return;
    case D_KEY_UP: if (lines) d_line_edit_line_up(edit); return;
    case D_KEY_DOWN: if (lines) d_line_edit_line_down(edit); return;
    default: break;
    }
    if (is_ctrl(event, 'a')) d_line_edit_home(edit);
    else if (is_ctrl(event, 'e')) d_line_edit_end(edit);
    else if (is_ctrl(event, 'u')) d_line_edit_kill_before(edit);
    else if (is_ctrl(event, 'w')) d_line_edit_delete_word(edit);
    else if (event->key == D_KEY_CHAR && !(event->mods & (D_MOD_CTRL | D_MOD_ALT)))
        d_line_edit_insert_codepoint(edit, event->codepoint);
}

static void prompt_commit(void)
{
    const char *text = d_line_edit_text(&fb.prompt_edit);
    if (fb.prompt == PROMPT_ADDRESS) {
        bool guessed = false;
        char *url = d_address_resolve(text, &guessed);
        prompt_close();
        if (!url) { set_status("Type an address or a search"); return; }
        navigate(url, NULL, NULL, guessed);
        free(url);
    } else {
        PromptKind kind = fb.prompt;
        char *url = fb.prompt_url ? d_strdup(fb.prompt_url) : NULL;
        char *path = d_strdup(text);
        if (!*path) { free(url); free(path); return; }
        if (d_fs_exists(path)) {
            set_status("%s exists; choose another name", path);
            free(url); free(path);
            return;
        }
        prompt_close();
        if (kind == PROMPT_SAVE_AS && url) transfer_start(url, path, TRANSFER_DOWNLOAD);
        else if (kind != PROMPT_SAVE_AS) export_to(kind == PROMPT_EXPORT_MARKDOWN, path);
        free(url); free(path);
    }
}

static void prompt_key(const DEvent *event)
{
    fb.dirty = true;
    if (event->key == D_KEY_ESCAPE) prompt_close();
    else if (event->key == D_KEY_ENTER) prompt_commit();
    else edit_key(&fb.prompt_edit, event, false);
}

static void popup_key(const DEvent *event)
{
    fb.dirty = true;
    if (event->key == D_KEY_ESCAPE) popup_close(false);
    else if (event->key == D_KEY_ENTER || is_char(event, ' ')) popup_close(true);
    else if ((event->key == D_KEY_UP || is_char(event, 'k')) && fb.popup.selected) --fb.popup.selected;
    else if ((event->key == D_KEY_DOWN || is_char(event, 'j')) && fb.popup.selected + 1 < fb.popup.count)
        ++fb.popup.selected;
}

/* Keys while a menu is open: the desktop's, arrows through the items, Right
   or Enter into a submenu, Left or Esc back out of it. */
static void menu_key(const DEvent *event)
{
    fb.dirty = true;
    int level = fb.in_submenu ? 1 : 0;
    MenuLevel *menu = &fb.menu[level];
    if (event->key == D_KEY_ESCAPE || event->key == D_KEY_F1 + 9 ||
        (event->key == D_KEY_LEFT && level == 1)) {
        if (level == 1 && event->key != D_KEY_F1 + 9) {
            fb.menu[1].id = MENU_NONE;
            fb.in_submenu = false;
        } else {
            menu_close();
        }
    } else if (event->key == D_KEY_DOWN || is_char(event, 'j')) {
        menu->selected = menu_step(menu->id, menu->selected, 1);
    } else if (event->key == D_KEY_UP || is_char(event, 'k')) {
        menu->selected = menu_step(menu->id, menu->selected < 0 ? 0 : menu->selected, -1);
    } else if (event->key == D_KEY_HOME) {
        menu->selected = menu_step(menu->id, -1, 1);
    } else if (event->key == D_KEY_END) {
        menu->selected = menu_step(menu->id, 0, -1);
    } else if (event->key == D_KEY_ENTER || is_char(event, ' ') ||
               (event->key == D_KEY_RIGHT && level == 0)) {
        MenuItem items[MENU_ITEMS_MAX];
        size_t count = menu_items(menu->id, items);
        if (menu->selected < 0 || (size_t)menu->selected >= count) return;
        if (event->key == D_KEY_RIGHT && items[menu->selected].kind != ITEM_SUBMENU) return;
        menu_choose(level, menu->selected);
    } else {
        return;
    }
    /* A submenu shows while its item is selected, as the pointer opens it. */
    if (!fb.in_submenu && fb.menu[0].id != MENU_NONE) {
        MenuItem items[MENU_ITEMS_MAX];
        size_t count = menu_items(fb.menu[0].id, items);
        int at = fb.menu[0].selected;
        if (at < 0 || (size_t)at >= count || items[at].kind != ITEM_SUBMENU) fb.menu[1].id = MENU_NONE;
    }
}

/* Keys for a text control with focus. False for a key it leaves to the page. */
static bool field_key(const DEvent *event)
{
    size_t control = fb.field_control;
    bool area = !strcmp(fb.page.controls[control].tag, "textarea");
    if (event->key == D_KEY_TAB) return false;
    if (event->key == D_KEY_ESCAPE) { store_field(); unfocus(); return true; }
    if (event->key == D_KEY_ENTER) {
        if (area) { d_line_edit_insert(&fb.field, "\n", 1); store_field(); return true; }
        /* Enter in a text field submits its form, with the field as the
           submitter: what the desktop does, and what a browser does. */
        store_field();
        submit_form(control);
        return true;
    }
    if (event->key == D_KEY_PAGE_UP || event->key == D_KEY_PAGE_DOWN) return false;
    if ((event->mods & D_MOD_ALT) || is_ctrl(event, 'l') || is_ctrl(event, 'q') || is_ctrl(event, 'c'))
        return false;
    edit_key(&fb.field, event, area);
    store_field();
    return true;
}

static void viewer_key(const DEvent *event)
{
    fb.dirty = true;
    bool zoomed = fb.viewer.zoom > 1.001;
    int step = fb.ui_px * 4;
    if (event->key == D_KEY_ESCAPE || event->key == D_KEY_BACKSPACE || is_char(event, 'q')) viewer_leave();
    else if (is_char(event, '+') || is_char(event, '=')) viewer_zoom(1.25);
    else if (is_char(event, '-')) viewer_zoom(0.8);
    else if (is_char(event, '0')) viewer_zoom(0);
    else if (is_char(event, 'd')) download(fb.viewer.urls[fb.viewer.index]);
    else if (is_char(event, 'p') || (!zoomed && event->key == D_KEY_LEFT)) viewer_step(-1);
    else if (is_char(event, 'n') || is_char(event, ' ') || (!zoomed && event->key == D_KEY_RIGHT)) viewer_step(1);
    else if (event->key == D_KEY_LEFT) viewer_pan(step, 0);
    else if (event->key == D_KEY_RIGHT) viewer_pan(-step, 0);
    else if (event->key == D_KEY_UP) viewer_pan(0, step);
    else if (event->key == D_KEY_DOWN) viewer_pan(0, -step);
}

static bool audio_key(const DEvent *event)
{
    if (!audio_loaded()) return false;
    MediaAudioState state;
    media_audio_snapshot(&state);
    if (is_char(event, ' ')) toggle_play();
    else if (is_char(event, 's')) media_audio_stop();
    else if (event->key == D_KEY_LEFT && !event->mods) seek_relative(-5);
    else if (event->key == D_KEY_RIGHT && !event->mods) seek_relative(5);
    else if (is_char(event, '[') || is_char(event, ']')) {
        float volume = state.volume + (event->codepoint == ']' ? 0.1f : -0.1f);
        media_audio_set_volume(volume < 0 ? 0 : volume > 1 ? 1 : volume);
    } else if (is_char(event, 'x')) {
        audio_release();
        set_status("Closed the track");
    } else {
        return false;
    }
    fb.dirty = true;
    return true;
}

static void open_address(bool keep)
{
    stop_editing();
    prompt_open(PROMPT_ADDRESS, "Go to: ", keep && fb.has_page && fb.page.url ? fb.page.url : "");
}

static void page_key(const DEvent *event)
{
    int page = view_height() - line_step() > line_step() ? view_height() - line_step() : line_step();
    fb.dirty = true;
    if (event->mods & D_MOD_ALT) {
        if (event->key == D_KEY_CHAR && event->codepoint >= '0' && event->codepoint <= '6') {
            jump_to_section(section_ids[event->codepoint - '0']);
        } else if (event->key == D_KEY_LEFT) {
            go_back();
        } else if (event->key == D_KEY_RIGHT) {
            go_forward();
        }
        return;
    }
    /* Space presses what has focus, as it does in any browser: a checkbox,
       a radio button, a list, a button. On a link or nothing it pages. */
    if (is_char(event, ' ') && fb.page.focus >= 0 &&
        fb.page.focusables[fb.page.focus].kind == FOCUS_CONTROL) {
        activate_focus();
        return;
    }
    if (audio_key(event)) return;
    switch (event->key) {
    case D_KEY_DOWN: scroll_by(line_step()); return;
    case D_KEY_UP: scroll_by(-line_step()); return;
    case D_KEY_PAGE_DOWN: scroll_by(page); return;
    case D_KEY_PAGE_UP: scroll_by(-page); return;
    case D_KEY_HOME: scroll_to(0); return;
    case D_KEY_END: scroll_to(INT_MAX / 2); return;
    case D_KEY_TAB: move_focus(event->mods & D_MOD_SHIFT ? -1 : 1); return;
    case D_KEY_ENTER: activate_focus(); return;
    case D_KEY_BACKSPACE: go_back(); return;
    case D_KEY_ESCAPE:
        if (fb.nav.active) nav_cancel();
        else if (fb.transfer) transfer_cancel();
        else unfocus();
        return;
    default: break;
    }
    if (event->key == D_KEY_F1 + 4) { reload(); return; }
    if (event->key == D_KEY_F1 + 5 || is_ctrl(event, 'l')) { open_address(true); return; }
    if (is_ctrl(event, 'f')) { scroll_by(page); return; }
    if (is_ctrl(event, 'b')) { scroll_by(-page); return; }
    if (event->key != D_KEY_CHAR || (event->mods & D_MOD_CTRL)) return;
    switch (event->codepoint) {
    case 'j': scroll_by(line_step()); break;
    case 'k': scroll_by(-line_step()); break;
    case ' ': scroll_by(page); break;
    case 'b': scroll_by(-page); break;
    case 'g': scroll_to(0); break;
    case 'G': scroll_to(INT_MAX / 2); break;
    case 'o': open_address(false); break;
    case 'H': go_back(); break;
    case 'L': go_forward(); break;
    case 'r': reload(); break;
    case 't': cycle_theme(); break;
    case '+': case '=': step_zoom(1); break;
    case '-': step_zoom(-1); break;
    case '0': set_zoom(100); break;
    case '?': fb.help = true; break;
    case 'q': fb.quit = true; break;
    default: break;
    }
}

static void handle_key(const DEvent *event)
{
    fb.dirty = true;
    if (is_ctrl(event, 'q') || is_ctrl(event, 'c')) { fb.quit = true; return; }
    if (fb.help) { fb.help = false; return; }
    if (fb.prompt) { prompt_key(event); return; }
    if (fb.menu[0].id != MENU_NONE) { menu_key(event); return; }
    if (event->key == D_KEY_F1 + 9) { menu_open(MENU_SETTINGS, true); return; }
    if (fb.popup_open) { popup_key(event); return; }
    if (fb.viewer.active) { viewer_key(event); return; }
    if (fb.editing && field_key(event)) return;
    page_key(event);
}

static void do_action(const Button *button, int x, int y)
{
    fb.dirty = true;
    switch (button->action) {
    case ACTION_BACK: go_back(); break;
    case ACTION_FORWARD: go_forward(); break;
    case ACTION_RELOAD: if (fb.nav.active) nav_cancel(); else reload(); break;
    case ACTION_ADDRESS: open_address(true); break;
    case ACTION_MENU:
        if (fb.menu[0].id == (MenuId)button->arg) menu_close();
        else menu_open((MenuId)button->arg, false);
        break;
    case ACTION_MENU_ITEM:
        fb.in_submenu = button->arg >= MENU_ITEMS_MAX;
        menu_choose(button->arg / MENU_ITEMS_MAX, button->arg % MENU_ITEMS_MAX);
        break;
    case ACTION_HELP: fb.help = !fb.help; break;
    case ACTION_PLAY_PAUSE: toggle_play(); break;
    case ACTION_CLOSE_AUDIO: audio_release(); set_status("Closed the track"); break;
    case ACTION_SEEK: fb.drag = DRAG_SEEK; seek_to_x(x); break;
    case ACTION_VIEWER_PREVIOUS: viewer_step(-1); break;
    case ACTION_VIEWER_NEXT: viewer_step(1); break;
    case ACTION_VIEWER_SAVE: download(fb.viewer.urls[fb.viewer.index]); break;
    case ACTION_VIEWER_CLOSE: viewer_leave(); break;
    case ACTION_POPUP_OPTION:
        fb.popup.selected = (size_t)button->arg;
        popup_close(true);
        break;
    case ACTION_SCROLLBAR: {
        int thumb_top = button->rect.y + button->arg;
        int view = view_height();
        int thumb = (int)((long long)view * view / (fb.page.height ? fb.page.height : 1));
        if (thumb < fb.ui_px * 2) thumb = fb.ui_px * 2;
        if (y >= thumb_top && y < thumb_top + thumb) {
            fb.drag = DRAG_SCROLLBAR;
            fb.drag_offset = y - thumb_top;
        } else {
            int page = view - line_step();
            scroll_by(y < thumb_top ? -page : page);
        }
        break;
    }
    default: break;
    }
}

static void drag_scrollbar(int y)
{
    int view = view_height();
    int thumb = (int)((long long)view * view / (fb.page.height ? fb.page.height : 1));
    if (thumb < fb.ui_px * 2) thumb = fb.ui_px * 2;
    if (thumb >= view) return;
    double fraction = (double)(y - fb.drag_offset - view_top()) / (view - thumb);
    scroll_to((int)(fraction * max_scroll() + 0.5));
}

/* The link under a screen point, into `href`; false when there is none. */
static bool link_at(int x, int y, char *href, size_t capacity)
{
    href[0] = '\0';
    if (!fb.has_page || !fb.page.doc || fb.viewer.active) return false;
    if (y < view_top() || y >= view_top() + view_height()) return false;
    return d_page_link_at(fb.page.doc, x, y - view_top() + fb.page.scroll, href, capacity);
}

static int control_at(int x, int y)
{
    if (!fb.has_page || fb.viewer.active || y < view_top() || y >= view_top() + view_height()) return -1;
    int doc_y = y - view_top() + fb.page.scroll;
    for (size_t i = 0; i < fb.page.control_count; ++i) {
        DFormBox *box = &fb.page.controls[i];
        if (control_visible(box) && x >= box->x && x < box->x + box->width && doc_y >= box->y &&
            doc_y < box->y + box->height)
            return (int)i;
    }
    /* The label of a checkbox or radio clicks it, as a browser's does. */
    char id[64];
    if (d_page_choice_id_at(fb.page.doc, x, doc_y, id, sizeof(id)))
        for (size_t i = 0; i < fb.page.control_count; ++i)
            if (!strcmp(fb.page.controls[i].id, id)) return (int)i;
    return -1;
}

static void pointer_press(const DEvent *event)
{
    fb.dirty = true;
    if (fb.help) { fb.help = false; return; }
    /* An open menu takes the click: on it or its button, or anywhere else to
       close it, as the desktop's does. */
    if (fb.menu[0].id != MENU_NONE) {
        for (size_t i = fb.button_count; i-- > 0;) {
            Action action = fb.buttons[i].action;
            if ((action == ACTION_MENU || action == ACTION_MENU_ITEM) &&
                inside(fb.buttons[i].rect, event->x, event->y)) {
                do_action(&fb.buttons[i], event->x, event->y);
                return;
            }
        }
        menu_close();
        return;
    }
    for (size_t i = fb.button_count; i-- > 0;) {
        if (!inside(fb.buttons[i].rect, event->x, event->y)) continue;
        if (fb.popup_open && fb.buttons[i].action != ACTION_POPUP_OPTION) popup_close(false);
        if (fb.prompt == PROMPT_ADDRESS && fb.buttons[i].action != ACTION_ADDRESS) prompt_close();
        do_action(&fb.buttons[i], event->x, event->y);
        return;
    }
    if (fb.popup_open) { popup_close(false); return; }
    if (fb.prompt == PROMPT_ADDRESS) prompt_close();
    if (fb.viewer.active) return;
    int control = control_at(event->x, event->y);
    if (control >= 0) {
        focus_on(FOCUS_CONTROL, (size_t)control);
        if (!control_is_text(&fb.page.controls[control])) activate_control((size_t)control);
        return;
    }
    char href[2048];
    if (link_at(event->x, event->y, href, sizeof(href))) {
        for (size_t i = 0; i < fb.page.link_count; ++i)
            if (!strcmp(fb.page.links[i].href, href)) { focus_on(FOCUS_LINK, i); break; }
        follow_href(href);
        return;
    }
    unfocus();
}

static void pointer_moved(const DEvent *event)
{
    fb.pointer_x = event->x;
    fb.pointer_y = event->y;
    if (fb.drag == DRAG_SCROLLBAR) { drag_scrollbar(event->y); return; }
    if (fb.drag == DRAG_SEEK) { seek_to_x(event->x); return; }
    if (fb.menu[0].id != MENU_NONE) {
        /* The pointer selects what it is over, and opens a submenu by
           resting on its item; the page under a menu does not hover. */
        for (size_t i = fb.button_count; i-- > 0;) {
            if (fb.buttons[i].action != ACTION_MENU_ITEM || !inside(fb.buttons[i].rect, event->x, event->y))
                continue;
            int level = fb.buttons[i].arg / MENU_ITEMS_MAX, index = fb.buttons[i].arg % MENU_ITEMS_MAX;
            MenuItem items[MENU_ITEMS_MAX];
            size_t count = menu_items(fb.menu[level].id, items);
            if ((size_t)index >= count) return;
            if (fb.menu[level].selected != (items[index].enabled ? index : -1)) fb.dirty = true;
            fb.menu[level].selected = items[index].enabled ? index : -1;
            fb.in_submenu = level == 1;
            if (level == 0) {
                if (items[index].kind == ITEM_SUBMENU && items[index].enabled)
                    submenu_open((MenuId)items[index].arg, false);
                else if (fb.menu[1].id != MENU_NONE) { fb.menu[1].id = MENU_NONE; fb.dirty = true; }
            }
            return;
        }
        return;
    }
    char href[2048];
    link_at(event->x, event->y, href, sizeof(href));
    if (strcmp(href, fb.hover)) {
        snprintf(fb.hover, sizeof(fb.hover), "%s", href);
        fb.dirty = true;
    }
}

static void resized(void)
{
    fb.mode = d_display_mode(fb.display);
    ensure_screen();
    if (fb.has_page) layout_page(&fb.page);
    fb.viewer.layer_valid = false;
    fb.dirty = fb.page_dirty = true;
}

static void handle(const DEvent *event)
{
    switch (event->kind) {
    case D_EVENT_KEY: handle_key(event); break;
    case D_EVENT_POINTER_MOVE: pointer_moved(event); break;
    case D_EVENT_POINTER_BUTTON:
        if (event->button != D_BUTTON_LEFT) break;
        if (event->pressed) pointer_press(event);
        else fb.drag = DRAG_NONE;
        break;
    case D_EVENT_SCROLL:
        fb.dirty = true;
        if (fb.menu[0].id != MENU_NONE) {
            MenuLevel *menu = &fb.menu[fb.in_submenu ? 1 : 0];
            menu->selected = menu_step(menu->id, menu->selected < 0 ? (event->lines < 0 ? 0 : -1)
                                                                    : menu->selected, event->lines < 0 ? -1 : 1);
        } else if (fb.popup_open) {
            if (event->lines < 0 && fb.popup.selected) --fb.popup.selected;
            else if (event->lines > 0 && fb.popup.selected + 1 < fb.popup.count) ++fb.popup.selected;
        } else if (fb.viewer.active) {
            viewer_zoom(event->lines < 0 ? 1.25 : 0.8);
        } else {
            scroll_by(event->lines * 3 * line_step());
        }
        break;
    case D_EVENT_RESIZE: resized(); break;
    case D_EVENT_SUSPEND: fb.suspended = true; break;
    case D_EVENT_RESUME: fb.suspended = false; fb.dirty = true; break;
    case D_EVENT_QUIT: fb.quit = true; break;
    default: break;
    }
}

/* ---- main ----------------------------------------------------------------- */

static char *engine_path(void)
{
    const char *configured = getenv("DISTILL_ENGINE");
    if (configured && *configured && d_fs_executable(configured)) return d_strdup(configured);
    char *directory = d_proc_executable_directory();
    if (directory) {
#ifdef DISTILL_DISPLAY_WINDOWS
        char *candidate = d_cat2(directory, "/distill.exe");
#else
        char *candidate = d_cat2(directory, "/distill");
#endif
        free(directory);
        if (d_fs_executable(candidate)) return candidate;
        free(candidate);
    }
    return d_strdup("distill");
}

static const char *shell_font_dir(void)
{
    static char *bundled;
    const char *configured = getenv("DISTILL_FONT_DIR");
    if (configured && *configured) return configured;
    if (!bundled) bundled = d_data_path("fonts", NULL);
    return bundled;
}

static void usage(const char *program, const char *default_display)
{
    fprintf(stderr,
            "usage: %s [--display SPEC] [--version] [URL]\n"
            "  Distill on a framebuffer. With no URL, opens the start page from settings.\n"
#ifdef DISTILL_DISPLAY_WINDOWS
            "  --display windows[:WxH] a Windows window (default 1050x760)\n"
#else
            "  --display console        the Linux console: /dev/fb0 (or $FRAMEBUFFER), keys from\n"
            "                           this virtual console's tty, the mouse from evdev\n"
#endif
            "  --display memory:WxH[,cell=CWxCH][,script=FILE]\n"
            "                           no screen: a buffer driven by a script (the --fb gate)\n"
#ifdef DISTILL_DISPLAY_X11
            "  --display x11[:WxH]      a window (default 1920x1080)\n"
#endif
#ifdef DISTILL_DISPLAY_WINDOWS
            "  The default display is %s.\n",
#else
            "  The default is %s. If a crash leaves the console blank, switch VT and back,\n"
            "  or type blind: kbd_mode -u; chvt N.\n",
#endif
            program, default_display);
}

int main(int argc, char **argv)
{
    d_measure_begin(argv);
#if defined(DISTILL_DISPLAY_WINDOWS)
    const char *display_spec = "windows";
#elif defined(DISTILL_DISPLAY_X11)
    const char *display_spec = "x11";
#else
    const char *display_spec = "console";
#endif
    const char *environment_display = getenv("DISTILL_DISPLAY");
    if (environment_display && *environment_display) display_spec = environment_display;
    const char *url_argument = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0], display_spec); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("distill-fb " DISTILL_VERSION); return 0; }
        if (!strcmp(argv[i], "--display") && i + 1 < argc) { display_spec = argv[++i]; continue; }
        if (argv[i][0] == '-') { usage(argv[0], display_spec); return 2; }
        url_argument = argv[i];
    }
    fb.page.focus = -1;
    d_app_history_init(&fb.history);
    char *start = NULL;
    load_settings(&start);
    fb.engine = engine_path();
    fb.font_dir = shell_font_dir();
    char *directory = d_fs_private_directory(NULL, "distill-fb");
    if (!directory || strlen(directory) >= sizeof(run_directory)) {
        fprintf(stderr, "distill-fb: no private directory for this run\n");
        free(directory);
        free(start);
        return 1;
    }
    snprintf(run_directory, sizeof(run_directory), "%s", directory);
    free(directory);
    d_proc_set_environment("TMPDIR", run_directory);
    fb.session_file = d_fs_private_file(NULL, "distill-session");
    if (fb.session_file) snprintf(session_path, sizeof(session_path), "%s", fb.session_file);
    /* Where the platform cannot catch a crash, the shell still runs; it just
       cannot put the console back after one. */
    d_proc_on_fatal_signal(emergency_restore);

    char *error = NULL;
    fb.display = d_display_open(display_spec, &error);
    int status = 0;
    if (!fb.display) {
        fprintf(stderr, "distill-fb: %s\n", error ? error : "no display");
        free(error);
        status = 1;
    } else {
        fb.mode = d_display_mode(fb.display);
        ensure_screen();
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
        fb.dirty = true;
        MediaAudioPhase last_phase = MEDIA_AUDIO_STOPPED;
        while (!fb.quit) {
            MediaAudioState audio;
            media_audio_snapshot(&audio);
            bool playing = audio.phase == MEDIA_AUDIO_PLAYING;
            /* The transport moves while playing, and changes when a track
               ends by itself. */
            if (playing || audio.phase != last_phase) fb.dirty = true;
            last_phase = audio.phase;
            if (fb.dirty && !fb.suspended) render();
            report();
            int handles[HANDLES_MAX];
            size_t count = d_display_handles(fb.display, handles, 8);
            size_t nav_first = count;
            count += d_nav_handles(&fb.nav, handles + count, 2);
            size_t nav_last = count;
            int transfer = -1;
            if (fb.transfer) { transfer = (int)count; handles[count++] = d_media_transfer_fd(fb.transfer); }
            DPollItem items[HANDLES_MAX];
            for (size_t i = 0; i < count; ++i) items[i] = (DPollItem){handles[i], false};
            int timeout = d_display_wait_ms(fb.display);
            if (playing && (timeout < 0 || timeout > 250)) timeout = 250;
            if (fb.external_count && (timeout < 0 || timeout > 1000)) timeout = 1000;
            if (d_poll(items, count, timeout) < 0) break;
            bool nav_ready = false;
            for (size_t i = nav_first; i < nav_last; ++i) nav_ready |= items[i].ready;
            if (nav_ready) nav_service();
            if (transfer >= 0 && items[transfer].ready) transfer_service();
            DEvent event;
            while (!fb.quit && d_display_next_event(fb.display, &event)) handle(&event);
            reap_externals();
        }
        if (fb.nav.active) d_nav_cancel(&fb.nav);
        if (fb.transfer) transfer_cancel();
        audio_release();
        viewer_free_image();
        stop_editing();
        prompt_close();
        popup_close(false);
        d_display_close(fb.display);
    }
    free(start);
    page_free(&fb.page);
    if (fb.viewer.layer) d_render_destroy(fb.viewer.layer);
    for (size_t i = 0; i < fb.viewer.count; ++i) free(fb.viewer.urls[i]);
    free(fb.viewer.urls);
    font_free(&fb.ui); font_free(&fb.bold); font_free(&fb.symbol);
    font_free(&fb.control); font_free(&fb.control_bold);
    if (fb.screen) d_render_destroy(fb.screen);
    d_app_history_clear(&fb.history, dispose_history_page);
    if (fb.session_file) { d_fs_remove(fb.session_file); free(fb.session_file); }
    session_path[0] = '\0';
    if (!d_fs_remove_directory(run_directory))
        fprintf(stderr, "distill-fb: %s was not empty on exit\n", run_directory);
    free(fb.engine);
    free(fb.theme);
    free(fb.status);
    d_measure_finish("fb", NULL);
    return status;
}
