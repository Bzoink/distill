#define _POSIX_C_SOURCE 200809L

#include "app_state.h"
#include "image.h"

/* For the self-test's PNG writer only; zlib arrives transitively through the
   toolkit otherwise, which is not something to depend on. */
#include <zlib.h>
#include "cutil.h"
#include "render.h"

#include <cairo.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DISTILL_ENGINE_PATH
#define DISTILL_ENGINE_PATH "distill"
#endif

#ifndef DISTILL_FONT_DIR
#define DISTILL_FONT_DIR ""
#endif

enum {
    /* The page follows the window between these two, in design pixels that
       zoom then scales. The upper end is the width the reader was drawn at:
       the measure is capped for readability, so a wider window buys margin
       and nothing else. The lower end is where a measure stops being one --
       a narrower window scrolls the page rather than squeezing it further. */
    PAGE_WIDTH_DESIGN = 760,
    PAGE_WIDTH_FLOOR = 480,
    PAGE_HEIGHT = 1400,
    /* A resize drag delivers a width per frame and laying the page out again
       is the expensive half of a navigation, so let the drag settle first. */
    RELAYOUT_SETTLE_MS = 120,
    /* Often enough that the count moves smoothly, rarely enough that a status
       line is not redrawn for its own sake. */
    PROGRESS_POLL_MS = 100,
    RENDER_OVERSCAN = 200,
    MAX_FORM_BOXES = 512,
    MAX_LINK_BOXES = 2048,
    MAX_TEXT_BOXES = 16384,
    /* A press that moves less than this is a click, not a drag. Without it a
       link could not be activated without selecting a character of it. */
    DRAG_THRESHOLD_PX = 3,
    /* Long enough to keep a host and a file name, short enough to fit the
       column: litehtml breaks neither on overflow-wrap nor on word-break, so
       a URL that does not fit simply runs off the edge of the page. */
    DISPLAY_URL_BUDGET = 58
};

/* The zoom levels the menu offers. Steps a reader would choose, not a
   continuous scale: each one is a visible change from its neighbour, and the
   ends are where the fixed-width column stops being worth reading. */
static const int zoom_levels[] = {75, 90, 100, 110, 125, 150, 175, 200};
enum { ZOOM_LEVEL_COUNT = (int)(sizeof(zoom_levels) / sizeof(zoom_levels[0])) };
#define ZOOM_DEFAULT 100

static int zoom_level_index(int zoom)
{
    for (int i = 0; i < ZOOM_LEVEL_COUNT; ++i) {
        if (zoom_levels[i] == zoom) return i;
    }
    return -1;
}

static bool valid_zoom(int zoom)
{
    return zoom_level_index(zoom) >= 0;
}

/* Zoom moves both ends of the span with the design it scales: at 175% the
   page wants 175% of the room before more of it stops helping. */
static int clamp_page_width(int available, int zoom)
{
    int maximum = (PAGE_WIDTH_DESIGN * zoom + 50) / 100;
    int minimum = (PAGE_WIDTH_FLOOR * zoom + 50) / 100;
    if (minimum > maximum) minimum = maximum;
    if (available < minimum) return minimum;
    if (available > maximum) return maximum;
    return available;
}

static int zoom_step_level(int zoom, int direction)
{
    int index = zoom_level_index(zoom);
    if (index < 0) index = zoom_level_index(ZOOM_DEFAULT);
    int next = index + direction;
    if (next < 0) next = 0;
    if (next >= ZOOM_LEVEL_COUNT) next = ZOOM_LEVEL_COUNT - 1;
    return zoom_levels[next];
}

enum {
    PAGE_SECTION_TOP,
    PAGE_SECTION_IMAGES,
    PAGE_SECTION_TABLES,
    PAGE_SECTION_FORMS,
    PAGE_SECTION_VIDEOS,
    PAGE_SECTION_RESULTS,
    PAGE_SECTION_LINKS,
    PAGE_SECTION_COUNT
};

typedef struct {
    const char *id;
    const char *label;
    const char *shortcut;
    guint keyval;
} PageSection;

static const PageSection page_sections[PAGE_SECTION_COUNT] = {
    [PAGE_SECTION_TOP] = {"top", "Top", "Alt+0", GDK_KEY_0},
    [PAGE_SECTION_IMAGES] = {"images", "Images", "Alt+1", GDK_KEY_1},
    [PAGE_SECTION_TABLES] = {"tables", "Tables", "Alt+2", GDK_KEY_2},
    [PAGE_SECTION_FORMS] = {"forms", "Forms", "Alt+3", GDK_KEY_3},
    [PAGE_SECTION_VIDEOS] = {"videos", "Videos", "Alt+4", GDK_KEY_4},
    [PAGE_SECTION_RESULTS] = {"results", "Results", "Alt+5", GDK_KEY_5},
    [PAGE_SECTION_LINKS] = {"links", "Links", "Alt+6", GDK_KEY_6}
};

/* Internal destinations. They are not URLs to fetch, they are things for
   the shell to do, so they never reach the navigation path. */
#define MEDIA_SCHEME "distill-media:"
#define CLOSE_URI "distill-close:"
#define SECTION_SCHEME "distill-section:"

typedef struct App App;

typedef struct {
    DFormBox box;
    GtkWidget *widget;
} FormControl;

/* A place in the page's text: which run, and how far into it. Runs arrive in
   the order they were drawn, which for a reader page in normal flow is reading
   order, so comparing two places is comparing this pair. */
typedef struct {
    size_t run;
    size_t offset;
} TextPoint;

/* What the engine has said so far about the run in flight. The load happens
   on a worker thread and the status line belongs to the main one, so the two
   meet here instead of reaching into each other: the worker writes what the
   engine reports, a timer on the main thread reads it, and the box outlives
   whichever of them finishes first. */
typedef struct {
    GMutex lock;
    int percent;
    char phase[16];
    gint references;
} LoadProgress;

typedef struct {
    char *url;
    char *method;
    char *data;
    char *theme;
    int zoom;
    int width;
    LoadProgress *progress;
    guint serial;
    bool replace_current;
} LoadRequest;

typedef struct {
    char *url;
    char *template_path;
    char *theme;
    int zoom;
    char *error;
    bool ok;
    guint serial;
    bool replace_current;
} LoadResult;

struct App {
    GApplication *application;
    GtkWidget *window;
    GtkWidget *url_entry;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *section_button;
    GtkWidget *section_items[PAGE_SECTION_COUNT];
    GtkWidget *settings_button;
    GtkWidget *theme_items[3];
    GtkWidget *zoom_items[ZOOM_LEVEL_COUNT];
    GtkCssProvider *form_css_provider;
    GtkWidget *status;
    GtkWidget *spinner;
    LoadProgress *progress;
    guint progress_source;
    GtkWidget *drawing_area;
    GtkWidget *event_box;
    GtkWidget *form_layer;
    GtkAdjustment *vertical_adjustment;
    GtkAdjustment *horizontal_adjustment;
    int page_width;
    int page_height;
    guint relayout_source;
    cairo_surface_t *surface;
    DLiteHtmlDocument *document;
    int surface_top;
    DFormBox *form_boxes;
    size_t form_count;
    FormControl *controls;
    size_t control_count;
    DLinkBox *link_boxes;
    size_t link_count;
    DTextBox *text_boxes;
    size_t text_count;
    bool text_collected;
    TextPoint selection_anchor;
    TextPoint selection_focus;
    bool has_selection;
    bool selecting;
    bool dragged;
    double press_x;
    double press_y;
    DAppHistory history;
    /* The images this page offers, in document order, captured while the page
       document exists -- viewer mode replaces that document, so the list has
       to outlive it for Previous and Next to mean anything. */
    GPtrArray *page_media;
    bool viewer_active;
    bool entering_viewer;
    int viewer_index;
    char *viewer_file;
    /* What the open image is, kept so the viewer can be laid out again when
       the window changes width. */
    char *viewer_url;
    char *viewer_failure;
    double reader_scroll_y;
    guint restore_scroll_source;
    guint load_serial;
    unsigned int references;
    bool closing;
    bool loading;
    bool updating_theme;
    bool updating_zoom;
};

static void app_ref(App *app)
{
    if (app) ++app->references;
}

static void app_unref(App *app)
{
    if (!app || app->references == 0) return;
    if (--app->references == 0) g_free(app);
}

static void page_dispose(DAppPage *page)
{
    if (!page) return;
    if (page->template_path && *page->template_path) unlink(page->template_path);
}

static void set_status(App *app, const char *message)
{
    gtk_label_set_text(GTK_LABEL(app->status), message ? message : "");
}

static LoadProgress *load_progress_new(void)
{
    LoadProgress *progress = g_new0(LoadProgress, 1);
    g_mutex_init(&progress->lock);
    progress->references = 1;
    return progress;
}

static LoadProgress *load_progress_ref(LoadProgress *progress)
{
    if (progress) g_atomic_int_inc(&progress->references);
    return progress;
}

static void load_progress_unref(LoadProgress *progress)
{
    if (!progress || !g_atomic_int_dec_and_test(&progress->references)) return;
    g_mutex_clear(&progress->lock);
    g_free(progress);
}

static void load_progress_set(LoadProgress *progress, int percent, const char *phase)
{
    if (!progress) return;
    g_mutex_lock(&progress->lock);
    progress->percent = percent;
    g_strlcpy(progress->phase, phase ? phase : "", sizeof progress->phase);
    g_mutex_unlock(&progress->lock);
}

static void load_progress_get(LoadProgress *progress, int *percent, char *phase, size_t size)
{
    *percent = 0;
    if (size) *phase = '\0';
    if (!progress) return;
    g_mutex_lock(&progress->lock);
    *percent = progress->percent;
    g_strlcpy(phase, progress->phase, size);
    g_mutex_unlock(&progress->lock);
}

/* What the reader is told while it waits. The phase is the part the engine
   actually measures, so it leads; the percentage follows it. */
static char *loading_message(int percent, const char *phase)
{
    const char *doing = "Loading";
    if (d_eq(phase, "fetch")) doing = "Fetching the page";
    else if (d_eq(phase, "scripts")) doing = "Running the page's scripts";
    else if (d_eq(phase, "extract")) doing = "Reading the page";
    else if (d_eq(phase, "render")) doing = "Laying out the page";
    else if (d_eq(phase, "done")) doing = "Finishing";
    if (percent <= 0) return g_strdup_printf("%s\xe2\x80\xa6", doing);
    return g_strdup_printf("%s\xe2\x80\xa6 %d%%", doing, percent);
}

static gboolean poll_load_progress(gpointer opaque)
{
    App *app = opaque;
    if (app->closing || !app->loading || !app->progress) {
        app->progress_source = 0;
        return G_SOURCE_REMOVE;
    }
    int percent = 0;
    char phase[16];
    load_progress_get(app->progress, &percent, phase, sizeof phase);
    char *message = loading_message(percent, phase);
    set_status(app, message);
    g_free(message);
    return G_SOURCE_CONTINUE;
}

/* The spinner says the run is alive even while the percentage is standing
   still, which it does whenever a phase cannot measure itself -- a server
   that declares no length, a page whose scripts are simply thinking. */
static void start_load_feedback(App *app)
{
    if (app->spinner) {
        gtk_widget_show(app->spinner);
        gtk_spinner_start(GTK_SPINNER(app->spinner));
    }
    if (!app->progress_source)
        app->progress_source = g_timeout_add(PROGRESS_POLL_MS, poll_load_progress, app);
}

static void stop_load_feedback(App *app)
{
    if (app->progress_source) {
        g_source_remove(app->progress_source);
        app->progress_source = 0;
    }
    if (app->spinner) {
        gtk_spinner_stop(GTK_SPINNER(app->spinner));
        gtk_widget_hide(app->spinner);
    }
    load_progress_unref(app->progress);
    app->progress = NULL;
}

static const char *active_theme_id(const App *app)
{
    static const char *fallback = "warm";
    if (!app) return fallback;
    for (size_t i = 0; i < sizeof(app->theme_items) / sizeof(app->theme_items[0]); ++i) {
        if (app->theme_items[i] &&
            gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->theme_items[i]))) {
            const char *theme = g_object_get_data(G_OBJECT(app->theme_items[i]),
                                                  "distill-theme-id");
            if (theme && *theme) return theme;
        }
    }
    return fallback;
}

static void select_theme_id(App *app, const char *theme)
{
    if (!app) return;
    app->updating_theme = true;
    for (size_t i = 0; i < sizeof(app->theme_items) / sizeof(app->theme_items[0]); ++i) {
        const char *item_theme = app->theme_items[i]
                                     ? g_object_get_data(G_OBJECT(app->theme_items[i]),
                                                         "distill-theme-id")
                                     : NULL;
        if (item_theme && strcmp(item_theme, theme ? theme : "warm") == 0)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->theme_items[i]), TRUE);
    }
    app->updating_theme = false;
}

static int active_zoom(const App *app)
{
    if (!app) return ZOOM_DEFAULT;
    for (int i = 0; i < ZOOM_LEVEL_COUNT; ++i) {
        if (app->zoom_items[i] &&
            gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->zoom_items[i])))
            return zoom_levels[i];
    }
    return ZOOM_DEFAULT;
}

static void select_zoom(App *app, int zoom)
{
    if (!app) return;
    int index = zoom_level_index(zoom);
    if (index < 0) index = zoom_level_index(ZOOM_DEFAULT);
    if (index < 0 || !app->zoom_items[index]) return;
    app->updating_zoom = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->zoom_items[index]), TRUE);
    app->updating_zoom = false;
}

/* The width to draw at now: what the window offers, held inside the span. */
static int reader_page_width(const App *app)
{
    if (!app) return PAGE_WIDTH_DESIGN;
    int available = PAGE_WIDTH_DESIGN;
    if (app->horizontal_adjustment) {
        double size = gtk_adjustment_get_page_size(app->horizontal_adjustment);
        if (size > 1.0) available = (int)size;
    }
    return clamp_page_width(available, active_zoom(app));
}

static void set_surface_size(App *app, int width, int height)
{
    if (!app) return;
    if (app->drawing_area) gtk_widget_set_size_request(app->drawing_area, width, height);
    if (app->event_box) gtk_widget_set_size_request(app->event_box, width, height);
    if (app->form_layer) gtk_widget_set_size_request(app->form_layer, width, height);
    app->page_width = width;
    app->page_height = height;
}

static bool valid_theme_id(const char *theme)
{
    return theme && (strcmp(theme, "warm") == 0 ||
                     strcmp(theme, "night") == 0 ||
                     strcmp(theme, "paper") == 0);
}

/* Native controls are the one part of the reader GTK draws, so the zoom the
   template was laid out at has to be told to them separately or a zoomed page
   shows large type inside default-sized fields. */
static void update_form_style(App *app, const char *theme, int zoom)
{
    if (!app || !app->window) return;
    if (!app->form_css_provider) {
        app->form_css_provider = gtk_css_provider_new();
        GdkScreen *screen = gtk_widget_get_screen(app->window);
        if (screen)
            gtk_style_context_add_provider_for_screen(
                screen, GTK_STYLE_PROVIDER(app->form_css_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    const char *entry_text = "#282622";
    const char *entry_background = "#ffffff";
    const char *border = "#cfc7bb";
    const char *focus_border = "#165d7a";
    const char *button_text = "#ffffff";
    const char *button_background = "#165d7a";
    const char *button_border = "#0f4a63";
    const char *button_hover = "#1d708f";
    if (theme && strcmp(theme, "night") == 0) {
        entry_text = "#e7e2d8";
        entry_background = "#202326";
        border = "#454b50";
        focus_border = "#78b9cf";
        button_text = "#17191b";
        button_background = "#78b9cf";
        button_border = "#9ed1df";
        button_hover = "#9ed1df";
    } else if (theme && strcmp(theme, "paper") == 0) {
        entry_text = "#332d24";
        entry_background = "#ffffff";
        border = "#d1c1a8";
        focus_border = "#79533b";
        button_background = "#79533b";
        button_border = "#5f402d";
        button_hover = "#936b4f";
    }

    if (!valid_zoom(zoom)) zoom = ZOOM_DEFAULT;

    /* A percentage rather than a pixel size: the control's text is scaled
       from whatever the desktop's font is, so zoom moves it without
       overriding a font size the user chose outside this window. */
    char *css = g_strdup_printf(
        /* The page no longer fills the window, so the space either side of it
           is the scrolled window's own background rather than something the
           reader surface painted. It is the desk the page lies on and stays
           the same dark whatever the reader theme, as it was when the surface
           drew it. */
        ".distill-desk,.distill-desk viewport{background-color:#1a1c1f;}"
        ".distill-form-entry,.distill-form-textarea,.distill-form-select,"
        ".distill-form-choice,.distill-form-button{font-size:%d%%;}"
        ".distill-form-entry{color:%s;background-color:%s;background-image:none;"
        "min-height:0;border:1px solid %s;border-radius:3px;padding:2px 6px;}"
        ".distill-form-entry:focus{border-color:%s;}"
        ".distill-form-textarea,.distill-form-select{color:%s;background-color:%s;"
        "background-image:none;border:1px solid %s;border-radius:3px;}"
        ".distill-form-textarea text{color:%s;background-color:%s;}"
        /* Checkbox and radio boxes are placed using litehtml's exact inline
           rectangle. Remove GTK's theme padding so their indicator starts at
           that rectangle rather than being inset from the label's left edge. */
        ".distill-form-choice{color:%s;padding:0;margin:0;min-width:0;min-height:0;}"
        ".distill-form-choice check{margin:0;padding:0;min-width:0;min-height:0;}"
        ".distill-form-button{color:%s;background-color:%s;background-image:none;"
        "border:1px solid %s;border-radius:3px;padding:6px 12px;}"
        ".distill-form-button:hover{background-color:%s;}",
        zoom,
        entry_text, entry_background, border, focus_border,
        entry_text, entry_background, border, entry_text, entry_background, entry_text,
        button_text, button_background, button_border, button_hover);
    GError *error = NULL;
    gtk_css_provider_load_from_data(app->form_css_provider, css, -1, &error);
    if (error) {
        g_warning("forms: unable to apply theme: %s", error->message);
        g_error_free(error);
    }
    g_free(css);
}

static char *browser_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "distill", "settings.ini", NULL);
}

/* One file holds every browser preference, so writing one of them loads what
   is there, changes the one key and writes the file back whole. Building a
   fresh key file per setting would silently drop the others. */
static GKeyFile *load_browser_settings(void)
{
    char *path = browser_settings_path();
    GKeyFile *settings = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, &error) && error)
        g_error_free(error);
    g_free(path);
    return settings;
}

static void save_browser_settings(GKeyFile *settings)
{
    char *directory = g_build_filename(g_get_user_config_dir(), "distill", NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_warning("settings: unable to create %s: %s", directory, g_strerror(errno));
        g_free(directory);
        return;
    }
    char *path = g_build_filename(directory, "settings.ini", NULL);
    GError *error = NULL;
    gsize length = 0;
    gchar *data = g_key_file_to_data(settings, &length, &error);
    if (!data || !g_file_set_contents(path, data, (gssize)length, &error))
        g_warning("settings: unable to save %s: %s", path,
                  error ? error->message : "unknown error");
    if (error) g_error_free(error);
    g_free(data);
    g_free(path);
    g_free(directory);
}

static char *load_saved_theme(void)
{
    GKeyFile *settings = load_browser_settings();
    char *theme = g_key_file_get_string(settings, "browser", "theme", NULL);
    if (!valid_theme_id(theme)) {
        g_free(theme);
        theme = g_strdup("warm");
    }
    g_key_file_free(settings);
    return theme;
}

static int load_saved_zoom(void)
{
    GKeyFile *settings = load_browser_settings();
    GError *error = NULL;
    int zoom = (int)g_key_file_get_integer(settings, "browser", "zoom", &error);
    if (error) {
        g_error_free(error);
        zoom = ZOOM_DEFAULT;
    }
    if (!valid_zoom(zoom)) zoom = ZOOM_DEFAULT;
    g_key_file_free(settings);
    return zoom;
}

static void save_theme(const char *theme)
{
    if (!valid_theme_id(theme)) return;
    GKeyFile *settings = load_browser_settings();
    g_key_file_set_string(settings, "browser", "theme", theme);
    save_browser_settings(settings);
    g_key_file_free(settings);
}

static void save_zoom(int zoom)
{
    if (!valid_zoom(zoom)) return;
    GKeyFile *settings = load_browser_settings();
    g_key_file_set_integer(settings, "browser", "zoom", zoom);
    save_browser_settings(settings);
    g_key_file_free(settings);
}

/* Rebuilding the litehtml document resets the adjustment while GTK lays out
   the new child. Restore the reader position from an idle callback, after the
   child has its real height, so the adjustment can accept the old offset. */
static gboolean restore_reader_scroll(gpointer opaque)
{
    App *app = opaque;
    app->restore_scroll_source = 0;
    if (!app->closing && !app->viewer_active && app->vertical_adjustment)
        gtk_adjustment_set_value(app->vertical_adjustment, app->reader_scroll_y);
    return G_SOURCE_REMOVE;
}

static void cancel_reader_scroll_restore(App *app)
{
    if (!app->restore_scroll_source) return;
    g_source_remove(app->restore_scroll_source);
    app->restore_scroll_source = 0;
}

static void schedule_reader_scroll_restore(App *app)
{
    cancel_reader_scroll_restore(app);
    app->restore_scroll_source = g_idle_add(restore_reader_scroll, app);
}

static bool has_scheme(const char *uri, const char *scheme)
{
    size_t length = strlen(scheme);
    return strncasecmp(uri ? uri : "", scheme, length) == 0 && uri[length] == ':';
}

static bool page_uri_allowed(const char *uri)
{
    if (!uri || !*uri) return false;
    if (has_scheme(uri, "http") || has_scheme(uri, "https") || has_scheme(uri, "file")) return true;
    return !strchr(uri, ':');
}

/* The search engine the reader is pointed at. DuckDuckGo's HTML endpoint is
   chosen on measured behaviour rather than preference: it is server-rendered,
   needs no JavaScript, and wraps each result in a "result" class, which is the
   signal the extractor's search-results shape keys on. Measured against the
   same query, it produced 27 structured result records where Mojeek, Brave,
   Ecosia, Startpage and SearX produced none, and Marginalia rate-limited the
   request outright. Override with DISTILL_SEARCH_URL; the encoded query is
   appended to whatever it is set to. */
#define DISTILL_DEFAULT_SEARCH_URL "https://html.duckduckgo.com/html/?q="

static bool external_uri_allowed(const char *uri)
{
    return has_scheme(uri, "http") || has_scheme(uri, "https") ||
           has_scheme(uri, "mailto") || has_scheme(uri, "tel");
}

/* DuckDuckGo hands out result links as /l/?uddg=<target>, and that redirect
   does not resolve here -- it answers 400 without its token and yields nothing
   with it. The destination is in the link already, so read it rather than
   following the wrapper, otherwise every search result is a dead end. */
static char *unwrap_redirect(const char *uri)
{
    if (!uri) return NULL;
    const char *host = strstr(uri, "duckduckgo.com/l/");
    if (!host) return NULL;
    const char *query = strchr(host, '?');
    if (!query) return NULL;
    for (const char *p = query + 1; *p; ) {
        const char *end = strchr(p, '&');
        size_t span = end ? (size_t)(end - p) : strlen(p);
        if (span > 5 && strncmp(p, "uddg=", 5) == 0) {
            char *encoded = g_strndup(p + 5, span - 5);
            char *target = g_uri_unescape_string(encoded, NULL);
            g_free(encoded);
            if (target && (has_scheme(target, "http") || has_scheme(target, "https"))) return target;
            g_free(target);
            return NULL;
        }
        if (!end) break;
        p = end + 1;
    }
    return NULL;
}

static char *engine_path(void)
{
    const char *configured = getenv("DISTILL_ENGINE");
    if (configured && *configured && access(configured, X_OK) == 0) return g_strdup(configured);
    if (access(DISTILL_ENGINE_PATH, X_OK) == 0) return g_strdup(DISTILL_ENGINE_PATH);

    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = '\0';
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            char *candidate = g_build_filename(executable, "distill", NULL);
            if (access(candidate, X_OK) == 0) return candidate;
            g_free(candidate);
        }
    }
    return g_strdup("distill");
}

static char *temporary_path(const char *prefix)
{
    GError *error = NULL;
    char *path = NULL;
    int descriptor = g_file_open_tmp(prefix, &path, &error);
    if (descriptor >= 0) {
        close(descriptor);
        unlink(path);
        return path;
    }
    if (error) {
        g_warning("temporary file: %s", error->message);
        g_error_free(error);
    }
    return NULL;
}

static void free_load_request(LoadRequest *request)
{
    if (!request) return;
    g_free(request->url);
    g_free(request->method);
    g_free(request->data);
    g_free(request->theme);
    load_progress_unref(request->progress);
    g_free(request);
}

static void free_load_result(LoadResult *result)
{
    if (!result) return;
    if (result->template_path) unlink(result->template_path);
    g_free(result->url);
    g_free(result->template_path);
    g_free(result->theme);
    g_free(result->error);
    g_free(result);
}

static void add_arg(GPtrArray *arguments, const char *value)
{
    g_ptr_array_add(arguments, g_strdup(value ? value : ""));
}

/* One session for the life of the window. Each navigation is a separate
   engine process, so without a shared cookie file a user who signs in is
   signed out again by their next click. */
static char *session_file = NULL;

static const char *session_file_path(void)
{
    if (session_file) return session_file;
    GError *error = NULL;
    char *path = NULL;
    int descriptor = g_file_open_tmp("distill-session-XXXXXX", &path, &error);
    if (descriptor < 0) {
        if (error) {
            g_warning("session file: %s", error->message);
            g_error_free(error);
        }
        return NULL;
    }
    /* g_file_open_tmp already creates the file 0600; make that explicit
       because the file holds live session cookies. */
    if (fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        close(descriptor);
        unlink(path);
        g_free(path);
        return NULL;
    }
    close(descriptor);
    session_file = path;
    return session_file;
}

static void session_file_discard(void)
{
    if (!session_file) return;
    unlink(session_file);
    g_free(session_file);
    session_file = NULL;
}

/* A submitted form may carry a password. Writing it to a private file keeps
   it out of the argument vector, which is readable from the process table and
   is the one place a credential must never appear. */
static char *write_private_body(const char *data)
{
    GError *error = NULL;
    char *path = NULL;
    int descriptor = g_file_open_tmp("distill-body-XXXXXX", &path, &error);
    if (descriptor < 0) {
        if (error) {
            g_warning("request body file: %s", error->message);
            g_error_free(error);
        }
        return NULL;
    }
    size_t length = strlen(data);
    bool ok = fchmod(descriptor, S_IRUSR | S_IWUSR) == 0;
    while (ok && length) {
        ssize_t written = write(descriptor, data, length);
        if (written <= 0) { ok = false; break; }
        data += written;
        length -= (size_t)written;
    }
    close(descriptor);
    if (!ok) {
        unlink(path);
        g_free(path);
        return NULL;
    }
    return path;
}

/* One "progress PERCENT PHASE" record per line. Anything else the engine puts
   on this pipe is ignored rather than guessed at. */
static void consume_progress(GString *pending, const char *bytes, size_t length,
                             LoadProgress *progress)
{
    g_string_append_len(pending, bytes, (gssize)length);
    char *start = pending->str;
    char *newline;
    while ((newline = strchr(start, '\n'))) {
        *newline = '\0';
        int percent = 0;
        char phase[16] = "";
        if (sscanf(start, "progress %d %15s", &percent, phase) == 2)
            load_progress_set(progress, percent, phase);
        start = newline + 1;
    }
    g_string_erase(pending, 0, (gssize)(start - pending->str));
}

/* Read both pipes until the engine closes them. Both, because a run with a
   lot to say on stderr would otherwise fill that pipe and stop with nobody
   draining it; as it happens rather than at the end, because a progress
   record that arrives after the page does is no use to anyone. */
static void read_engine_output(int out_fd, int err_fd, LoadProgress *progress,
                               GString *diagnostics)
{
    GString *pending = g_string_new(NULL);
    struct pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
    int open_pipes = 2;
    while (open_pipes > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !fds[i].revents) continue;
            char buffer[4096];
            ssize_t got = read(fds[i].fd, buffer, sizeof buffer);
            if (got > 0) {
                if (i == 0) consume_progress(pending, buffer, (size_t)got, progress);
                else g_string_append_len(diagnostics, buffer, (gssize)got);
                continue;
            }
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            close(fds[i].fd);
            fds[i].fd = -1;
            --open_pipes;
        }
    }
    g_string_free(pending, TRUE);
}

static bool run_engine(const LoadRequest *request, const char *template_path,
                       char **error_message)
{
    char *engine = engine_path();
    if (!engine) return false;
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    add_arg(arguments, engine);
    add_arg(arguments, "--url");
    add_arg(arguments, request->url);
    add_arg(arguments, "--template");
    add_arg(arguments, template_path);
    add_arg(arguments, "--width");
    char *width = g_strdup_printf("%d", request->width);
    add_arg(arguments, width);
    g_free(width);
    /* 0 means "as tall as the content". A reader page is as long as what it
       has to say, and a fixed surface silently drops the rest: the page looks
       finished and simply stops. */
    add_arg(arguments, "--height");
    add_arg(arguments, "0");
    add_arg(arguments, "--theme");
    add_arg(arguments, request->theme);
    add_arg(arguments, "--zoom");
    char *zoom = g_strdup_printf("%d", request->zoom);
    add_arg(arguments, zoom);
    g_free(zoom);
    add_arg(arguments, "--native-forms");
    add_arg(arguments, "--progress");
    if (request->method && *request->method) {
        add_arg(arguments, "--request-method");
        add_arg(arguments, request->method);
    }
    char *body_path = NULL;
    if (request->data && *request->data) {
        body_path = write_private_body(request->data);
        if (!body_path) {
            g_ptr_array_free(arguments, TRUE);
            g_free(engine);
            if (error_message) *error_message = g_strdup("unable to stage the request body");
            return false;
        }
        add_arg(arguments, "--request-data-file");
        add_arg(arguments, body_path);
    }
    const char *session = session_file_path();
    if (session) {
        add_arg(arguments, "--session-file");
        add_arg(arguments, session);
    }
    add_arg(arguments, "--script-cap-ms");
    add_arg(arguments, "5000");
    g_ptr_array_add(arguments, NULL);

    GError *error = NULL;
    GPid child = 0;
    gint out_fd = -1, err_fd = -1;
    /* Spawned with pipes rather than run to completion: the run reports where
       it has got to while it is still running, and this is the end that
       listens. The child is not reaped by GLib so its status can be collected
       here, on the thread that is already waiting for it. */
    gboolean spawned = g_spawn_async_with_pipes(NULL, (char **)arguments->pdata, NULL,
                                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                                NULL, NULL, &child, NULL, &out_fd, &err_fd,
                                                &error);
    GString *diagnostics = g_string_new(NULL);
    int wait_status = 0;
    bool waited = false;
    if (spawned) {
        read_engine_output(out_fd, err_fd, request->progress, diagnostics);
        pid_t reaped;
        while ((reaped = waitpid(child, &wait_status, 0)) < 0 && errno == EINTR) {}
        /* A status only means something if it was actually collected: a child
           lost to someone else's wait must not read as a clean exit. */
        waited = reaped == (pid_t)child;
        g_spawn_close_pid(child);
    }
    if (body_path) {
        unlink(body_path);
        g_free(body_path);
    }
    bool ok = spawned && waited && g_spawn_check_wait_status(wait_status, &error);
    if (!ok && error_message) {
        const char *message = error ? error->message
                                    : (diagnostics->len ? diagnostics->str : "engine failed");
        *error_message = g_strdup(message && *message ? message : "engine failed");
    }
    if (error) g_error_free(error);
    g_string_free(diagnostics, TRUE);
    g_ptr_array_free(arguments, TRUE);
    g_free(engine);
    return ok;
}

static void load_worker(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    LoadRequest *request = task_data;
    LoadResult *result = g_new0(LoadResult, 1);
    result->url = g_strdup(request->url);
    result->theme = g_strdup(request->theme);
    result->zoom = request->zoom;
    result->serial = request->serial;
    result->replace_current = request->replace_current;
    result->template_path = temporary_path("distill-template-XXXXXX");
    if (!result->template_path) {
        result->error = g_strdup("unable to create temporary page files");
    } else {
        result->ok = run_engine(request, result->template_path,
                                &result->error);
    }
    g_task_return_pointer(task, result, (GDestroyNotify)free_load_result);
}

static void clear_controls(App *app)
{
    if (app->form_layer) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(app->form_layer));
        for (GList *item = children; item; item = item->next)
            gtk_widget_destroy(GTK_WIDGET(item->data));
        g_list_free(children);
    }
    g_free(app->form_boxes);
    app->form_boxes = NULL;
    app->form_count = 0;
    g_free(app->controls);
    app->controls = NULL;
    app->control_count = 0;
    g_free(app->link_boxes);
    app->link_boxes = NULL;
    app->link_count = 0;
}

static int section_y(const App *app, size_t index)
{
    if (!app || index >= PAGE_SECTION_COUNT || !app->document) return -1;
    if (index == PAGE_SECTION_TOP) return 0;
    const char *id = page_sections[index].id;
    size_t prefix_length = strlen(SECTION_SCHEME);
    for (size_t i = 0; i < app->link_count; ++i) {
        const char *href = app->link_boxes[i].href;
        if (strncmp(href, SECTION_SCHEME, prefix_length) == 0 &&
            strcmp(href + prefix_length, id) == 0)
            return app->link_boxes[i].y;
    }
    return -1;
}

static void update_section_menu(App *app)
{
    if (!app) return;
    for (size_t i = 0; i < PAGE_SECTION_COUNT; ++i) {
        if (app->section_items[i]) {
            bool available = !app->viewer_active && section_y(app, i) >= 0;
            gtk_widget_set_sensitive(app->section_items[i], available);
        }
    }
    if (app->section_button)
        gtk_widget_set_tooltip_text(app->section_button, "Page sections · Alt+0–6");
}

static void jump_to_section(App *app, size_t index)
{
    if (!app || index >= PAGE_SECTION_COUNT || app->viewer_active || app->loading) return;
    int y = section_y(app, index);
    if (y < 0) {
        set_status(app, "That section is not available on this page");
        return;
    }
    if (app->vertical_adjustment)
        gtk_adjustment_set_value(app->vertical_adjustment, y);
    set_status(app, page_sections[index].label);
}

static void section_item_activate(GtkMenuItem *item, gpointer opaque)
{
    App *app = opaque;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "distill-section-index"));
    if (index >= 0 && index < PAGE_SECTION_COUNT)
        jump_to_section(app, (size_t)index);
}

static int section_index_for_key(guint keyval)
{
    for (size_t i = 0; i < PAGE_SECTION_COUNT; ++i)
        if (page_sections[i].keyval == keyval) return (int)i;
    return -1;
}

static int section_index_for_id(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < PAGE_SECTION_COUNT; ++i)
        if (strcmp(page_sections[i].id, id) == 0) return (int)i;
    return -1;
}

/* Collected when the reader first reaches for text, not when the page is
   built: scrolling through a page nobody selects should not pay for a pass
   over every line of it. */
static void ensure_text_boxes(App *app)
{
    if (app->text_collected || !app->document) return;
    app->text_collected = true;
    size_t count = distill_litehtml_document_text_boxes(app->document, NULL, 0);
    if (count > MAX_TEXT_BOXES) count = MAX_TEXT_BOXES;
    if (!count) return;
    app->text_boxes = g_new0(DTextBox, count);
    app->text_count = count;
    distill_litehtml_document_text_boxes(app->document, app->text_boxes, count);
}

static void clear_selection(App *app)
{
    if (!app->has_selection) return;
    app->has_selection = false;
    if (app->drawing_area) gtk_widget_queue_draw(app->drawing_area);
}

static int text_point_compare(TextPoint a, TextPoint b)
{
    if (a.run != b.run) return a.run < b.run ? -1 : 1;
    if (a.offset != b.offset) return a.offset < b.offset ? -1 : 1;
    return 0;
}

static void selection_range(const App *app, TextPoint *start, TextPoint *end)
{
    if (text_point_compare(app->selection_anchor, app->selection_focus) <= 0) {
        *start = app->selection_anchor;
        *end = app->selection_focus;
    } else {
        *start = app->selection_focus;
        *end = app->selection_anchor;
    }
}

/* The nearest run to a point, not the one under it: a drag that wanders into
   the margin or past the last line should keep selecting rather than stop
   dead. Vertical distance dominates, because which line the pointer is on
   matters more than how far along it the pointer has got. */
static size_t nearest_text_run(const App *app, double x, double y)
{
    size_t best = 0;
    long best_distance = LONG_MAX;
    for (size_t i = 0; i < app->text_count; ++i) {
        const DTextBox *box = &app->text_boxes[i];
        long dy = 0;
        if (y < box->y) dy = (long)(box->y - y);
        else if (y > box->y + box->height) dy = (long)(y - (box->y + box->height));
        long dx = 0;
        if (x < box->x) dx = (long)(box->x - x);
        else if (x > box->x + box->width) dx = (long)(x - (box->x + box->width));
        long distance = dy * 4096 + dx;
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static bool text_point_at(App *app, double x, double y, TextPoint *point)
{
    ensure_text_boxes(app);
    if (!app->text_count) return false;
    size_t best = nearest_text_run(app, x, y);
    point->run = best;
    point->offset = distill_litehtml_document_text_offset_at(
        app->document, best, (int)(x - app->text_boxes[best].x));
    return true;
}

static size_t run_text_length(const App *app, size_t run)
{
    return run < app->text_count ? strlen(app->text_boxes[run].text) : 0;
}

/* Word boundaries the Latin way: runs of letters and digits, everything else
   a separator. The reader's own scripts are covered, and a script that does
   not put spaces between words is not served by any rule this simple -- there
   a double click selects the run it landed in. */
static bool word_character(unsigned char c)
{
    return g_ascii_isalnum(c) || c >= 0x80 || c == '\'' || c == '_';
}

static void select_word_at(App *app, TextPoint point)
{
    const char *text = app->text_boxes[point.run].text;
    size_t length = strlen(text);
    size_t start = point.offset > length ? length : point.offset;
    size_t end = start;
    while (start > 0 && word_character((unsigned char)text[start - 1])) --start;
    while (end < length && word_character((unsigned char)text[end])) ++end;
    if (start == end) {
        start = 0;
        end = length;
    }
    app->selection_anchor = (TextPoint){point.run, start};
    app->selection_focus = (TextPoint){point.run, end};
    app->has_selection = true;
}

/* A triple click takes the block, which is the paragraph or list item the
   layout put the run in -- the same identity that decides where a copied
   selection breaks its lines. */
static void select_block_at(App *app, TextPoint point)
{
    unsigned int block = app->text_boxes[point.run].block;
    size_t first = point.run, last = point.run;
    while (first > 0 && app->text_boxes[first - 1].block == block) --first;
    while (last + 1 < app->text_count && app->text_boxes[last + 1].block == block) ++last;
    app->selection_anchor = (TextPoint){first, 0};
    app->selection_focus = (TextPoint){last, run_text_length(app, last)};
    app->has_selection = true;
}

static void select_all_text(App *app)
{
    ensure_text_boxes(app);
    if (!app->text_count) return;
    app->selection_anchor = (TextPoint){0, 0};
    app->selection_focus = (TextPoint){app->text_count - 1,
                                       run_text_length(app, app->text_count - 1)};
    app->has_selection = true;
    gtk_widget_queue_draw(app->drawing_area);
}

/* The selected text, assembled from the runs it covers. Runs of one block are
   one paragraph: a line that wrapped is joined with a space, because that
   break was the layout's decision rather than the writer's. A change of block
   is a real break -- one newline for a list item, a blank line between
   paragraphs, which is what pasting somewhere else should look like. */
static char *selection_text(const App *app)
{
    if (!app->has_selection || !app->text_count) return g_strdup("");
    TextPoint start, end;
    selection_range(app, &start, &end);
    GString *out = g_string_new(NULL);
    for (size_t i = start.run; i <= end.run && i < app->text_count; ++i) {
        const DTextBox *box = &app->text_boxes[i];
        size_t length = strlen(box->text);
        size_t from = i == start.run ? start.offset : 0;
        size_t to = i == end.run ? end.offset : length;
        if (from > length) from = length;
        if (to > length) to = length;
        if (i > start.run) {
            const DTextBox *previous = &app->text_boxes[i - 1];
            bool spaced = out->len && out->str[out->len - 1] == ' ';
            if (box->block != previous->block)
                g_string_append(out, strcmp(box->block_tag, "li") == 0 ? "\n" : "\n\n");
            else if (!spaced && (box->y != previous->y ||
                                 box->x > previous->x + previous->width))
                g_string_append_c(out, ' ');
        }
        if (to > from) g_string_append_len(out, box->text + from, (gssize)(to - from));
    }
    return g_string_free(out, FALSE);
}

static void copy_selection(App *app, bool primary)
{
    char *text = selection_text(app);
    if (!text || !*text) {
        g_free(text);
        return;
    }
    GdkAtom target = primary ? GDK_SELECTION_PRIMARY : GDK_SELECTION_CLIPBOARD;
    gtk_clipboard_set_text(gtk_clipboard_get(target), text, -1);
    if (!primary) {
        long characters = g_utf8_strlen(text, -1);
        char *message = g_strdup_printf("Copied %ld characters", characters);
        set_status(app, message);
        g_free(message);
    }
    g_free(text);
}

/* The highlight is painted over the finished surface rather than into it: the
   text is already drawn, so a wash is what there is room for, and a drag costs
   a repaint of the band instead of a re-render of the page. The tint follows
   the theme, because the same wash cannot stay readable on a cream page and on
   a dark one. */
static void draw_selection(App *app, cairo_t *cairo)
{
    if (!app->has_selection || !app->text_count || !app->document) return;
    TextPoint start, end;
    selection_range(app, &start, &end);
    if (text_point_compare(start, end) == 0) return;
    const char *theme = active_theme_id(app);
    if (strcmp(theme, "night") == 0) cairo_set_source_rgba(cairo, 0.47, 0.73, 0.81, 0.32);
    else if (strcmp(theme, "paper") == 0) cairo_set_source_rgba(cairo, 0.47, 0.33, 0.23, 0.26);
    else cairo_set_source_rgba(cairo, 0.09, 0.36, 0.48, 0.24);
    for (size_t i = start.run; i <= end.run && i < app->text_count; ++i) {
        const DTextBox *box = &app->text_boxes[i];
        int from = 0;
        int to = box->width;
        if (i == start.run)
            from = distill_litehtml_document_text_x_at(app->document, i, start.offset);
        if (i == end.run)
            to = distill_litehtml_document_text_x_at(app->document, i, end.offset);
        if (to <= from) continue;
        cairo_rectangle(cairo, box->x + from, box->y, to - from, box->height);
    }
    cairo_fill(cairo);
}

static gboolean draw_page(GtkWidget *widget, cairo_t *cairo, gpointer opaque)
{
    App *app = opaque;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    cairo_set_source_rgb(cairo, 0.10, 0.11, 0.12);
    cairo_paint(cairo);
    if (app->surface && cairo_surface_status(app->surface) == CAIRO_STATUS_SUCCESS) {
        cairo_set_source_surface(cairo, app->surface, 0, app->surface_top);
        cairo_paint(cairo);
        draw_selection(app, cairo);
    } else {
        cairo_set_source_rgb(cairo, 0.82, 0.79, 0.73);
        cairo_select_font_face(cairo, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cairo, 20);
        cairo_move_to(cairo, 36, 54);
        cairo_show_text(cairo, "Type a URL above to start reading");
    }
    (void)allocation;
    return FALSE;
}

static int visible_scroll_y(const App *app)
{
    if (!app || !app->vertical_adjustment) return 0;
    double value = gtk_adjustment_get_value(app->vertical_adjustment);
    return value > 0.0 ? (int)value : 0;
}

static void render_visible_band(App *app)
{
    if (!app || !app->document) return;
    int scroll_y = visible_scroll_y(app);
    int top = scroll_y - RENDER_OVERSCAN;
    if (top < 0) top = 0;
    if (!distill_litehtml_document_render_region(app->document, top)) return;
    app->surface_top = top;
    app->surface = (cairo_surface_t *)distill_litehtml_document_surface(app->document);
    if (app->surface) cairo_surface_flush(app->surface);
    gtk_widget_queue_draw(app->drawing_area);
}

static void scroll_value_changed(GtkAdjustment *adjustment, gpointer opaque)
{
    (void)adjustment;
    render_visible_band(opaque);
}

static bool point_in_box(double x, double y, int box_x, int box_y, int width, int height)
{
    return x >= box_x && y >= box_y && x <= box_x + width && y <= box_y + height;
}

static void build_document(App *app, const char *template);
static void collect_page_media(App *app);
static void app_navigate(App *app, const char *uri, const char *method,
                         const char *data, bool replace_current);
static void open_link(App *app, const char *uri);

/* Ask the laid-out document, not the collected boxes. Only block-level
   anchors have a placement, so a box list finds headlines that happen to fill
   their line and misses every link inside a sentence; litehtml did the layout
   and knows what is under the point either way. The boxes remain for keyboard
   navigation, which needs an ordered list rather than a point. */
static bool link_href_at(const App *app, double x, double y, char *href, size_t capacity)
{
    if (!app || !app->document) return false;
    return distill_litehtml_document_link_at(app->document, (int)x, (int)y,
                                             href, capacity) != 0;
}

static bool link_present_at(const App *app, double x, double y)
{
    char href[sizeof(((DLinkBox *)0)->href)];
    return link_href_at(app, x, y, href, sizeof(href));
}

/* An event arrives in the coordinate space of the widget it was delivered to.
   The scrolled child is allocated the whole document, so its events are
   already document coordinates; a widget allocated only the viewport is not.
   Deciding from the allocation rather than assuming removes a guess that was
   silently wrong for every click below the fold -- and right at the top of the
   page, where the scroll offset is zero and both readings agree. */
static double document_y_from(int content_height, int allocation_height,
                              int scroll, double y)
{
    if (content_height > 0 && allocation_height >= content_height) return y;
    return y + scroll;
}

static double document_y(const App *app, GtkWidget *widget, double y)
{
    if (!app || !widget) return y;
    int content = app->document
                      ? distill_litehtml_document_content_height(app->document) : 0;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    return document_y_from(content, allocation.height, visible_scroll_y(app), y);
}

static gboolean activate_link_at(App *app, double x, double y)
{
    char href[sizeof(((DLinkBox *)0)->href)];
    if (!link_href_at(app, x, y, href, sizeof(href))) return FALSE;
    open_link(app, href);
    return TRUE;
}

static void set_cursor_shape(GtkWidget *widget, GdkCursorType shape)
{
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window) return;
    GdkCursor *cursor = gdk_cursor_new_for_display(gdk_window_get_display(window), shape);
    if (!cursor) return;
    gdk_window_set_cursor(window, cursor);
    g_object_unref(cursor);
}

static bool text_present_at(App *app, double x, double y)
{
    ensure_text_boxes(app);
    for (size_t i = 0; i < app->text_count; ++i) {
        const DTextBox *box = &app->text_boxes[i];
        if (x >= box->x && x <= box->x + box->width &&
            y >= box->y && y <= box->y + box->height) return true;
    }
    return false;
}

/* The pointer says what the surface will do with it: a hand over a link, a
   beam over text that can be selected, an arrow over the rest. */
static void set_reader_cursor(App *app, GtkWidget *widget, double x, double y)
{
    if (link_present_at(app, x, y)) set_cursor_shape(widget, GDK_HAND2);
    else if (text_present_at(app, x, y)) set_cursor_shape(widget, GDK_XTERM);
    else set_cursor_shape(widget, GDK_LEFT_PTR);
}

/* Activating a media link is the only thing that fetches an image, so the
   first one a reader opens costs a request. The shell has no network of its
   own -- libcurl is linked into the engine and nowhere else -- so the request
   is one short-lived engine run that does nothing but fetch and validate. */
static char *fetch_media_file(const char *url, char **error_message)
{
    char *path = temporary_path("distill-media-XXXXXX");
    if (!path) {
        if (error_message) *error_message = g_strdup("unable to create a temporary file");
        return NULL;
    }
    char *engine = engine_path();
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    add_arg(arguments, engine ? engine : "distill");
    add_arg(arguments, "--url");
    add_arg(arguments, url);
    add_arg(arguments, "--fetch-media");
    add_arg(arguments, path);
    g_ptr_array_add(arguments, NULL);
    char *stderr_text = NULL;
    GError *error = NULL;
    gint wait_status = 0;
    gboolean spawned = g_spawn_sync(NULL, (char **)arguments->pdata, NULL,
                                    G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                                    &stderr_text, &wait_status, &error);
    bool ok = spawned && g_spawn_check_wait_status(wait_status, &error);
    g_ptr_array_free(arguments, TRUE);
    g_free(engine);
    if (!ok) {
        if (error_message) {
            /* The engine's own last line says whether this was a bad status,
               a refused scheme, or something that simply was not an image. */
            char *detail = stderr_text ? g_strstrip(g_strdup(stderr_text)) : NULL;
            char *last = detail ? strrchr(detail, '\n') : NULL;
            *error_message = g_strdup(last ? last + 1 : (detail && *detail ? detail : "the image could not be fetched"));
            g_free(detail);
        }
        if (error) g_error_free(error);
        g_free(stderr_text);
        unlink(path);
        g_free(path);
        return NULL;
    }
    if (error) g_error_free(error);
    g_free(stderr_text);
    return path;
}

/* A URL as something to read. The same reason as in the reader template:
   litehtml has no overflow-wrap, so a long URL runs off the edge. */
static char *display_url(const char *url)
{
    const char *text = url ? url : "";
    if (g_str_has_prefix(text, "https://")) text += 8;
    else if (g_str_has_prefix(text, "http://")) text += 7;
    size_t length = strlen(text);
    if (length <= DISPLAY_URL_BUDGET) return g_strdup(text);
    size_t host = strcspn(text, "/");
    if (host > DISPLAY_URL_BUDGET / 2) host = DISPLAY_URL_BUDGET / 2;
    size_t tail = DISPLAY_URL_BUDGET - host - 1;
    return g_strdup_printf("%.*s\xe2\x80\xa6%s", (int)host, text, text + length - tail);
}

static char *escape_markup(const char *text)
{
    return g_markup_escape_text(text ? text : "", -1);
}

/* Viewer mode's page. The image is sized here rather than left to a
   max-width rule, because the shell has already decoded the file and knows
   the shape of it: fit to the column, never enlarge past natural size. */
static char *viewer_html(App *app, const char *url, const char *file,
                         const char *failure)
{
    int count = app->page_media ? (int)app->page_media->len : 0;
    /* A negative index means this image is not one the page offered, so there
       is no "next" to speak of -- only a way back out. */
    bool placed = app->viewer_index >= 0 && app->viewer_index < count;
    GString *out = g_string_new(
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "html,body{margin:0;padding:0;background:#17191b;color:#f5f1e8;}"
        "body{font-family:sans-serif;font-size:16px;}"
        "body{padding:26px 0;}"
        /* Full-bleed stage, margins for everything else: the renderer fits an
           image to the surface width, so an inset stage would clip that much
           off the right of a wide one, and a horizontal padding rule shifts
           sibling blocks in this litehtml build. */
        ".top{font-size:.82em;color:#aaa79f;margin:0 30px 18px;}"
        ".stage{background:#24272a;margin:0 0 16px;}"
        ".stage img{display:block;}"
        ".failed{padding:48px 20px;text-align:center;color:#e6b8ab;}"
        ".url{color:#aaa79f;font-size:.82em;margin:0 30px 18px;}"
        "a{color:#78b9cf;text-decoration:none;}"
        ".nav{margin:0 30px;}.nav a{margin-right:18px;}"
        ".spent{color:#6b6862;margin-right:18px;}"
        /* <main> is not a block element in litehtml's master stylesheet, so
           the viewer puts its content straight in the body rather than
           wrapping it in one and laying out around an inline box. */
        "</style></head><body>");

    if (placed)
        g_string_append_printf(out, "<p class=\"top\">Image %d of %d</p>",
                               app->viewer_index + 1, count);
    else
        g_string_append(out, "<p class=\"top\">Image</p>");

    g_string_append(out, "<div class=\"stage\">");
    DImage image = {0};
    if (file && d_image_load_file(file, &image)) {
        /* Decoded here only to find out whether it can be decoded at all, so
           that a failure is reported with its reason instead of as an empty
           frame. The size is left to the renderer, which fits an image to the
           surface; an explicit CSS size on an <img> reserves the box but is
           not painted by this litehtml build. */
        d_image_free(&image);
        char *escaped = escape_markup(file);
        g_string_append_printf(out, "<img src=\"%s\">", escaped);
        g_free(escaped);
    } else {
        /* Saying why beats an empty frame: "no decoder for gif images" and
           "404" are different problems and a reader can act on the difference. */
        const char *reason = failure && *failure ? failure
                                                 : (file ? d_image_last_error() : "the image could not be fetched");
        char *escaped = escape_markup(*reason ? reason : "the image could not be shown");
        g_string_append_printf(out, "<div class=\"failed\">%s</div>", escaped);
        g_free(escaped);
    }
    g_string_append(out, "</div>");

    char *shown = display_url(url);
    char *escaped_url = escape_markup(shown);
    g_string_append_printf(out, "<p class=\"url\">%s</p>", escaped_url);
    g_free(shown);
    g_free(escaped_url);

    g_string_append(out, "<p class=\"nav\">");
    /* Both labels are always present, and the ends of the list are shown as
       plain text rather than by removing the link. A control that moves when
       it becomes available is a control you click by accident: with Previous
       appearing only from the second image on, Next shifted sideways after
       the first step and the next click landed somewhere else. */
    if (placed && app->viewer_index > 0) {
        char *previous = escape_markup(g_ptr_array_index(app->page_media, app->viewer_index - 1));
        g_string_append_printf(out, "<a href=\"%s%s\">Previous</a>", MEDIA_SCHEME, previous);
        g_free(previous);
    } else {
        g_string_append(out, "<span class=\"spent\">Previous</span>");
    }
    if (placed && app->viewer_index + 1 < count) {
        char *next = escape_markup(g_ptr_array_index(app->page_media, app->viewer_index + 1));
        g_string_append_printf(out, "<a href=\"%s%s\">Next</a>", MEDIA_SCHEME, next);
        g_free(next);
    } else {
        g_string_append(out, "<span class=\"spent\">Next</span>");
    }
    g_string_append_printf(out, "<a href=\"%s\">Close</a>", CLOSE_URI);
    g_string_append(out, "</p><p class=\"top\">Escape closes, arrow keys move.</p>"
                         "</body></html>");
    return g_string_free(out, FALSE);
}

static void forget_viewer_image(App *app)
{
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); app->viewer_file = NULL; }
    g_free(app->viewer_url);
    app->viewer_url = NULL;
    g_free(app->viewer_failure);
    app->viewer_failure = NULL;
}

static void leave_viewer(App *app)
{
    if (!app->viewer_active) return;
    app->viewer_active = false;
    forget_viewer_image(app);
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page) return;
    char *template = d_read_file(page->template_path, NULL);
    build_document(app, template ? template : "");
    free(template);
    collect_page_media(app);
    schedule_reader_scroll_restore(app);
    set_status(app, "Closed the image");
}

/* Draw the page again at the width the window now offers. This is a layout,
   not a navigation: the template is already on disk and the image already
   fetched, so nothing is refetched and no script runs again. */
static void relayout_page(App *app)
{
    if (!app || app->closing || app->loading) return;
    if (reader_page_width(app) == app->page_width) return;
    if (app->viewer_active) {
        if (!app->viewer_url) return;
        char *html = viewer_html(app, app->viewer_url, app->viewer_file, app->viewer_failure);
        build_document(app, html);
        g_free(html);
        return;
    }
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page) {
        set_surface_size(app, reader_page_width(app), PAGE_HEIGHT);
        return;
    }
    /* The text reflows, so the same offset is a different place. Carry the
       position as a fraction of the page and the reader stays where it was
       rather than jumping forward as the measure narrows. */
    double fraction = 0.0;
    if (app->vertical_adjustment && app->page_height > 0)
        fraction = gtk_adjustment_get_value(app->vertical_adjustment) / app->page_height;
    char *template = d_read_file(page->template_path, NULL);
    build_document(app, template ? template : "");
    free(template);
    collect_page_media(app);
    app->reader_scroll_y = fraction * app->page_height;
    schedule_reader_scroll_restore(app);
}

static gboolean relayout_timeout(gpointer opaque)
{
    App *app = opaque;
    app->relayout_source = 0;
    relayout_page(app);
    return G_SOURCE_REMOVE;
}

static void cancel_relayout(App *app)
{
    if (!app || !app->relayout_source) return;
    g_source_remove(app->relayout_source);
    app->relayout_source = 0;
}

static void viewport_width_changed(GtkAdjustment *adjustment, gpointer opaque)
{
    (void)adjustment;
    App *app = opaque;
    if (app->closing || app->loading) return;
    /* Laying the page out changes what the page requests, which brings us
       back here; comparing against the width in hand is what ends that. */
    if (reader_page_width(app) == app->page_width) return;
    cancel_relayout(app);
    app->relayout_source = g_timeout_add(RELAYOUT_SETTLE_MS, relayout_timeout, app);
}

/* Where this image sits among the page's images. -1 when the page does not
   offer it, which is not the same as "the first one": answering 0 for an
   unknown URL is what made Next walk to the second image and stop, because
   every step after the first resolved back to the start. */
static int media_index_of(const App *app, const char *url)
{
    if (!app->page_media || !url) return -1;
    for (guint i = 0; i < app->page_media->len; ++i)
        if (strcmp(g_ptr_array_index(app->page_media, i), url) == 0) return (int)i;
    return -1;
}

static void enter_viewer(App *app, const char *url)
{
    if (!url || !*url) return;
    /* Re-entrant already: entering the viewer replaces the document that the
       click being handled came from. */
    if (app->entering_viewer) return;
    app->entering_viewer = true;
    if (!app->viewer_active && app->vertical_adjustment)
        app->reader_scroll_y = gtk_adjustment_get_value(app->vertical_adjustment);
    int index = media_index_of(app, url);
    set_status(app, "Fetching the image\xe2\x80\xa6");

    char *failure = NULL;
    char *file = fetch_media_file(url, &failure);
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); }
    app->viewer_file = file;
    app->viewer_index = index;
    app->viewer_active = true;
    g_free(app->viewer_url);
    app->viewer_url = g_strdup(url);
    g_free(app->viewer_failure);
    app->viewer_failure = failure ? g_strdup(failure) : NULL;
    char *html = viewer_html(app, url, file, failure);
    build_document(app, html);
    g_free(html);
    gtk_entry_set_text(GTK_ENTRY(app->url_entry), url);
    if (file) set_status(app, "Escape closes the image");
    else set_status(app, failure ? failure : "The image could not be shown");
    g_free(failure);
    /* The keys belong to the viewer now, not to the address bar. */
    gtk_widget_grab_focus(app->event_box);
    app->entering_viewer = false;
}

/* Viewer mode must be usable by keyboard alone: that is a requirement of
   every target, and on the framebuffer and TUI ones there is no pointer at
   all. Escape leaves, the arrows step through the page's images. */
static void step_viewer(App *app, int delta)
{
    if (!app->viewer_active || !app->page_media || !app->page_media->len) return;
    if (app->viewer_index < 0) return;
    int next = app->viewer_index + delta;
    if (next < 0 || next >= (int)app->page_media->len) return;
    /* enter_viewer replaces page_media's owner only when the page changes, so
       the entry stays valid for the call -- but copy it anyway rather than
       depending on that from a distance. */
    char *target = g_strdup(g_ptr_array_index(app->page_media, next));
    enter_viewer(app, target);
    g_free(target);
}

static void apply_zoom(App *app, int zoom);
static void step_zoom(App *app, int direction);

static gboolean window_key_press(GtkWidget *widget, GdkEventKey *event, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    GdkModifierType control = event->state & gtk_accelerator_get_default_mod_mask();
    /* Zoom works in viewer mode too: it is a reader setting, and leaving the
       image to change it and coming back is not a way to read. */
    /* A focused entry keeps its own copy and select-all: the reader's are for
       the page, not for whatever the caret happens to be in. */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    bool editing = focus && (GTK_IS_ENTRY(focus) || GTK_IS_TEXT_VIEW(focus));
    if (control == GDK_CONTROL_MASK && !editing && !app->viewer_active) {
        switch (event->keyval) {
            case GDK_KEY_c:
            case GDK_KEY_C:
                if (!app->has_selection) return FALSE;
                copy_selection(app, false);
                return TRUE;
            case GDK_KEY_a:
            case GDK_KEY_A:
                select_all_text(app);
                return TRUE;
            default:
                break;
        }
    }
    if (control == GDK_CONTROL_MASK) {
        switch (event->keyval) {
            case GDK_KEY_plus:
            case GDK_KEY_equal:
            case GDK_KEY_KP_Add:
                step_zoom(app, 1);
                return TRUE;
            case GDK_KEY_minus:
            case GDK_KEY_underscore:
            case GDK_KEY_KP_Subtract:
                step_zoom(app, -1);
                return TRUE;
            case GDK_KEY_0:
            case GDK_KEY_KP_0:
                apply_zoom(app, ZOOM_DEFAULT);
                return TRUE;
            default:
                break;
        }
    }
    if (!app->viewer_active) {
        if (event->keyval == GDK_KEY_Escape && app->has_selection && !editing) {
            clear_selection(app);
            return TRUE;
        }
        GdkModifierType modifiers = event->state & gtk_accelerator_get_default_mod_mask();
        if (modifiers == GDK_MOD1_MASK) {
            int index = section_index_for_key(event->keyval);
            if (index >= 0) {
                jump_to_section(app, (size_t)index);
                return TRUE;
            }
        }
        return FALSE;
    }
    /* Escape closes wherever the focus is. The address bar keeps the keys it
       needs -- typing, and the arrows that move the caret -- but Escape means
       nothing to a text field here, and "the cursor happens to be in the URL
       bar" is not a reason for the way out of an image to stop working. */
    if (event->keyval == GDK_KEY_Escape) {
        leave_viewer(app);
        return TRUE;
    }
    if (gtk_widget_has_focus(app->url_entry)) return FALSE;
    switch (event->keyval) {
        case GDK_KEY_BackSpace:
            leave_viewer(app);
            return TRUE;
        case GDK_KEY_Left:
        case GDK_KEY_p:
            step_viewer(app, -1);
            return TRUE;
        case GDK_KEY_Right:
        case GDK_KEY_n:
            step_viewer(app, 1);
            return TRUE;
        default:
            return FALSE;
    }
}

static void open_external(App *app, const char *uri)
{
    if (!external_uri_allowed(uri)) {
        set_status(app, "External link blocked: unsupported URI scheme");
        return;
    }
    gchar *argv[] = {(gchar *)"xdg-open", (gchar *)uri, NULL};
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        char *message = g_strdup_printf("External hand-off failed: %s", error ? error->message : "unknown error");
        set_status(app, message);
        g_free(message);
        if (error) g_error_free(error);
    } else {
        set_status(app, "Opened the link with the system handler");
    }
}

static void open_link(App *app, const char *uri)
{
    if (!uri || !*uri) return;
    if (strncmp(uri, MEDIA_SCHEME, strlen(MEDIA_SCHEME)) == 0) {
        enter_viewer(app, uri + strlen(MEDIA_SCHEME));
        return;
    }
    if (strcmp(uri, CLOSE_URI) == 0) { leave_viewer(app); return; }
    if (strncmp(uri, SECTION_SCHEME, strlen(SECTION_SCHEME)) == 0) {
        int index = section_index_for_id(uri + strlen(SECTION_SCHEME));
        if (index >= 0) jump_to_section(app, (size_t)index);
        return;
    }
    if (has_scheme(uri, "mailto") || has_scheme(uri, "tel")) {
        open_external(app, uri);
    } else if (page_uri_allowed(uri)) {
        app_navigate(app, uri, NULL, NULL, false);
    } else {
        open_external(app, uri);
    }
}

static bool activate_choice_at(App *app, int x, int y);

/* A press no longer activates anything: it may be the start of a drag across
   the text. What it was is known on release, which is also where a browser
   decides -- press, move, release without moving is a click. */
static gboolean reader_button_press(App *app, GtkWidget *widget, GdkEventButton *event)
{
    if (event->button != 1) return FALSE;
    int y = document_y(app, widget, event->y);
    if (event->type == GDK_2BUTTON_PRESS || event->type == GDK_3BUTTON_PRESS) {
        TextPoint point;
        if (!text_point_at(app, event->x, y, &point)) return FALSE;
        if (event->type == GDK_2BUTTON_PRESS) select_word_at(app, point);
        else select_block_at(app, point);
        app->selecting = false;
        copy_selection(app, true);
        gtk_widget_queue_draw(app->drawing_area);
        return TRUE;
    }
    if (event->type != GDK_BUTTON_PRESS) return FALSE;
    /* Clicking the page gives the page the keys. Without this the
       address bar keeps them after a click on the text, and Ctrl+C
       copies whatever is in the entry rather than what is selected. A
       form control is its own widget and takes focus for itself. */
    if (app->event_box && !gtk_widget_has_focus(app->event_box))
        gtk_widget_grab_focus(app->event_box);
    clear_selection(app);
    app->press_x = event->x;
    app->press_y = y;
    app->dragged = false;
    app->selecting = true;
    TextPoint point;
    if (text_point_at(app, event->x, y, &point)) {
        app->selection_anchor = point;
        app->selection_focus = point;
    }
    return FALSE;
}

static gboolean reader_button_release(App *app, GtkWidget *widget, GdkEventButton *event)
{
    if (event->button != 1) return FALSE;
    bool dragged = app->dragged;
    app->selecting = false;
    app->dragged = false;
    if (dragged) {
        /* The X11 convention: a finished selection is available to a middle
           click without anyone having asked for a copy. */
        copy_selection(app, true);
        return TRUE;
    }
    int y = document_y(app, widget, event->y);
    return activate_choice_at(app, event->x, y) || activate_link_at(app, event->x, y);
}

static gboolean reader_motion(App *app, GtkWidget *widget, GdkEventMotion *event)
{
    int y = document_y(app, widget, event->y);
    if (app->selecting && (event->state & GDK_BUTTON1_MASK)) {
        if (!app->dragged &&
            (ABS(event->x - app->press_x) > DRAG_THRESHOLD_PX ||
             ABS(y - app->press_y) > DRAG_THRESHOLD_PX))
            app->dragged = true;
        if (app->dragged) {
            TextPoint point;
            if (text_point_at(app, event->x, y, &point)) {
                app->selection_focus = point;
                app->has_selection = true;
                gtk_widget_queue_draw(app->drawing_area);
            }
            return TRUE;
        }
    }
    set_reader_cursor(app, widget, event->x, y);
    return TRUE;
}

static gboolean page_button_press(GtkWidget *widget, GdkEventButton *event, gpointer opaque)
{
    return reader_button_press(opaque, widget, event);
}

static gboolean page_button_release(GtkWidget *widget, GdkEventButton *event, gpointer opaque)
{
    return reader_button_release(opaque, widget, event);
}

static gboolean page_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                   gpointer opaque)
{
    return reader_motion(opaque, widget, event);
}

static gboolean overlay_button_press(GtkWidget *widget, GdkEventButton *event,
                                     gpointer opaque)
{
    return reader_button_press(opaque, widget, event);
}

static gboolean overlay_button_release(GtkWidget *widget, GdkEventButton *event,
                                       gpointer opaque)
{
    return reader_button_release(opaque, widget, event);
}

static gboolean overlay_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                      gpointer opaque)
{
    return reader_motion(opaque, widget, event);
}

static bool is_hidden(const DFormBox *box)
{
    /* Reader inputs have an explicit 28px layout height, and
       add_native_controls retains the same fallback for any control that
       still reports a zero-height replaced-element box. Only discard controls
       that are explicitly hidden or have no horizontal placement at all. */
    return strcasecmp(box->type, "hidden") == 0 || box->width <= 0;
}

static void form_submit(App *app, const FormControl *submit_control);

static gboolean form_control_button_press(GtkWidget *widget, GdkEventButton *event,
                                          gpointer opaque)
{
    (void)opaque;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1)
        gtk_widget_grab_focus(widget);
    return FALSE;
}

static void submit_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    FormControl *control = opaque;
    App *app = g_object_get_data(G_OBJECT(control->widget), "distill-app");
    if (app) form_submit(app, control);
}

static size_t find_form_control_count(const App *app, unsigned int form_index)
{
    size_t count = 0;
    for (size_t i = 0; i < app->control_count; ++i)
        if (app->controls[i].box.form_index == form_index) ++count;
    return count;
}

static bool form_type_is(const DFormBox *box, const char *type)
{
    return box && type && strcasecmp(box->type, type) == 0;
}

static bool form_control_is_submit(const FormControl *control)
{
    return control && form_type_is(&control->box, "submit");
}

static bool form_control_is_choice(const FormControl *control)
{
    return control && (form_type_is(&control->box, "checkbox") ||
                       form_type_is(&control->box, "radio"));
}

static bool activate_choice_at(App *app, int x, int y)
{
    char id[64];
    if (!app || !app->document ||
        !distill_litehtml_document_choice_id_at(app->document, x, y, id, sizeof(id))) return false;
    for (size_t i = 0; i < app->control_count; ++i) {
        FormControl *control = &app->controls[i];
        if (!form_control_is_choice(control) || control->box.disabled ||
            strcmp(control->box.id, id) != 0 || !GTK_IS_TOGGLE_BUTTON(control->widget)) continue;
        if (form_type_is(&control->box, "radio"))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control->widget), TRUE);
        else
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control->widget),
                                         !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control->widget)));
        gtk_widget_grab_focus(control->widget);
        return true;
    }
    return false;
}

static GtkRadioButton *radio_anchor(const App *app, size_t before,
                                    const FormControl *control)
{
    if (!app || !control || !form_type_is(&control->box, "radio")) return NULL;
    for (size_t i = 0; i < before; ++i) {
        const FormControl *candidate = &app->controls[i];
        if (candidate->box.form_index != control->box.form_index ||
            !form_type_is(&candidate->box, "radio") ||
            strcmp(candidate->box.name, control->box.name) != 0 ||
            !GTK_IS_RADIO_BUTTON(candidate->widget)) continue;
        return GTK_RADIO_BUTTON(candidate->widget);
    }
    return NULL;
}

static void select_initial_option(GtkComboBoxText *combo, const char *options,
                                  const char *initial_value)
{
    int active = -1;
    int index = 0;
    for (const char *cursor = options ? options : ""; *cursor;) {
        const char *end = strchr(cursor, '\x1e');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char *record = g_strndup(cursor, length);
        gchar **parts = g_strsplit(record, "\x1f", 3);
        if (parts[0] && parts[1]) {
            gtk_combo_box_text_append(combo, parts[0], parts[1]);
            if (parts[2] && strcmp(parts[2], "1") == 0) active = index;
            else if (active < 0 && initial_value && *initial_value &&
                     strcmp(parts[0], initial_value) == 0) active = index;
            ++index;
        }
        g_strfreev(parts);
        g_free(record);
        if (!end) break;
        cursor = end + 1;
    }
    if (active >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    else if (index > 0) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void reset_select_option(GtkComboBox *combo, const char *initial_value)
{
    if (!combo) return;
    if (!initial_value || !*initial_value ||
        !gtk_combo_box_set_active_id(combo, initial_value))
        gtk_combo_box_set_active(combo, 0);
}

static void reset_control(FormControl *control)
{
    if (!control || !control->widget || control->box.disabled) return;
    if (GTK_IS_ENTRY(control->widget)) {
        gtk_entry_set_text(GTK_ENTRY(control->widget), control->box.value);
    } else if (GTK_IS_TEXT_VIEW(control->widget)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(control->widget));
        gtk_text_buffer_set_text(buffer, control->box.value, -1);
    } else if (GTK_IS_COMBO_BOX(control->widget)) {
        reset_select_option(GTK_COMBO_BOX(control->widget), control->box.value);
    } else if (GTK_IS_TOGGLE_BUTTON(control->widget)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control->widget), control->box.checked);
    }
}

static void reset_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    FormControl *reset = opaque;
    App *app = reset && reset->widget
        ? g_object_get_data(G_OBJECT(reset->widget), "distill-app") : NULL;
    if (!app || !reset) return;
    for (size_t i = 0; i < app->control_count; ++i)
        if (app->controls[i].box.form_index == reset->box.form_index)
            reset_control(&app->controls[i]);
}

static void add_native_controls(App *app)
{
    for (size_t i = 0; i < app->form_count; ++i) {
        FormControl *control = &app->controls[i];
        const DFormBox *box = &control->box;
        if (is_hidden(box)) continue;
        if (form_type_is(box, "submit") || form_type_is(box, "reset") ||
            form_type_is(box, "button")) {
            control->widget = gtk_button_new_with_label(box->caption[0] ? box->caption :
                                                         (box->value[0] ? box->value : "Submit"));
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-button");
            if (form_type_is(box, "submit"))
                g_signal_connect(control->widget, "clicked", G_CALLBACK(submit_clicked), control);
            else if (form_type_is(box, "reset"))
                g_signal_connect(control->widget, "clicked", G_CALLBACK(reset_clicked), control);
        } else if (form_type_is(box, "checkbox")) {
            control->widget = gtk_check_button_new();
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control->widget), box->checked);
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-choice");
        } else if (form_type_is(box, "radio")) {
            GtkRadioButton *anchor = radio_anchor(app, i, control);
            control->widget = anchor ? gtk_radio_button_new_from_widget(anchor)
                                     : gtk_radio_button_new(NULL);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control->widget), box->checked);
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-choice");
        } else if (form_type_is(box, "select")) {
            control->widget = gtk_combo_box_text_new();
            select_initial_option(GTK_COMBO_BOX_TEXT(control->widget), box->options, box->value);
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-select");
        } else if (form_type_is(box, "textarea")) {
            control->widget = gtk_text_view_new();
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(control->widget));
            gtk_text_buffer_set_text(buffer, box->value, -1);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(control->widget), GTK_WRAP_WORD_CHAR);
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-textarea");
        } else {
            control->widget = gtk_entry_new();
            gtk_entry_set_text(GTK_ENTRY(control->widget), box->value);
            gtk_entry_set_placeholder_text(GTK_ENTRY(control->widget),
                                           box->placeholder[0] ? box->placeholder : box->name);
            gtk_entry_set_alignment(GTK_ENTRY(control->widget), 0.0);
            if (form_type_is(box, "password"))
                gtk_entry_set_visibility(GTK_ENTRY(control->widget), FALSE);
            gtk_style_context_add_class(gtk_widget_get_style_context(control->widget),
                                         "distill-form-entry");
            g_signal_connect(control->widget, "activate", G_CALLBACK(submit_clicked), control);
        }
        gtk_widget_set_can_focus(control->widget, TRUE);
        gtk_widget_set_focus_on_click(control->widget, TRUE);
        g_signal_connect(control->widget, "button-press-event",
                         G_CALLBACK(form_control_button_press), NULL);
        g_object_set_data(G_OBJECT(control->widget), "distill-app", app);
        gtk_widget_set_sensitive(control->widget, !box->disabled);
        /* Entries need a usable fallback when CSS supplied no size. A choice
           already has an exact 18px inline rectangle; enlarging it to the
           entry fallback moves the GTK indicator away from the painted label. */
        int control_width = form_control_is_choice(control)
            ? (box->width > 0 ? box->width : 18)
            : (box->width > 20 ? box->width : 120);
        int control_height = form_control_is_choice(control)
            ? (box->height > 0 ? box->height : 18)
            : (box->height > 20 ? box->height : 28);
        gtk_widget_set_size_request(control->widget, control_width, control_height);
        /* GTK's check/radio glyph is visually inset from its allocation. Lift
           the overlay slightly to meet the baseline of the painted label. */
        int control_y = box->y - (form_control_is_choice(control) ? 4 : 0);
        gtk_fixed_put(GTK_FIXED(app->form_layer), control->widget, box->x, control_y);
        gtk_widget_show(control->widget);
    }
}

static char *form_encode(const char *text)
{
    DBuffer buffer;
    dbuf_init(&buffer);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            dbuf_append(&buffer, (const char *)p, 1);
        } else if (*p == ' ') {
            dbuf_puts(&buffer, "+");
        } else {
            dbuf_printf(&buffer, "%%%02X", *p);
        }
    }
    return dbuf_take(&buffer);
}

static void append_form_pair(DBuffer *data, const char *name, const char *value)
{
    if (!name || !*name) return;
    if (data->length) dbuf_puts(data, "&");
    char *encoded_name = form_encode(name);
    char *encoded_value = form_encode(value ? value : "");
    dbuf_puts(data, encoded_name);
    dbuf_puts(data, "=");
    dbuf_puts(data, encoded_value);
    free(encoded_name);
    free(encoded_value);
}

static const char *control_value(const FormControl *control, char **owned)
{
    *owned = NULL;
    if (!control->widget) return control->box.value;
    if (GTK_IS_ENTRY(control->widget)) return gtk_entry_get_text(GTK_ENTRY(control->widget));
    if (GTK_IS_TEXT_VIEW(control->widget)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(control->widget));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        *owned = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        return *owned;
    }
    if (GTK_IS_COMBO_BOX(control->widget)) {
        const char *value = gtk_combo_box_get_active_id(GTK_COMBO_BOX(control->widget));
        return value ? value : "";
    }
    if (GTK_IS_TOGGLE_BUTTON(control->widget))
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control->widget))
            ? control->box.value : "";
    return control->box.value;
}

static bool radio_group_is_active(const App *app, const FormControl *radio)
{
    if (!app || !radio) return false;
    for (size_t i = 0; i < app->control_count; ++i) {
        const FormControl *candidate = &app->controls[i];
        if (candidate->box.form_index != radio->box.form_index ||
            !form_type_is(&candidate->box, "radio") ||
            strcmp(candidate->box.name, radio->box.name) != 0 ||
            !GTK_IS_TOGGLE_BUTTON(candidate->widget)) continue;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(candidate->widget))) return true;
    }
    return false;
}

static bool form_control_is_empty(const App *app, const FormControl *control)
{
    if (!control) return true;
    if (form_type_is(&control->box, "radio")) return !radio_group_is_active(app, control);
    if (form_control_is_choice(control))
        return !control->widget || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control->widget));
    char *owned = NULL;
    const char *value = control_value(control, &owned);
    bool empty = !value || !*value;
    g_free(owned);
    return empty;
}

static bool form_control_is_successful(const FormControl *control,
                                       const FormControl *submit_control)
{
    if (!control || control->box.disabled || !control->box.name[0]) return false;
    if (form_control_is_submit(control)) return control == submit_control;
    if (form_type_is(&control->box, "reset") || form_type_is(&control->box, "button"))
        return false;
    if (form_control_is_choice(control))
        return control->widget && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control->widget));
    return true;
}

static char *append_query(const char *action, const char *data)
{
    const char *hash = strchr(action, '#');
    size_t prefix_length = hash ? (size_t)(hash - action) : strlen(action);
    char *prefix = d_strndup(action, prefix_length);
    const char *separator = strchr(prefix, '?') ? (data && *data ? "&" : "") : (data && *data ? "?" : "");
    char *out = d_cat4(prefix, separator, data ? data : "", hash ? hash : "");
    free(prefix);
    return out;
}

static void form_submit(App *app, const FormControl *submit_control)
{
    if (!submit_control) return;
    unsigned int form_index = submit_control->box.form_index;
    if (!find_form_control_count(app, form_index)) return;
    DBuffer data;
    dbuf_init(&data);
    const char *action = submit_control->box.action;
    const char *method = submit_control->box.method[0] ? submit_control->box.method : "get";
    for (size_t i = 0; i < app->control_count; ++i) {
        const FormControl *control = &app->controls[i];
        if (control->box.form_index != form_index || control->box.disabled ||
            !control->box.required || form_control_is_submit(control) ||
            form_type_is(&control->box, "reset") || form_type_is(&control->box, "button")) continue;
        if (form_control_is_empty(app, control)) {
            char *message = g_strdup_printf("Complete required field: %s",
                                            control->box.name[0] ? control->box.name : "field");
            set_status(app, message);
            g_free(message);
            dbuf_free(&data);
            return;
        }
    }
    for (size_t i = 0; i < app->control_count; ++i) {
        const FormControl *control = &app->controls[i];
        if (control->box.form_index != form_index) continue;
        if (!form_control_is_successful(control, submit_control)) continue;
        char *owned = NULL;
        const char *value = control_value(control, &owned);
        append_form_pair(&data, control->box.name, value);
        g_free(owned);
    }
    if (!page_uri_allowed(action)) {
        set_status(app, "Form blocked: unsupported action URI scheme");
        dbuf_free(&data);
        return;
    }
    char *target = NULL;
    if (strcasecmp(method, "post") == 0) target = g_strdup(action);
    else target = append_query(action, data.data ? data.data : "");
    app_navigate(app, target, strcasecmp(method, "post") == 0 ? "POST" : NULL,
                 strcasecmp(method, "post") == 0 ? (data.data ? data.data : "") : NULL, false);
    g_free(target);
    dbuf_free(&data);
}

/* Lay out one document and take everything the shell needs from it. Both a
   reader page and viewer mode go through here: viewer mode is a state this
   surface enters, not a second window, which is what lets it work the same
   way on a framebuffer with no window manager to open one in. */
static void build_document(App *app, const char *template)
{
    if (app->document) distill_litehtml_document_destroy(app->document);
    app->document = NULL;
    app->surface = NULL;
    app->surface_top = 0;
    clear_controls(app);
    /* The runs belong to the document that is being replaced, and so does
       anything selected in them. */
    g_free(app->text_boxes);
    app->text_boxes = NULL;
    app->text_count = 0;
    app->text_collected = false;
    app->has_selection = false;
    app->selecting = false;
    int page_height = PAGE_HEIGHT;
    int page_width = reader_page_width(app);

    {
        const char *font_dir = *DISTILL_FONT_DIR ? DISTILL_FONT_DIR : getenv("DISTILL_FONT_DIR");
        int viewport = PAGE_HEIGHT;
        if (app->vertical_adjustment) {
            double size = gtk_adjustment_get_page_size(app->vertical_adjustment);
            if (size > 1.0) viewport = (int)size;
        }
        int band_height = viewport + RENDER_OVERSCAN * 2;
        if (band_height < PAGE_HEIGHT) band_height = PAGE_HEIGHT;
        app->document = distill_litehtml_document_create(template, page_width,
                                                         band_height, font_dir);
        if (app->document) {
            page_height = distill_litehtml_document_content_height(app->document);
            if (page_height < 1) page_height = 1;
            app->surface = (cairo_surface_t *)distill_litehtml_document_surface(app->document);
            size_t form_count = distill_litehtml_document_form_boxes(app->document, NULL, 0);
            size_t link_count = distill_litehtml_document_link_boxes(app->document, NULL, 0);
            if (form_count > MAX_FORM_BOXES) form_count = MAX_FORM_BOXES;
            if (link_count > MAX_LINK_BOXES) link_count = MAX_LINK_BOXES;
            app->form_boxes = form_count ? g_new0(DFormBox, form_count) : NULL;
            app->link_boxes = link_count ? g_new0(DLinkBox, link_count) : NULL;
            app->form_count = form_count;
            app->link_count = link_count;
            if (form_count)
                distill_litehtml_document_form_boxes(app->document, app->form_boxes, form_count);
            if (link_count)
                distill_litehtml_document_link_boxes(app->document, app->link_boxes, link_count);
            if (form_count) {
                app->controls = g_new0(FormControl, form_count);
                app->control_count = form_count;
                for (size_t i = 0; i < form_count; ++i) app->controls[i].box = app->form_boxes[i];
                add_native_controls(app);
            }
        }
    }
    set_surface_size(app, page_width, page_height);
    if (app->vertical_adjustment) gtk_adjustment_set_value(app->vertical_adjustment, 0.0);
    render_visible_band(app);
    update_section_menu(app);
}

/* The images the reader may open, taken from the media links in the template.
   Reading them from the laid-out document rather than from a second file keeps
   one description of the page: what is clickable is exactly what is in this
   list. */
static void collect_page_media(App *app)
{
    if (app->page_media) g_ptr_array_free(app->page_media, TRUE);
    app->page_media = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; i < app->link_count; ++i) {
        const char *href = app->link_boxes[i].href;
        if (strncmp(href, MEDIA_SCHEME, strlen(MEDIA_SCHEME)) != 0) continue;
        const char *url = href + strlen(MEDIA_SCHEME);
        if (*url) g_ptr_array_add(app->page_media, g_strdup(url));
    }
}

static void load_surface(App *app, const DAppPage *page)
{
    cancel_reader_scroll_restore(app);
    char *template = d_read_file(page->template_path, NULL);
    build_document(app, template ? template : "");
    free(template);
    /* Navigating away is another way out of viewer mode, and the downloaded
       file goes with it rather than waiting for the session to end. */
    app->viewer_active = false;
    forget_viewer_image(app);
    collect_page_media(app);
    gtk_entry_set_text(GTK_ENTRY(app->url_entry), page->url);
    select_theme_id(app, page->theme);
    select_zoom(app, page->zoom);
    gtk_widget_set_sensitive(app->back_button, d_app_history_can_back(&app->history));
    gtk_widget_set_sensitive(app->forward_button, d_app_history_can_forward(&app->history));
}

static void load_done(GObject *source_object, GAsyncResult *result, gpointer opaque)
{
    (void)source_object;
    App *app = opaque;
    GError *error = NULL;
    LoadResult *loaded = g_task_propagate_pointer(G_TASK(result), &error);
    if (app->closing) {
        if (loaded) free_load_result(loaded);
        if (error) g_error_free(error);
        app_unref(app);
        return;
    }
    if (error) {
        app->loading = false;
        stop_load_feedback(app);
        set_status(app, error->message);
        g_error_free(error);
        app_unref(app);
        return;
    }
    /* A stale result leaves the feedback running: what the status line is
       describing is the newer run, not this one. */
    if (!loaded || loaded->serial != app->load_serial) {
        if (loaded) free_load_result(loaded);
        app_unref(app);
        return;
    }
    app->loading = false;
    stop_load_feedback(app);
    if (!loaded->ok) {
        char *message = g_strdup_printf("Load failed: %s", loaded->error ? loaded->error : "engine failed");
        set_status(app, message);
        g_free(message);
        free_load_result(loaded);
        app_unref(app);
        return;
    }
    bool history_ok;
    if (loaded->replace_current && d_app_history_current(&app->history)) {
        history_ok = d_app_history_replace_current(&app->history, loaded->url,
                                                    loaded->template_path,
                                                    loaded->theme, loaded->zoom,
                                                    page_dispose);
    } else {
        history_ok = d_app_history_push(&app->history, loaded->url,
                                        loaded->template_path,
                                        loaded->theme, loaded->zoom, page_dispose);
    }
    if (!history_ok) {
        free_load_result(loaded);
        set_status(app, "Unable to update navigation history");
        app_unref(app);
        return;
    }
    /* The history owns copies of these names now. */
    g_free(loaded->template_path);
    loaded->template_path = NULL;
    const DAppPage *page = d_app_history_current(&app->history);
    load_surface(app, page);
    gtk_window_set_title(GTK_WINDOW(app->window), page && page->url ? page->url : "Distill");
    set_status(app, "Ready");
    free_load_result(loaded);
    app_unref(app);
}

static void app_begin_load(App *app, const char *uri, const char *method,
                           const char *data, bool replace_current)
{
    char *unwrapped = unwrap_redirect(uri);
    if (unwrapped) {
        app_begin_load(app, unwrapped, method, data, replace_current);
        g_free(unwrapped);
        return;
    }
    if (!page_uri_allowed(uri)) {
        set_status(app, "Navigation blocked: use an http(s), file, or local path URL");
        return;
    }
    LoadRequest *request = g_new0(LoadRequest, 1);
    request->url = g_strdup(uri);
    request->method = g_strdup(method ? method : "");
    request->data = g_strdup(data ? data : "");
    request->theme = g_strdup(active_theme_id(app));
    request->zoom = active_zoom(app);
    request->width = reader_page_width(app);
    request->progress = load_progress_new();
    load_progress_unref(app->progress);
    app->progress = load_progress_ref(request->progress);
    request->serial = ++app->load_serial;
    request->replace_current = replace_current;
    app->loading = true;
    app_ref(app);
    set_status(app, "Loading…");
    start_load_feedback(app);
    GTask *task = g_task_new(NULL, NULL, load_done, app);
    g_task_set_task_data(task, request, (GDestroyNotify)free_load_request);
    g_task_run_in_thread(task, load_worker);
    g_object_unref(task);
}

static void app_navigate(App *app, const char *uri, const char *method,
                         const char *data, bool replace_current)
{
    if (app->loading) {
        set_status(app, "A page is still loading");
        return;
    }
    app_begin_load(app, uri, method, data, replace_current);
}

/* Does this token name a place, or describe one? The address bar accepts
   both, and has to decide from a single string. The rule is the one browsers
   converged on, because users have learned it: a scheme, a path, or a dotted
   host is somewhere to go; a bare word or anything containing a space is
   something to look for. "dn.se" navigates, "kingfisher hunting" searches. A
   bare word that is also a hostname -- an intranet name, say -- is searched,
   and typing a scheme forces navigation. */
static bool entry_looks_like_url(const char *text)
{
    if (!text || !*text) return false;
    if (has_scheme(text, "http") || has_scheme(text, "https") || has_scheme(text, "file")) return true;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t') return false;
    }
    if (text[0] == '/' || text[0] == '~' ||
        strncmp(text, "./", 2) == 0 || strncmp(text, "../", 3) == 0) return true;
    /* A scheme we do not handle is not a search term either; let the
       navigation check reject it by name. */
    const char *colon = strchr(text, ':');
    const char *slash = strchr(text, '/');
    if (colon && (!slash || colon < slash)) {
        bool port = true;
        for (const char *p = colon + 1; *p && *p != '/'; ++p) {
            if (*p < '0' || *p > '9') { port = false; break; }
        }
        if (!port) return false;
    }
    const char *host_end = strpbrk(text, "/?#");
    size_t host_len = host_end ? (size_t)(host_end - text) : strlen(text);
    char *host = g_strndup(text, host_len);
    if (colon && colon < text + host_len) host[colon - text] = '\0';
    bool ok = false;
    if (g_ascii_strcasecmp(host, "localhost") == 0) {
        ok = true;
    } else {
        const char *dot = strrchr(host, '.');
        if (dot && dot != host && dot[1]) {
            /* A top-level domain is alphabetic, which is what separates a
               hostname from a version number: "3.14" is not a host. An
               address is the other accepted form, and must be four numeric
               labels rather than merely digits and dots. */
            bool alpha = true;
            size_t label = 0;
            for (const char *p = dot + 1; *p; ++p, ++label) {
                if (!g_ascii_isalpha(*p)) alpha = false;
            }
            ok = alpha && label >= 2;
            if (!ok && strspn(host, "0123456789.") == strlen(host)) {
                char **parts = g_strsplit(host, ".", -1);
                size_t count = g_strv_length(parts);
                bool address = count == 4;
                for (size_t i = 0; address && i < count; ++i) {
                    if (!*parts[i] || strlen(parts[i]) > 3) address = false;
                }
                g_strfreev(parts);
                ok = address;
            }
        }
    }
    g_free(host);
    return ok;
}

/* Turn what was typed into something to fetch. */
static char *resolve_entry_input(const char *raw)
{
    char *text = g_strstrip(g_strdup(raw ? raw : ""));
    if (!*text) { g_free(text); return NULL; }
    if (entry_looks_like_url(text)) {
        if (has_scheme(text, "http") || has_scheme(text, "https") || has_scheme(text, "file") ||
            text[0] == '/' || text[0] == '~' ||
            strncmp(text, "./", 2) == 0 || strncmp(text, "../", 3) == 0) {
            return text;
        }
        /* Loopback names have no certificate to present, so asking for https
           there fails for a reason that has nothing to do with the request. */
        bool loopback = g_ascii_strncasecmp(text, "localhost", 9) == 0 ||
                        g_ascii_strncasecmp(text, "127.0.0.1", 9) == 0;
        char *url = g_strconcat(loopback ? "http://" : "https://", text, NULL);
        g_free(text);
        return url;
    }
    const char *engine = g_getenv("DISTILL_SEARCH_URL");
    if (!engine || !*engine) engine = DISTILL_DEFAULT_SEARCH_URL;
    char *query = form_encode(text);
    char *url = g_strconcat(engine, query, NULL);
    free(query);
    g_free(text);
    return url;
}

static void url_activate(GtkWidget *widget, gpointer opaque)
{
    App *app = opaque;
    char *uri = resolve_entry_input(gtk_entry_get_text(GTK_ENTRY(widget)));
    if (!uri) {
        set_status(app, "Type a URL or a search term");
        return;
    }
    app_navigate(app, uri, NULL, NULL, false);
    g_free(uri);
}

static void show_history_page(App *app)
{
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page) return;
    load_surface(app, page);
    gtk_window_set_title(GTK_WINDOW(app->window), page->url);
    set_status(app, "Ready");
}

static void back_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    if (app->loading) return;
    if (d_app_history_back(&app->history)) show_history_page(app);
}

static void forward_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    if (app->loading) return;
    if (d_app_history_forward(&app->history)) show_history_page(app);
}

static void external_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    const DAppPage *page = d_app_history_current(&app->history);
    if (page) open_external(app, page->url);
}

static void theme_changed(GtkCheckMenuItem *item, gpointer opaque)
{
    if (!gtk_check_menu_item_get_active(item)) return;
    App *app = opaque;
    if (app->updating_theme || app->loading) return;
    const char *theme = g_object_get_data(G_OBJECT(item), "distill-theme-id");
    if (!valid_theme_id(theme)) return;
    save_theme(theme);
    update_form_style(app, theme, active_zoom(app));
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page || strcmp(theme, page->theme) == 0) return;
    app_begin_load(app, page->url, NULL, NULL, true);
}

/* Zoom is laid out by the engine, not scaled after the fact, so changing it
   means rendering the page again. The current entry is replaced rather than
   pushed: the same page at another size is not somewhere else to go back to. */
static void apply_zoom(App *app, int zoom)
{
    if (!valid_zoom(zoom) || app->loading) return;
    if (zoom != active_zoom(app)) select_zoom(app, zoom);
    save_zoom(zoom);
    update_form_style(app, active_theme_id(app), zoom);
    char *message = g_strdup_printf("Zoom %d%%", zoom);
    set_status(app, message);
    g_free(message);
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page || page->zoom == zoom) return;
    app_begin_load(app, page->url, NULL, NULL, true);
}

static void zoom_changed(GtkCheckMenuItem *item, gpointer opaque)
{
    if (!gtk_check_menu_item_get_active(item)) return;
    App *app = opaque;
    if (app->updating_zoom || app->loading) return;
    int zoom = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "distill-zoom"));
    apply_zoom(app, zoom);
}

/* Ctrl+= and Ctrl+- move through the offered levels rather than by a fixed
   percentage, so the keys and the menu can never disagree about where the
   reader is. The ends hold rather than wrap: zooming in past the largest step
   should do nothing, not drop the page back to the smallest. */
static void step_zoom(App *app, int direction)
{
    int next = zoom_step_level(active_zoom(app), direction);
    if (next == active_zoom(app)) return;
    apply_zoom(app, next);
}

static GtkWidget *make_sections_button(App *app)
{
    GtkWidget *menu = gtk_menu_new();
    for (size_t i = 0; i < PAGE_SECTION_COUNT; ++i) {
        char *label = g_strdup_printf("%s    %s", page_sections[i].label,
                                      page_sections[i].shortcut);
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_free(label);
        g_object_set_data(G_OBJECT(item), "distill-section-index",
                          GINT_TO_POINTER((gint)i));
        gtk_widget_set_sensitive(item, FALSE);
        g_signal_connect(item, "activate", G_CALLBACK(section_item_activate), app);
        app->section_items[i] = item;
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    GtkWidget *button = gtk_menu_button_new();
    GtkWidget *icon = gtk_image_new_from_icon_name("view-list-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(button), icon);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, "Page sections · Alt+0–6");
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    gtk_widget_show_all(menu);
    return button;
}

static GtkWidget *make_settings_button(App *app)
{
    static const char *theme_ids[] = {"warm", "night", "paper"};
    static const char *theme_labels[] = {"Warm (Light)", "Night (Dark)", "Paper (Light)"};

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *external = gtk_menu_item_new_with_label("Open externally");
    g_signal_connect(external, "activate", G_CALLBACK(external_clicked), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), external);

    GtkWidget *theme_item = gtk_menu_item_new_with_label("Theme");
    GtkWidget *theme_menu = gtk_menu_new();
    GSList *group = NULL;
    for (size_t i = 0; i < sizeof(theme_ids) / sizeof(theme_ids[0]); ++i) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, theme_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "distill-theme-id", (gpointer)theme_ids[i]);
        app->theme_items[i] = item;
        g_signal_connect(item, "toggled", G_CALLBACK(theme_changed), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(theme_menu), item);
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->theme_items[0]), TRUE);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(theme_item), theme_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), theme_item);

    GtkWidget *zoom_item = gtk_menu_item_new_with_label("Zoom");
    GtkWidget *zoom_menu = gtk_menu_new();
    GSList *zoom_group = NULL;
    for (int i = 0; i < ZOOM_LEVEL_COUNT; ++i) {
        char *label = zoom_levels[i] == ZOOM_DEFAULT
                          ? g_strdup_printf("%d%%    Ctrl+0", zoom_levels[i])
                          : g_strdup_printf("%d%%", zoom_levels[i]);
        GtkWidget *item = gtk_radio_menu_item_new_with_label(zoom_group, label);
        g_free(label);
        zoom_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "distill-zoom", GINT_TO_POINTER(zoom_levels[i]));
        app->zoom_items[i] = item;
        g_signal_connect(item, "toggled", G_CALLBACK(zoom_changed), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(zoom_menu), item);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(zoom_item), zoom_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), zoom_item);

    GtkWidget *button = gtk_menu_button_new();
    GtkWidget *icon = gtk_image_new_from_icon_name("preferences-system-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(button), icon);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, "Settings");
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    gtk_widget_show_all(menu);
    return button;
}

static GtkWidget *make_navigation_button(const char *icon_name, const char *tooltip,
                                         GCallback callback, App *app)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 26);
    gtk_button_set_image(GTK_BUTTON(button), icon);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_signal_connect(button, "clicked", callback, app);
    return button;
}

static void app_destroy(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    app->closing = true;
    cancel_reader_scroll_restore(app);
    cancel_relayout(app);
    stop_load_feedback(app);
    if (app->document) distill_litehtml_document_destroy(app->document);
    app->document = NULL;
    app->surface = NULL;
    clear_controls(app);
    g_free(app->text_boxes);
    app->text_boxes = NULL;
    app->text_count = 0;
    if (app->form_css_provider) {
        GdkScreen *screen = gtk_widget_get_screen(app->window);
        if (screen)
            gtk_style_context_remove_provider_for_screen(
                screen, GTK_STYLE_PROVIDER(app->form_css_provider));
        g_object_unref(app->form_css_provider);
        app->form_css_provider = NULL;
    }
    d_app_history_clear(&app->history, page_dispose);
    if (app->page_media) g_ptr_array_free(app->page_media, TRUE);
    forget_viewer_image(app);
    g_application_quit(app->application);
    app_unref(app);
}

static void app_activate(GtkApplication *application, gpointer opaque)
{
    const char *initial_url = opaque;
    char *saved_theme = load_saved_theme();
    int saved_zoom = load_saved_zoom();
    App *app = g_new0(App, 1);
    app->application = G_APPLICATION(application);
    app->references = 1;
    d_app_history_init(&app->history);

    app->window = gtk_application_window_new(application);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1080, 800);
    gtk_window_set_title(GTK_WINDOW(app->window), "Distill");
    g_signal_connect(app->window, "destroy", G_CALLBACK(app_destroy), app);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(window_key_press), app);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 8);
    gtk_widget_set_margin_bottom(toolbar, 8);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);

    app->back_button = make_navigation_button("go-previous-symbolic", "Back",
                                              G_CALLBACK(back_clicked), app);
    app->forward_button = make_navigation_button("go-next-symbolic", "Forward",
                                                  G_CALLBACK(forward_clicked), app);
    gtk_widget_set_sensitive(app->back_button, FALSE);
    gtk_widget_set_sensitive(app->forward_button, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), app->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->forward_button, FALSE, FALSE, 0);
    app->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->url_entry), "Enter an http(s) URL or local HTML file");
    gtk_widget_set_hexpand(app->url_entry, TRUE);
    g_signal_connect(app->url_entry, "activate", G_CALLBACK(url_activate), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->url_entry, TRUE, TRUE, 0);
    app->section_button = make_sections_button(app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->section_button, FALSE, FALSE, 0);
    app->settings_button = make_settings_button(app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->settings_button, FALSE, FALSE, 0);
    select_zoom(app, saved_zoom);
    update_form_style(app, saved_theme, saved_zoom);
    select_theme_id(app, saved_theme);
    g_free(saved_theme);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    app->vertical_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    g_signal_connect(app->vertical_adjustment, "value-changed",
                     G_CALLBACK(scroll_value_changed), app);
    /* The horizontal adjustment's page size is the visible width whatever the
       page requested, which is what the span is measured against. */
    app->horizontal_adjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
    g_signal_connect(app->horizontal_adjustment, "changed",
                     G_CALLBACK(viewport_width_changed), app);
    gtk_style_context_add_class(gtk_widget_get_style_context(scrolled), "distill-desk");
    gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_add_events(overlay, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(overlay, "button-release-event", G_CALLBACK(overlay_button_release), app);
    g_signal_connect(overlay, "button-press-event", G_CALLBACK(overlay_button_press), app);
    g_signal_connect(overlay, "motion-notify-event", G_CALLBACK(overlay_motion_notify), app);
    gtk_container_add(GTK_CONTAINER(scrolled), overlay);
    app->event_box = gtk_event_box_new();
    /* Focusable so that entering viewer mode can take the keys away from the
       address bar: an event box cannot hold focus by default, and
       gtk_widget_grab_focus on a widget that cannot is a no-op. */
    gtk_widget_set_can_focus(app->event_box, TRUE);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(app->event_box), FALSE);
    gtk_event_box_set_above_child(GTK_EVENT_BOX(app->event_box), TRUE);
    gtk_widget_set_halign(app->event_box, GTK_ALIGN_CENTER);
    gtk_widget_add_events(app->event_box, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(app->event_box, "button-release-event", G_CALLBACK(page_button_release), app);
    g_signal_connect(app->event_box, "button-press-event", G_CALLBACK(page_button_press), app);
    g_signal_connect(app->event_box, "motion-notify-event", G_CALLBACK(page_motion_notify), app);
    app->drawing_area = gtk_drawing_area_new();

    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(draw_page), app);
    gtk_container_add(GTK_CONTAINER(app->event_box), app->drawing_area);
    gtk_container_add(GTK_CONTAINER(overlay), app->event_box);
    app->form_layer = gtk_fixed_new();
    gtk_widget_set_can_focus(app->form_layer, FALSE);
    gtk_widget_set_halign(app->form_layer, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->form_layer);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), app->form_layer, FALSE);
    set_surface_size(app, reader_page_width(app), PAGE_HEIGHT);

    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_row, 10);
    gtk_widget_set_margin_top(status_row, 5);
    gtk_widget_set_margin_bottom(status_row, 5);
    app->spinner = gtk_spinner_new();
    /* Shown only while something is running: a spinner that is always there
       says nothing, and one that is there but still says the wrong thing. */
    gtk_widget_set_no_show_all(app->spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(status_row), app->spinner, FALSE, FALSE, 0);
    app->status = gtk_label_new("Ready");
    gtk_widget_set_halign(app->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status_row), app->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), status_row, FALSE, FALSE, 0);
    gtk_widget_show_all(app->window);
    if (initial_url && *initial_url) app_begin_load(app, initial_url, NULL, NULL, false);
}

static bool expect_resolved(const char *input, const char *expected)
{
    char *got = resolve_entry_input(input);
    bool ok = (!expected && !got) || (expected && got && strcmp(got, expected) == 0);
    if (!ok) {
        fprintf(stderr, "address bar: %s -> %s, expected %s\n",
                input, got ? got : "(nothing)", expected ? expected : "(nothing)");
    }
    g_free(got);
    return ok;
}

static bool run_coordinate_self_test(void)
{
    /* The link "Matematisk modell kan förklara resultat" sits at document
       y=1296 on one dn.se front page. Scrolled 400px down, the pointer is at
       viewport y=896. Reading that as document space and adding the scroll
       again lands at 1696 -- a different part of the page, which is why every
       click below the fold missed while clicks at the very top worked. */
    bool ok = true;
    if (document_y_from(17664, 17664, 400, 896) != 896) {
        fprintf(stderr, "coords: scrolled child must already be document space\n");
        ok = false;
    }
    if (document_y_from(17664, 1100, 400, 896) != 1296) {
        fprintf(stderr, "coords: viewport-sized widget must gain the scroll offset\n");
        ok = false;
    }
    /* At the top of the page both readings agree, which is what hid this. */
    if (document_y_from(17664, 17664, 0, 300) != document_y_from(17664, 1100, 0, 300)) {
        fprintf(stderr, "coords: readings must agree at scroll 0\n");
        ok = false;
    }
    return ok;
}

static bool run_address_bar_self_test(void)
{
    const char *search = DISTILL_DEFAULT_SEARCH_URL;
    char *kingfisher = g_strconcat(search, "kingfisher+hunting", NULL);
    char *bare = g_strconcat(search, "kingfisher", NULL);
    char *punctuated = g_strconcat(search, "what+is+a+c%2B%2B+lambda%3F", NULL);
    char *version = g_strconcat(search, "3.14", NULL);
    bool ok =
        /* A dotted host is somewhere to go. */
        expect_resolved("dn.se", "https://dn.se") &&
        expect_resolved("  dn.se  ", "https://dn.se") &&
        expect_resolved("www.bbc.com/news", "https://www.bbc.com/news") &&
        expect_resolved("example.com/a?b=c#d", "https://example.com/a?b=c#d") &&
        /* An explicit scheme is always honoured, including for a file. */
        expect_resolved("http://example.com/", "http://example.com/") &&
        expect_resolved("https://dn.se", "https://dn.se") &&
        expect_resolved("file:///tmp/page.html", "file:///tmp/page.html") &&
        expect_resolved("/tmp/page.html", "/tmp/page.html") &&
        expect_resolved("./page.html", "./page.html") &&
        /* Loopback gets http: there is no certificate to present. */
        expect_resolved("localhost:8080", "http://localhost:8080") &&
        expect_resolved("127.0.0.1:8877/account", "http://127.0.0.1:8877/account") &&
        /* Anything with a space, or a bare word, is a search. */
        expect_resolved("kingfisher hunting", kingfisher) &&
        expect_resolved("kingfisher", bare) &&
        expect_resolved("what is a c++ lambda?", punctuated) &&
        /* A version number is not a hostname, but an address is. */
        expect_resolved("3.14", version) &&
        expect_resolved("192.168.1.10", "https://192.168.1.10") &&
        /* Nothing typed is not a search for nothing. */
        expect_resolved("", NULL) &&
        expect_resolved("   ", NULL);
    g_free(kingfisher); g_free(bare); g_free(punctuated); g_free(version);
    return ok;
}

static bool run_redirect_self_test(void)
{
    char *target = unwrap_redirect(
        "https://duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa%2Db&rut=abc");
    bool ok = target && strcmp(target, "https://example.com/a-b") == 0;
    if (!ok) fprintf(stderr, "redirect: got %s\n", target ? target : "(nothing)");
    g_free(target);
    /* Anything that is not one of those wrappers must be left alone. */
    char *untouched = unwrap_redirect("https://example.com/l/?uddg=x");
    ok = ok && untouched == NULL;
    g_free(untouched);
    return ok;
}

/* A solid PNG, written with stored-deflate blocks so the self-test can make
   one without pulling in an encoder. */
static bool write_solid_test_png(const char *path, int width, int height)
{
    size_t row = (size_t)width * 3 + 1, raw_length = row * (size_t)height;
    unsigned char *raw = g_malloc(raw_length);
    for (int y = 0; y < height; ++y) {
        unsigned char *line = raw + (size_t)y * row;
        line[0] = 0;
        for (int x = 0; x < width; ++x) {
            line[1 + x * 3] = 0x30; line[2 + x * 3] = 0x70; line[3 + x * 3] = 0xb0;
        }
    }
    GByteArray *stream = g_byte_array_new();
    g_byte_array_append(stream, (const guint8 *)"\x78\x01", 2);
    size_t left = raw_length, offset = 0;
    do {
        size_t take = left > 65535 ? 65535 : left;
        guint8 block[5] = {(guint8)((left - take) ? 0 : 1),
                           (guint8)(take & 0xff), (guint8)(take >> 8),
                           (guint8)(~take & 0xff), (guint8)((~take >> 8) & 0xff)};
        g_byte_array_append(stream, block, 5);
        g_byte_array_append(stream, raw + offset, (guint)take);
        offset += take; left -= take;
    } while (left);
    unsigned long s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_length; ++i) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    unsigned long adler = (s2 << 16) | s1;
    guint8 tail[4] = {(guint8)(adler >> 24), (guint8)(adler >> 16), (guint8)(adler >> 8), (guint8)adler};
    g_byte_array_append(stream, tail, 4);
    g_free(raw);

    GByteArray *out = g_byte_array_new();
    g_byte_array_append(out, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
    guint8 ihdr[13] = {(guint8)(width >> 24), (guint8)(width >> 16), (guint8)(width >> 8), (guint8)width,
                       (guint8)(height >> 24), (guint8)(height >> 16), (guint8)(height >> 8), (guint8)height,
                       8, 2, 0, 0, 0};
    const char *types[3] = {"IHDR", "IDAT", "IEND"};
    const guint8 *bodies[3] = {ihdr, stream->data, NULL};
    size_t lengths[3] = {sizeof(ihdr), stream->len, 0};
    for (int i = 0; i < 3; ++i) {
        size_t length = lengths[i];
        guint8 size[4] = {(guint8)(length >> 24), (guint8)(length >> 16), (guint8)(length >> 8), (guint8)length};
        g_byte_array_append(out, size, 4);
        guint start = out->len;
        g_byte_array_append(out, (const guint8 *)types[i], 4);
        if (length) g_byte_array_append(out, bodies[i], (guint)length);
        guint32 check = crc32(0, out->data + start, out->len - start);
        guint8 crc[4] = {(guint8)(check >> 24), (guint8)(check >> 16), (guint8)(check >> 8), (guint8)check};
        g_byte_array_append(out, crc, 4);
    }
    g_byte_array_free(stream, TRUE);
    bool ok = g_file_set_contents(path, (const char *)out->data, out->len, NULL);
    g_byte_array_free(out, TRUE);
    return ok;
}

/* The URL behind one of the viewer's navigation links, as a click would
   resolve it: entity references and all, because the page is HTML and the
   renderer decodes it before the shell ever sees the href. */
static char *viewer_nav_target(const char *html, const char *label)
{
    char *needle = g_strdup_printf(">%s</a>", label);
    const char *end = strstr(html, needle);
    g_free(needle);
    if (!end) return NULL;
    const char *open = end;
    while (open > html && strncmp(open, "<a href=\"", 9) != 0) --open;
    if (strncmp(open, "<a href=\"", 9) != 0) return NULL;
    open += 9;
    const char *close = strchr(open, '"');
    if (!close || close > end) return NULL;
    char *raw = g_strndup(open, (size_t)(close - open));
    char *decoded = NULL;
    if (g_str_has_prefix(raw, MEDIA_SCHEME)) {
        /* g_markup escaped it on the way in; undo exactly that much. */
        char **parts = g_strsplit(raw + strlen(MEDIA_SCHEME), "&amp;", -1);
        decoded = g_strjoinv("&", parts);
        g_strfreev(parts);
    }
    g_free(raw);
    return decoded;
}

/* Walking the page's images was wrong in a way no smoke test would show: an
   unknown URL resolved to index 0, so every step after the first came back to
   the start, and Next appearing only once Previous existed moved the control
   out from under the pointer. Both are index arithmetic over a list, so both
   can be walked here without a window.

   The walk reads each step's destination out of the page the viewer actually
   generates, rather than calling the step function directly: that is what a
   reader clicking Next does, and it is the part that was broken. */
static bool run_viewer_walk_self_test(void)
{
    const char *urls[] = {
        "https://static.example.test/a/one.png?io=1&width=640",
        "https://static.example.test/a/two.jpg?io=1&width=640",
        "https://static.example.test/a/three.webp",
        "https://static.example.test/a/four.png?q=a&r=b",
    };
    const int count = (int)(sizeof(urls) / sizeof(urls[0]));
    App probe = {0};
    probe.page_media = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < count; ++i) g_ptr_array_add(probe.page_media, g_strdup(urls[i]));

    bool ok = true;
    /* Forwards from the first to the last, one Next at a time. */
    int at = 0;
    for (int step = 0; step < count - 1; ++step) {
        probe.viewer_index = at;
        char *html = viewer_html(&probe, urls[at], NULL, "no fetch in a self-test");
        char *target = viewer_nav_target(html, "Next");
        int resolved = target ? media_index_of(&probe, target) : -1;
        if (resolved != at + 1) {
            fprintf(stderr, "viewer walk: Next from %d resolved to %d (%s)\n",
                    at, resolved, target ? target : "no link");
            ok = false;
        }
        g_free(target);
        g_free(html);
        if (resolved < 0) break;
        at = resolved;
    }
    if (at != count - 1) {
        fprintf(stderr, "viewer walk: reached image %d of %d\n", at + 1, count);
        ok = false;
    }

    /* And backwards, which is where "Previous from the last goes to the
       first" would show up. */
    for (int step = 0; step < count - 1; ++step) {
        probe.viewer_index = at;
        char *html = viewer_html(&probe, urls[at], NULL, "no fetch in a self-test");
        char *target = viewer_nav_target(html, "Previous");
        int resolved = target ? media_index_of(&probe, target) : -1;
        if (resolved != at - 1) {
            fprintf(stderr, "viewer walk: Previous from %d resolved to %d (%s)\n",
                    at, resolved, target ? target : "no link");
            ok = false;
        }
        g_free(target);
        g_free(html);
        if (resolved < 0) break;
        at = resolved;
    }
    if (at != 0) { fprintf(stderr, "viewer walk: came back to image %d\n", at + 1); ok = false; }

    /* The ends offer no link in that direction, and say so rather than
       dropping the label and moving the other one. */
    probe.viewer_index = 0;
    char *first = viewer_html(&probe, urls[0], NULL, "x");
    char *back = viewer_nav_target(first, "Previous");
    if (back) { fprintf(stderr, "viewer walk: the first image offered Previous\n"); ok = false; }
    if (!strstr(first, ">Previous<")) { fprintf(stderr, "viewer walk: Previous label missing\n"); ok = false; }
    g_free(back); g_free(first);

    probe.viewer_index = count - 1;
    char *last = viewer_html(&probe, urls[count - 1], NULL, "x");
    char *onward = viewer_nav_target(last, "Next");
    if (onward) { fprintf(stderr, "viewer walk: the last image offered Next\n"); ok = false; }
    if (!strstr(last, ">Next<")) { fprintf(stderr, "viewer walk: Next label missing\n"); ok = false; }
    g_free(onward); g_free(last);

    /* An image the page never listed has no position and no neighbours. */
    if (media_index_of(&probe, "https://static.example.test/a/absent.png") != -1) {
        fprintf(stderr, "viewer walk: an unlisted image was placed in the list\n");
        ok = false;
    }
    g_ptr_array_free(probe.page_media, TRUE);
    return ok;
}

/* Viewer mode is mouse-and-eye work that a headless test cannot click, but/* Viewer mode is mouse-and-eye work that a headless test cannot click, but
   the two decisions behind it are ordinary functions: which destinations are
   the shell's own rather than somewhere to navigate, and what a URL looks like
   once it has been cut down to fit. */
static bool run_media_self_test(void)
{
    bool ok = true;

    char *shown = display_url("https://cdn.example.com/a/photo.jpg");
    ok = shown && strcmp(shown, "cdn.example.com/a/photo.jpg") == 0 && ok;
    if (!shown || strcmp(shown, "cdn.example.com/a/photo.jpg")) 
        fprintf(stderr, "display_url: short URL became %s\n", shown ? shown : "(nothing)");
    g_free(shown);

    /* A URL past the budget keeps its host and its file name, loses the
       middle, and comes back short enough to lay out. */
    const char *long_url =
        "https://cdn.example.com/2026/09/15/assets/rendition/cache/"
        "8f2a1c9e4b7d6053a1f8/harbour-at-dawn-full-bleed.jpg";
    shown = display_url(long_url);
    ok = shown && g_str_has_prefix(shown, "cdn.example.com") && ok;
    ok = shown && g_str_has_suffix(shown, "harbour-at-dawn-full-bleed.jpg") && ok;
    ok = shown && g_utf8_strlen(shown, -1) <= DISPLAY_URL_BUDGET + 1 && ok;
    if (!ok) fprintf(stderr, "display_url: long URL became %s\n", shown ? shown : "(nothing)");
    g_free(shown);

    /* The internal destinations must never be treated as pages to fetch. */
    ok = !page_uri_allowed(MEDIA_SCHEME "https://example.com/a.png") && ok;
    ok = !page_uri_allowed(CLOSE_URI) && ok;
    ok = !external_uri_allowed(MEDIA_SCHEME "https://example.com/a.png") && ok;
    if (!ok) fprintf(stderr, "media: an internal destination was treated as a URL\n");

    /* Viewer mode's page, laid out by the renderer the shell actually uses.
       Markup and renderer have to agree about the image or the reader gets an
       empty frame, and that agreement is checkable without a window: a
       document holding a 600x400 image is at least 400 tall, and one that
       failed to find the image is nowhere near. */
    App probe = {0};
    probe.page_media = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(probe.page_media, g_strdup("https://example.test/photo.png"));
    char *source = NULL;
    int descriptor = g_file_open_tmp("distill-selftest-XXXXXX.png", &source, NULL);
    bool checked = false;
    if (descriptor >= 0) {
        close(descriptor);
        if (write_solid_test_png(source, 600, 400)) {
            char *html = viewer_html(&probe, "https://example.test/photo.png", source, NULL);
            const char *font_dir = *DISTILL_FONT_DIR ? DISTILL_FONT_DIR : getenv("DISTILL_FONT_DIR");
            int height = distill_litehtml_content_height(html, PAGE_WIDTH_DESIGN, font_dir);
            checked = true;
            if (height < 400) {
                fprintf(stderr, "viewer: a 600x400 image laid out %d tall\n", height);
                ok = false;
            }
            /* And the page must offer the ways back out of it. */
            if (!strstr(html, CLOSE_URI)) {
                fprintf(stderr, "viewer: no way to close the image\n");
                ok = false;
            }
            g_free(html);
        }
        unlink(source);
    }
    /* A skipped check is a failed check: this is the assertion that would
       have caught the image not being drawn at all. */
    if (!checked) {
        fprintf(stderr, "viewer: could not stage a test image, check skipped\n");
        ok = false;
    }
    g_free(source);
    g_ptr_array_free(probe.page_media, TRUE);

    ok = run_viewer_walk_self_test() && ok;
    return ok;
}

/* The keys and the menu read the same table, so what this checks is the part
   that can still be wrong: the ends hold, and an unknown saved value lands on
   the default rather than off the end of the table. */
static bool run_zoom_self_test(void)
{
    bool ok = true;
    if (zoom_levels[0] >= ZOOM_DEFAULT || zoom_levels[ZOOM_LEVEL_COUNT - 1] <= ZOOM_DEFAULT) {
        fprintf(stderr, "zoom: the default is not inside the offered levels\n");
        ok = false;
    }
    if (!valid_zoom(ZOOM_DEFAULT) || valid_zoom(137) || valid_zoom(0)) {
        fprintf(stderr, "zoom: level validation accepts a level the menu cannot show\n");
        ok = false;
    }
    if (zoom_step_level(zoom_levels[0], -1) != zoom_levels[0] ||
        zoom_step_level(zoom_levels[ZOOM_LEVEL_COUNT - 1], 1) !=
            zoom_levels[ZOOM_LEVEL_COUNT - 1]) {
        fprintf(stderr, "zoom: stepping past an end does not hold there\n");
        ok = false;
    }
    if (zoom_step_level(ZOOM_DEFAULT, 1) <= ZOOM_DEFAULT ||
        zoom_step_level(ZOOM_DEFAULT, -1) >= ZOOM_DEFAULT ||
        zoom_step_level(zoom_step_level(ZOOM_DEFAULT, 1), -1) != ZOOM_DEFAULT) {
        fprintf(stderr, "zoom: stepping does not move one level at a time\n");
        ok = false;
    }
    if (zoom_step_level(137, 0) != ZOOM_DEFAULT) {
        fprintf(stderr, "zoom: an unknown level does not fall back to the default\n");
        ok = false;
    }
    return ok;
}

/* The span is the whole feature: a window wider than the design width must
   stop the page growing, a much narrower one must stop it shrinking, and
   between the two the page is the window. */
static bool run_width_self_test(void)
{
    bool ok = true;
    if (clamp_page_width(1920, 100) != PAGE_WIDTH_DESIGN ||
        clamp_page_width(PAGE_WIDTH_DESIGN, 100) != PAGE_WIDTH_DESIGN) {
        fprintf(stderr, "width: a wide window does not stop at the design width\n");
        ok = false;
    }
    if (clamp_page_width(320, 100) != PAGE_WIDTH_FLOOR ||
        clamp_page_width(0, 100) != PAGE_WIDTH_FLOOR) {
        fprintf(stderr, "width: a narrow window shrinks the page past the floor\n");
        ok = false;
    }
    if (clamp_page_width(600, 100) != 600) {
        fprintf(stderr, "width: a window inside the span does not set the width\n");
        ok = false;
    }
    if (clamp_page_width(1920, 200) != 2 * PAGE_WIDTH_DESIGN ||
        clamp_page_width(320, 200) != 2 * PAGE_WIDTH_FLOOR) {
        fprintf(stderr, "width: zoom does not move both ends of the span\n");
        ok = false;
    }
    if (clamp_page_width(1920, 50) >= clamp_page_width(1920, 100)) {
        fprintf(stderr, "width: zooming out does not narrow the page\n");
        ok = false;
    }
    return ok;
}

/* The pipe hands over whatever it has, which is not whole lines, and the
   status line is only as truthful as this parse. The phase vocabulary is
   shared with the engine, so a name that no longer matches shows up here as
   the fallback text rather than as a wrong description of what is happening. */
static bool run_progress_self_test(void)
{
    bool ok = true;
    LoadProgress *progress = load_progress_new();
    GString *pending = g_string_new(NULL);
    int percent = 0;
    char phase[16];

    consume_progress(pending, "progress 12 fet", strlen("progress 12 fet"), progress);
    load_progress_get(progress, &percent, phase, sizeof phase);
    if (percent != 0) {
        fprintf(stderr, "progress: half a record was read as a whole one\n");
        ok = false;
    }
    const char *rest = "ch\nprogress 40 scripts\nprogress 75 ex";
    consume_progress(pending, rest, strlen(rest), progress);
    load_progress_get(progress, &percent, phase, sizeof phase);
    if (percent != 40 || strcmp(phase, "scripts") != 0) {
        fprintf(stderr, "progress: records split across reads did not arrive (%d %s)\n",
                percent, phase);
        ok = false;
    }
    consume_progress(pending, "tract\n", strlen("tract\n"), progress);
    load_progress_get(progress, &percent, phase, sizeof phase);
    if (percent != 75 || strcmp(phase, "extract") != 0) {
        fprintf(stderr, "progress: the last record did not complete (%d %s)\n",
                percent, phase);
        ok = false;
    }
    const char *noise = "phase2 scripts=3 cap_hit=false\n";
    consume_progress(pending, noise, strlen(noise), progress);
    load_progress_get(progress, &percent, phase, sizeof phase);
    if (percent != 75 || strcmp(phase, "extract") != 0) {
        fprintf(stderr, "progress: a line that is not a record moved the count\n");
        ok = false;
    }
    g_string_free(pending, TRUE);
    load_progress_unref(progress);

    static const char *phases[] = {"fetch", "scripts", "extract", "render", "done"};
    char *fallback = loading_message(50, "nonsense");
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; ++i) {
        char *message = loading_message(50, phases[i]);
        if (strcmp(message, fallback) == 0 || !strstr(message, "50%")) {
            fprintf(stderr, "progress: phase %s has no message of its own\n", phases[i]);
            ok = false;
        }
        g_free(message);
    }
    g_free(fallback);
    return ok;
}

/* What a selection turns into is the whole point of the feature: the runs are
   what the layout drew, and the reader wanted the text back. The joins are
   what this checks -- a wrapped line is not a new line, a new paragraph is. */
static bool run_selection_self_test(void)
{
    bool ok = true;
    DTextBox boxes[5] = {
        /* One paragraph over two lines, then a second paragraph, then two
           list items, laid out the way litehtml hands them over. */
        {.x = 0, .y = 0, .width = 200, .height = 20, .block = 1, .block_tag = "p",
         .text = "The harbour was quiet"},
        {.x = 0, .y = 20, .width = 200, .height = 20, .block = 1, .block_tag = "p",
         .text = "that morning"},
        {.x = 0, .y = 60, .width = 200, .height = 20, .block = 2, .block_tag = "p",
         .text = "The tugs were tied up"},
        {.x = 0, .y = 100, .width = 200, .height = 20, .block = 3, .block_tag = "li",
         .text = "First item"},
        {.x = 0, .y = 120, .width = 200, .height = 20, .block = 4, .block_tag = "li",
         .text = "Second item"}
    };
    App app = {0};
    app.text_boxes = boxes;
    app.text_count = sizeof boxes / sizeof boxes[0];
    app.has_selection = true;
    app.selection_anchor = (TextPoint){0, 0};
    app.selection_focus = (TextPoint){4, strlen(boxes[4].text)};
    char *text = selection_text(&app);
    const char *expected =
        "The harbour was quiet that morning\n\n"
        "The tugs were tied up\n"
        "First item\n"
        "Second item";
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "selection: joined as \"%s\"\n", text);
        ok = false;
    }
    g_free(text);

    /* Backwards is the same selection: a drag that ends above where it began
       selects what lies between, not nothing. */
    app.selection_anchor = (TextPoint){1, 4};
    app.selection_focus = (TextPoint){0, 4};
    text = selection_text(&app);
    if (strcmp(text, "harbour was quiet that") != 0) {
        fprintf(stderr, "selection: dragged backwards gave \"%s\"\n", text);
        ok = false;
    }
    g_free(text);

    /* A point outside the text belongs to the nearest line rather than to
       nothing, or a drag into the margin would stop selecting. */
    if (nearest_text_run(&app, 400, 22) != 1 ||
        nearest_text_run(&app, -50, 105) != 3 ||
        nearest_text_run(&app, 10, 9000) != 4) {
        fprintf(stderr, "selection: a point off the text found the wrong line\n");
        ok = false;
    }
    return ok;
}

static int run_state_self_test(void)
{
    DAppHistory history;
    d_app_history_init(&history);
    bool ok = d_app_history_push(&history, "one", "one", "warm", 100, NULL) &&
              d_app_history_push(&history, "two", "two", "night", 125, NULL) &&
              d_app_history_back(&history) &&
              d_app_history_current(&history) &&
              strcmp(d_app_history_current(&history)->url, "one") == 0 &&
              d_app_history_forward(&history);
    d_app_history_clear(&history, NULL);
    ok = run_zoom_self_test() && ok;
    ok = run_width_self_test() && ok;
    ok = run_progress_self_test() && ok;
    ok = run_selection_self_test() && ok;
    ok = run_coordinate_self_test() && ok;
    ok = run_address_bar_self_test() && ok;
    ok = run_redirect_self_test() && ok;
    ok = run_media_self_test() && ok;
    if (ok) puts("app-shell: ok");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *initial_url = NULL;
    bool self_test = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) self_test = true;
        else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) initial_url = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "usage: %s [--url URL] [--self-test]\n", argv[0]);
            return 0;
        } else if (!initial_url && argv[i][0] != '-') {
            initial_url = argv[i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (self_test) return run_state_self_test();
    GtkApplication *application = gtk_application_new("org.distill.Reader", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(app_activate), (gpointer)initial_url);
    int result = g_application_run(G_APPLICATION(application), 0, NULL);
    g_object_unref(application);
    session_file_discard();
    return result;
}
