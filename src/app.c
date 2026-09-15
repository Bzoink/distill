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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DISTILL_ENGINE_PATH
#define DISTILL_ENGINE_PATH "distill"
#endif

#ifndef DISTILL_FONT_DIR
#define DISTILL_FONT_DIR ""
#endif

enum {
    PAGE_WIDTH = 760,
    PAGE_HEIGHT = 1400,
    RENDER_OVERSCAN = 200,
    MAX_FORM_BOXES = 512,
    MAX_LINK_BOXES = 2048,
    /* Long enough to keep a host and a file name, short enough to fit the
       column: litehtml breaks neither on overflow-wrap nor on word-break, so
       a URL that does not fit simply runs off the edge of the page. */
    DISPLAY_URL_BUDGET = 58
};

/* Two internal destinations. They are not URLs to fetch, they are things for
   the shell to do, so they never reach the navigation path. */
#define MEDIA_SCHEME "distill-media:"
#define CLOSE_URI "distill-close:"

typedef struct App App;

typedef struct {
    DFormBox box;
    GtkWidget *widget;
} FormControl;

typedef struct {
    char *url;
    char *method;
    char *data;
    char *theme;
    guint serial;
    bool replace_current;
} LoadRequest;

typedef struct {
    char *url;
    char *template_path;
    char *theme;
    char *error;
    bool ok;
    guint serial;
    bool replace_current;
} LoadResult;

struct App {
    GApplication *application;
    GtkWidget *window;
    GtkWidget *url_entry;
    GtkWidget *go_button;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *external_button;
    GtkWidget *list_button;
    GtkWidget *theme_combo;
    GtkWidget *status;
    GtkWidget *drawing_area;
    GtkWidget *event_box;
    GtkWidget *form_layer;
    GtkAdjustment *vertical_adjustment;
    cairo_surface_t *surface;
    DLiteHtmlDocument *document;
    int surface_top;
    DFormBox *form_boxes;
    size_t form_count;
    FormControl *controls;
    size_t control_count;
    DLinkBox *link_boxes;
    size_t link_count;
    DAppHistory history;
    GPtrArray *reading_list;
    /* The images this page offers, in document order, captured while the page
       document exists -- viewer mode replaces that document, so the list has
       to outlive it for Previous and Next to mean anything. */
    GPtrArray *page_media;
    bool viewer_active;
    bool entering_viewer;
    int viewer_index;
    char *viewer_file;
    guint load_serial;
    unsigned int references;
    bool closing;
    bool loading;
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
    char *width = g_strdup_printf("%d", PAGE_WIDTH);
    add_arg(arguments, width);
    g_free(width);
    /* 0 means "as tall as the content". A reader page is as long as what it
       has to say, and a fixed surface silently drops the rest: the page looks
       finished and simply stops. */
    add_arg(arguments, "--height");
    add_arg(arguments, "0");
    add_arg(arguments, "--theme");
    add_arg(arguments, request->theme);
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

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    GError *error = NULL;
    gint wait_status = 0;
    gboolean spawned = g_spawn_sync(NULL, (char **)arguments->pdata, NULL,
                                    G_SPAWN_SEARCH_PATH, NULL, NULL, &stdout_text,
                                    &stderr_text, &wait_status, &error);
    if (body_path) {
        unlink(body_path);
        g_free(body_path);
    }
    bool ok = spawned && g_spawn_check_wait_status(wait_status, &error);
    if (!ok && error_message) {
        const char *message = error ? error->message : (stderr_text ? stderr_text : "engine failed");
        *error_message = g_strdup(message ? message : "engine failed");
    }
    if (error) g_error_free(error);
    g_free(stdout_text);
    g_free(stderr_text);
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

static void update_list_button(App *app)
{
    const DAppPage *page = d_app_history_current(&app->history);
    bool saved = false;
    if (page) {
        for (guint i = 0; i < app->reading_list->len; ++i) {
            if (strcmp(g_ptr_array_index(app->reading_list, i), page->url) == 0) {
                saved = true;
                break;
            }
        }
    }
    gtk_button_set_label(GTK_BUTTON(app->list_button), saved ? "★" : "☆");
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

static void set_link_cursor(GtkWidget *widget, bool link)
{
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window) return;
    GdkCursor *cursor = gdk_cursor_new_for_display(
        gdk_window_get_display(window), link ? GDK_HAND2 : GDK_LEFT_PTR);
    if (!cursor) return;
    gdk_window_set_cursor(window, cursor);
    g_object_unref(cursor);
}

/* Activating a placeholder is the only thing that fetches an image, so the
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

static void leave_viewer(App *app)
{
    if (!app->viewer_active) return;
    app->viewer_active = false;
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); app->viewer_file = NULL; }
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page) return;
    char *template = d_read_file(page->template_path, NULL);
    build_document(app, template ? template : "");
    free(template);
    collect_page_media(app);
    set_status(app, "Closed the image");
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
    int index = media_index_of(app, url);
    set_status(app, "Fetching the image\xe2\x80\xa6");

    char *failure = NULL;
    char *file = fetch_media_file(url, &failure);
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); }
    app->viewer_file = file;
    app->viewer_index = index;
    app->viewer_active = true;
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

static gboolean window_key_press(GtkWidget *widget, GdkEventKey *event, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    if (!app->viewer_active) return FALSE;
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
    if (has_scheme(uri, "mailto") || has_scheme(uri, "tel")) {
        open_external(app, uri);
    } else if (page_uri_allowed(uri)) {
        app_navigate(app, uri, NULL, NULL, false);
    } else {
        open_external(app, uri);
    }
}

static gboolean page_button_press(GtkWidget *widget, GdkEventButton *event, gpointer opaque)
{
    App *app = opaque;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;
    return activate_link_at(app, event->x, document_y(app, widget, event->y));
}

static gboolean page_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                   gpointer opaque)
{
    App *app = opaque;
    set_link_cursor(widget, link_present_at(app, event->x, document_y(app, widget, event->y)));
    return TRUE;
}

static gboolean overlay_button_press(GtkWidget *widget, GdkEventButton *event,
                                     gpointer opaque)
{
    App *app = opaque;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;
    return activate_link_at(app, event->x, document_y(app, widget, event->y));
}

static gboolean overlay_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                      gpointer opaque)
{
    App *app = opaque;
    set_link_cursor(widget, link_present_at(app, event->x, document_y(app, widget, event->y)));
    return FALSE;
}

static bool is_hidden(const DFormBox *box)
{
    return strcasecmp(box->type, "hidden") == 0 || box->height <= 0 || box->width <= 0;
}

static void form_submit(App *app, const FormControl *submit_control);

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

static void add_native_controls(App *app)
{
    for (size_t i = 0; i < app->form_count; ++i) {
        FormControl *control = &app->controls[i];
        const DFormBox *box = &control->box;
        if (is_hidden(box)) continue;
        if (strcasecmp(box->type, "submit") == 0 || strcasecmp(box->tag, "button") == 0) {
            control->widget = gtk_button_new_with_label(box->value[0] ? box->value : "Submit");
            g_signal_connect(control->widget, "clicked", G_CALLBACK(submit_clicked), control);
        } else {
            control->widget = gtk_entry_new();
            gtk_entry_set_text(GTK_ENTRY(control->widget), box->value);
            gtk_entry_set_placeholder_text(GTK_ENTRY(control->widget), box->name);
            if (strcasecmp(box->type, "password") == 0)
                gtk_entry_set_visibility(GTK_ENTRY(control->widget), FALSE);
            g_signal_connect(control->widget, "activate", G_CALLBACK(submit_clicked), control);
        }
        g_object_set_data(G_OBJECT(control->widget), "distill-app", app);
        gtk_widget_set_size_request(control->widget,
                                    box->width > 20 ? box->width : 120,
                                    box->height > 20 ? box->height : 34);
        gtk_fixed_put(GTK_FIXED(app->form_layer), control->widget, box->x, box->y);
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
    return control->box.value;
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
        if (control->box.form_index != form_index) continue;
        if (strcasecmp(control->box.type, "submit") == 0 || strcasecmp(control->box.tag, "button") == 0) {
            if (control != submit_control) continue;
        }
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
    int page_height = PAGE_HEIGHT;

    {
        const char *font_dir = *DISTILL_FONT_DIR ? DISTILL_FONT_DIR : getenv("DISTILL_FONT_DIR");
        int viewport = PAGE_HEIGHT;
        if (app->vertical_adjustment) {
            double size = gtk_adjustment_get_page_size(app->vertical_adjustment);
            if (size > 1.0) viewport = (int)size;
        }
        int band_height = viewport + RENDER_OVERSCAN * 2;
        if (band_height < PAGE_HEIGHT) band_height = PAGE_HEIGHT;
        app->document = distill_litehtml_document_create(template, PAGE_WIDTH,
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
    gtk_widget_set_size_request(app->drawing_area, PAGE_WIDTH, page_height);
    gtk_widget_set_size_request(app->event_box, PAGE_WIDTH, page_height);
    gtk_widget_set_size_request(app->form_layer, PAGE_WIDTH, page_height);
    if (app->vertical_adjustment) gtk_adjustment_set_value(app->vertical_adjustment, 0.0);
    render_visible_band(app);
}

/* The images the reader may open, taken from the links the template put on
   the placeholders. Reading them from the laid-out document rather than from
   a second file keeps one description of the page: what is clickable is
   exactly what is in this list. */
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
    char *template = d_read_file(page->template_path, NULL);
    build_document(app, template ? template : "");
    free(template);
    /* Navigating away is another way out of viewer mode, and the downloaded
       file goes with it rather than waiting for the session to end. */
    app->viewer_active = false;
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); app->viewer_file = NULL; }
    collect_page_media(app);
    gtk_entry_set_text(GTK_ENTRY(app->url_entry), page->url);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->theme_combo), page->theme ? page->theme : "warm");
    gtk_widget_set_sensitive(app->back_button, d_app_history_can_back(&app->history));
    gtk_widget_set_sensitive(app->forward_button, d_app_history_can_forward(&app->history));
    update_list_button(app);
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
        set_status(app, error->message);
        g_error_free(error);
        app->loading = false;
        gtk_widget_set_sensitive(app->go_button, TRUE);
        app_unref(app);
        return;
    }
    if (!loaded || loaded->serial != app->load_serial) {
        if (loaded) free_load_result(loaded);
        app_unref(app);
        return;
    }
    app->loading = false;
    if (!loaded->ok) {
        char *message = g_strdup_printf("Load failed: %s", loaded->error ? loaded->error : "engine failed");
        set_status(app, message);
        g_free(message);
        free_load_result(loaded);
        gtk_widget_set_sensitive(app->go_button, TRUE);
        app_unref(app);
        return;
    }
    bool history_ok;
    if (loaded->replace_current && d_app_history_current(&app->history)) {
        history_ok = d_app_history_replace_current(&app->history, loaded->url,
                                                    loaded->template_path,
                                                    loaded->theme, page_dispose);
    } else {
        history_ok = d_app_history_push(&app->history, loaded->url,
                                        loaded->template_path,
                                        loaded->theme, page_dispose);
    }
    if (!history_ok) {
        free_load_result(loaded);
        set_status(app, "Unable to update navigation history");
        gtk_widget_set_sensitive(app->go_button, TRUE);
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
    gtk_widget_set_sensitive(app->go_button, TRUE);
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
    const char *theme_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->theme_combo));
    request->theme = g_strdup(theme_id && *theme_id ? theme_id : "warm");
    request->serial = ++app->load_serial;
    request->replace_current = replace_current;
    app->loading = true;
    app_ref(app);
    gtk_widget_set_sensitive(app->go_button, FALSE);
    set_status(app, "Loading…");
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

static void go_clicked(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    url_activate(opaque ? GTK_WIDGET(((App *)opaque)->url_entry) : NULL, opaque);
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

static void update_list_tooltip(App *app)
{
    GString *text = g_string_new("Reading list");
    for (guint i = 0; i < app->reading_list->len; ++i) {
        g_string_append(text, "\n");
        g_string_append(text, g_ptr_array_index(app->reading_list, i));
    }
    gtk_widget_set_tooltip_text(app->list_button, text->str);
    g_string_free(text, TRUE);
}

static void list_clicked(GtkWidget *widget, gpointer opaque)
{
    App *app = opaque;
    const DAppPage *page = d_app_history_current(&app->history);
    if (!page) return;
    for (guint i = 0; i < app->reading_list->len; ++i) {
        const char *saved = g_ptr_array_index(app->reading_list, i);
        if (strcmp(saved, page->url) == 0) {
            g_ptr_array_remove_index(app->reading_list, i);
            gtk_button_set_label(GTK_BUTTON(widget), "☆");
            set_status(app, "Removed from reading list");
            update_list_tooltip(app);
            return;
        }
    }
    g_ptr_array_add(app->reading_list, g_strdup(page->url));
    gtk_button_set_label(GTK_BUTTON(widget), "★");
    set_status(app, "Added to reading list");
    update_list_tooltip(app);
}

static void theme_changed(GtkComboBox *combo, gpointer opaque)
{
    App *app = opaque;
    if (app->loading || !d_app_history_current(&app->history)) return;
    const DAppPage *page = d_app_history_current(&app->history);
    const char *theme = gtk_combo_box_get_active_id(combo);
    if (!theme || strcmp(theme, page->theme) == 0) return;
    app_begin_load(app, page->url, NULL, NULL, true);
}

static GtkWidget *make_button(const char *label, GCallback callback, App *app)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_signal_connect(button, "clicked", callback, app);
    return button;
}

static void app_destroy(GtkWidget *widget, gpointer opaque)
{
    (void)widget;
    App *app = opaque;
    app->closing = true;
    if (app->document) distill_litehtml_document_destroy(app->document);
    app->document = NULL;
    app->surface = NULL;
    clear_controls(app);
    d_app_history_clear(&app->history, page_dispose);
    g_ptr_array_free(app->reading_list, TRUE);
    if (app->page_media) g_ptr_array_free(app->page_media, TRUE);
    if (app->viewer_file) { unlink(app->viewer_file); g_free(app->viewer_file); }
    g_application_quit(app->application);
    app_unref(app);
}

static void app_activate(GtkApplication *application, gpointer opaque)
{
    const char *initial_url = opaque;
    App *app = g_new0(App, 1);
    app->application = G_APPLICATION(application);
    app->references = 1;
    d_app_history_init(&app->history);
    app->reading_list = g_ptr_array_new_with_free_func(g_free);

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

    app->back_button = make_button("‹", G_CALLBACK(back_clicked), app);
    app->forward_button = make_button("›", G_CALLBACK(forward_clicked), app);
    gtk_widget_set_sensitive(app->back_button, FALSE);
    gtk_widget_set_sensitive(app->forward_button, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), app->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->forward_button, FALSE, FALSE, 0);
    app->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->url_entry), "Enter an http(s) URL or local HTML file");
    gtk_widget_set_hexpand(app->url_entry, TRUE);
    g_signal_connect(app->url_entry, "activate", G_CALLBACK(url_activate), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->url_entry, TRUE, TRUE, 0);
    app->go_button = make_button("Go", G_CALLBACK(go_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->go_button, FALSE, FALSE, 0);
    app->external_button = make_button("Open externally", G_CALLBACK(external_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->external_button, FALSE, FALSE, 0);
    app->list_button = make_button("☆", G_CALLBACK(list_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->list_button, FALSE, FALSE, 0);
    app->theme_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->theme_combo), "warm", "Warm light");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->theme_combo), "night", "Night");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->theme_combo), "paper", "Paper");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->theme_combo), "warm");
    g_signal_connect(app->theme_combo, "changed", G_CALLBACK(theme_changed), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->theme_combo, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    app->vertical_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    g_signal_connect(app->vertical_adjustment, "value-changed",
                     G_CALLBACK(scroll_value_changed), app);
    gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_add_events(overlay, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
                          GDK_LEAVE_NOTIFY_MASK);
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
    gtk_widget_set_size_request(app->event_box, PAGE_WIDTH, PAGE_HEIGHT);
    gtk_widget_add_events(app->event_box, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
                          GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(app->event_box, "button-press-event", G_CALLBACK(page_button_press), app);
    g_signal_connect(app->event_box, "motion-notify-event", G_CALLBACK(page_motion_notify), app);
    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->drawing_area, PAGE_WIDTH, PAGE_HEIGHT);
    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(draw_page), app);
    gtk_container_add(GTK_CONTAINER(app->event_box), app->drawing_area);
    gtk_container_add(GTK_CONTAINER(overlay), app->event_box);
    app->form_layer = gtk_fixed_new();
    gtk_widget_set_size_request(app->form_layer, PAGE_WIDTH, PAGE_HEIGHT);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->form_layer);

    app->status = gtk_label_new("Ready");
    gtk_widget_set_halign(app->status, GTK_ALIGN_START);
    gtk_widget_set_margin_start(app->status, 10);
    gtk_widget_set_margin_top(app->status, 5);
    gtk_widget_set_margin_bottom(app->status, 5);
    gtk_box_pack_start(GTK_BOX(root), app->status, FALSE, FALSE, 0);
    update_list_tooltip(app);
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
            int height = distill_litehtml_content_height(html, PAGE_WIDTH, font_dir);
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

static int run_state_self_test(void)
{
    DAppHistory history;
    d_app_history_init(&history);
    bool ok = d_app_history_push(&history, "one", "one", "warm", NULL) &&
              d_app_history_push(&history, "two", "two", "night", NULL) &&
              d_app_history_back(&history) &&
              d_app_history_current(&history) &&
              strcmp(d_app_history_current(&history)->url, "one") == 0 &&
              d_app_history_forward(&history);
    d_app_history_clear(&history, NULL);
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
