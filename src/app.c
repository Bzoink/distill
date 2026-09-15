#define _POSIX_C_SOURCE 200809L

#include "app_state.h"
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
    MAX_FORM_BOXES = 512,
    MAX_LINK_BOXES = 2048
};

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
    char *png_path;
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
    GtkWidget *form_layer;
    cairo_surface_t *surface;
    DFormBox *form_boxes;
    size_t form_count;
    FormControl *controls;
    size_t control_count;
    DLinkBox *link_boxes;
    size_t link_count;
    DAppHistory history;
    GPtrArray *reading_list;
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
    if (page->png_path && *page->png_path) unlink(page->png_path);
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
    if (result->png_path) unlink(result->png_path);
    if (result->template_path) unlink(result->template_path);
    g_free(result->url);
    g_free(result->png_path);
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

static bool run_engine(const LoadRequest *request, const char *png_path,
                       const char *template_path, char **error_message)
{
    char *engine = engine_path();
    if (!engine) return false;
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    add_arg(arguments, engine);
    add_arg(arguments, "--url");
    add_arg(arguments, request->url);
    add_arg(arguments, "--render");
    add_arg(arguments, png_path);
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
    result->png_path = temporary_path("distill-page-XXXXXX");
    result->template_path = temporary_path("distill-template-XXXXXX");
    if (!result->png_path || !result->template_path) {
        result->error = g_strdup("unable to create temporary page files");
    } else {
        result->ok = run_engine(request, result->png_path, result->template_path,
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
        cairo_set_source_surface(cairo, app->surface, 0, 0);
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

static bool point_in_box(double x, double y, int box_x, int box_y, int width, int height)
{
    return x >= box_x && y >= box_y && x <= box_x + width && y <= box_y + height;
}

static void app_navigate(App *app, const char *uri, const char *method,
                         const char *data, bool replace_current);

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
    (void)widget;
    App *app = opaque;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;
    for (size_t i = 0; i < app->link_count; ++i) {
        const DLinkBox *link = &app->link_boxes[i];
        if (point_in_box(event->x, event->y, link->x, link->y, link->width, link->height)) {
            open_link(app, link->href);
            return TRUE;
        }
    }
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

static void load_surface(App *app, const DAppPage *page)
{
    if (app->surface) cairo_surface_destroy(app->surface);
    app->surface = cairo_image_surface_create_from_png(page->png_path);
    clear_controls(app);
    /* The rendered surface is the authority on how long the page is. The
       scroll area and the widget geometry must agree with it, or the window
       scrolls to a bottom that is not the document's. */
    int page_height = PAGE_HEIGHT;
    if (app->surface &&
        cairo_surface_status(app->surface) == CAIRO_STATUS_SUCCESS) {
        int measured = cairo_image_surface_get_height(app->surface);
        if (measured > 0) page_height = measured;
    }
    gtk_widget_set_size_request(app->drawing_area, PAGE_WIDTH, page_height);
    gtk_widget_set_size_request(app->form_layer, PAGE_WIDTH, page_height);

    char *template = d_read_file(page->template_path, NULL);
    if (template) {
        const char *font_dir = *DISTILL_FONT_DIR ? DISTILL_FONT_DIR : getenv("DISTILL_FONT_DIR");
        size_t form_count = distill_litehtml_form_boxes(template, PAGE_WIDTH, page_height,
                                                        font_dir, NULL, 0);
        size_t link_count = distill_litehtml_link_boxes(template, PAGE_WIDTH, page_height,
                                                        font_dir, NULL, 0);
        if (form_count > MAX_FORM_BOXES) form_count = MAX_FORM_BOXES;
        if (link_count > MAX_LINK_BOXES) link_count = MAX_LINK_BOXES;
        app->form_boxes = form_count ? g_new0(DFormBox, form_count) : NULL;
        app->link_boxes = link_count ? g_new0(DLinkBox, link_count) : NULL;
        app->form_count = form_count;
        app->link_count = link_count;
        if (form_count)
            distill_litehtml_form_boxes(template, PAGE_WIDTH, page_height, font_dir,
                                        app->form_boxes, form_count);
        if (link_count)
            distill_litehtml_link_boxes(template, PAGE_WIDTH, page_height, font_dir,
                                        app->link_boxes, link_count);
        if (form_count) {
            app->controls = g_new0(FormControl, form_count);
            app->control_count = form_count;
            for (size_t i = 0; i < form_count; ++i) app->controls[i].box = app->form_boxes[i];
            add_native_controls(app);
        }
        free(template);
    }
    gtk_entry_set_text(GTK_ENTRY(app->url_entry), page->url);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->theme_combo), page->theme ? page->theme : "warm");
    gtk_widget_set_sensitive(app->back_button, d_app_history_can_back(&app->history));
    gtk_widget_set_sensitive(app->forward_button, d_app_history_can_forward(&app->history));
    update_list_button(app);
    gtk_widget_queue_draw(app->drawing_area);
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
                                                    loaded->png_path, loaded->template_path,
                                                    loaded->theme, page_dispose);
    } else {
        history_ok = d_app_history_push(&app->history, loaded->url, loaded->png_path,
                                        loaded->template_path, loaded->theme, page_dispose);
    }
    if (!history_ok) {
        free_load_result(loaded);
        set_status(app, "Unable to update navigation history");
        gtk_widget_set_sensitive(app->go_button, TRUE);
        app_unref(app);
        return;
    }
    /* The history owns copies of these names now. */
    g_free(loaded->png_path);
    g_free(loaded->template_path);
    loaded->png_path = NULL;
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
    if (app->surface) cairo_surface_destroy(app->surface);
    clear_controls(app);
    d_app_history_clear(&app->history, page_dispose);
    g_ptr_array_free(app->reading_list, TRUE);
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
    gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(scrolled), overlay);
    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->drawing_area, PAGE_WIDTH, PAGE_HEIGHT);
    gtk_widget_add_events(app->drawing_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(draw_page), app);
    g_signal_connect(app->drawing_area, "button-press-event", G_CALLBACK(page_button_press), app);
    gtk_container_add(GTK_CONTAINER(overlay), app->drawing_area);
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

static int run_state_self_test(void)
{
    DAppHistory history;
    d_app_history_init(&history);
    bool ok = d_app_history_push(&history, "one", "one", "one", "warm", NULL) &&
              d_app_history_push(&history, "two", "two", "two", "night", NULL) &&
              d_app_history_back(&history) &&
              d_app_history_current(&history) &&
              strcmp(d_app_history_current(&history)->url, "one") == 0 &&
              d_app_history_forward(&history);
    d_app_history_clear(&history, NULL);
    ok = run_address_bar_self_test() && ok;
    ok = run_redirect_self_test() && ok;
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
