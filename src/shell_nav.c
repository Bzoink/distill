/* One engine run per navigation; see shell_nav.h. */

#include "shell_nav.h"

#include "dfs.h"
#include "engine_request.h"
#include "form_submit.h"

#include <stdio.h>

static char *write_private_body(const char *data)
{
    char *path = d_fs_private_file(NULL, "distill-body");
    if (!path) return NULL;
    if (!d_fs_overwrite(path, data, strlen(data))) { d_fs_remove(path); free(path); return NULL; }
    return path;
}

bool d_nav_start(DShellNav *nav, const DNavOptions *options, const char *url,
                 const char *method, const char *body, bool https_guessed, DNavKind kind,
                 char **error)
{
    *error = NULL;
    if (nav->active) d_nav_cancel(nav);
    char *template_path = d_fs_private_file(NULL, "distill-template");
    if (!template_path) { *error = d_strdup("Unable to create a temporary file"); return false; }
    char *body_path = NULL;
    if (body && *body) {
        body_path = write_private_body(body);
        if (!body_path) {
            d_fs_remove(template_path);
            free(template_path);
            *error = d_strdup("Unable to stage the request body");
            return false;
        }
    }
    char number[16];
    DRequestFile request;
    d_request_file_init(&request);
    d_request_file_add(&request, "url", url);
    d_request_file_add(&request, "template", template_path);
    d_request_file_add(&request, "theme", options->theme);
    snprintf(number, sizeof(number), "%d", options->zoom);
    d_request_file_add(&request, "zoom", number);
    if (options->width > 0) {
        snprintf(number, sizeof(number), "%d", options->width);
        d_request_file_add(&request, "width", number);
    }
    if (options->native_forms) d_request_file_add(&request, "native-forms", NULL);
    d_request_file_add(&request, "progress", NULL);
    if (https_guessed) d_request_file_add(&request, "https-optional", NULL);
    if (method && *method) d_request_file_add(&request, "request-method", method);
    if (body_path) d_request_file_add(&request, "request-data-file", body_path);
    if (options->session_file) d_request_file_add(&request, "session-file", options->session_file);
    d_request_file_add(&request, "script-cap-ms", "5000");
    char *request_path = d_request_file_write(&request);
    const char *argv[] = {options->engine, "--request", request_path, NULL};
    char *spawn_error = NULL;
    DProc *engine = request_path ? d_proc_spawn(argv, D_PROC_PIPE, D_PROC_PIPE, &spawn_error) : NULL;
    if (!engine) {
        char message[512];
        snprintf(message, sizeof(message), "Unable to start the engine %s",
                 spawn_error ? spawn_error : request_path ? options->engine : "(no request file)");
        *error = d_strdup(message);
        free(spawn_error);
        d_fs_remove(template_path); free(template_path);
        if (body_path) { d_fs_remove(body_path); free(body_path); }
        if (request_path) { d_fs_remove(request_path); free(request_path); }
        return false;
    }
    *nav = (DShellNav){.active = true, .engine = engine, .url = d_strdup(url),
                       .template_path = template_path, .body_path = body_path,
                       .request_path = request_path, .kind = kind};
    dbuf_init(&nav->pending);
    dbuf_init(&nav->diagnostics);
    snprintf(nav->phase, sizeof(nav->phase), "fetch");
    return true;
}

size_t d_nav_handles(const DShellNav *nav, int *handles, size_t capacity)
{
    size_t count = 0;
    if (!nav->active) return 0;
    int out = d_proc_output(nav->engine), err = d_proc_errors(nav->engine);
    if (out >= 0 && count < capacity) handles[count++] = out;
    if (err >= 0 && count < capacity) handles[count++] = err;
    return count;
}

/* One record per line: "progress PERCENT PHASE" while the page loads,
   "location URL" once the engine knows where the page really came from. */
static void consume_progress(DShellNav *nav)
{
    char *start = nav->pending.data;
    if (!start) return;
    char *newline;
    while ((newline = memchr(start, '\n', (size_t)(nav->pending.data + nav->pending.length - start)))) {
        *newline = '\0';
        int percent = 0;
        char phase[16] = "";
        if (sscanf(start, "progress %d %15s", &percent, phase) == 2) {
            nav->percent = percent;
            snprintf(nav->phase, sizeof(nav->phase), "%s", phase);
        } else if (!strncmp(start, "location ", 9) && start[9]) {
            free(nav->location);
            nav->location = d_strdup(start + 9);
        }
        start = newline + 1;
    }
    size_t rest = (size_t)(nav->pending.data + nav->pending.length - start);
    memmove(nav->pending.data, start, rest);
    nav->pending.length = rest;
}

/* Read what one of the engine's streams has said. False once it has ended,
   and the stream is then closed. */
static bool drain(DShellNav *nav, bool errors, DBuffer *into)
{
    int handle = errors ? d_proc_errors(nav->engine) : d_proc_output(nav->engine);
    char buffer[4096];
    for (;;) {
        long got = d_handle_read(handle, buffer, sizeof(buffer));
        if (got > 0) {
            if (into->length < 256 * 1024) dbuf_append(into, buffer, (size_t)got);
            continue;
        }
        if (got == D_READ_EMPTY) return true;
        if (errors) d_proc_close_errors(nav->engine);
        else d_proc_close_output(nav->engine);
        return false;
    }
}

DNavOutcome d_nav_service(DShellNav *nav)
{
    if (!nav->active) return D_NAV_FAILED;
    if (d_proc_output(nav->engine) >= 0) {
        drain(nav, false, &nav->pending);
        consume_progress(nav);
    }
    if (d_proc_errors(nav->engine) >= 0) drain(nav, true, &nav->diagnostics);
    if (d_proc_output(nav->engine) >= 0 || d_proc_errors(nav->engine) >= 0) return D_NAV_RUNNING;
    DProcStatus status = d_proc_wait(nav->engine);
    int code = status.exited ? status.code : -1;
    if (code == 0) return D_NAV_LOADED;
    if (code == 4) return D_NAV_CHALLENGE;
    if (nav->template_path) d_fs_remove(nav->template_path);
    return D_NAV_FAILED;
}

const char *d_nav_final_url(const DShellNav *nav)
{
    return nav->location && d_page_uri_allowed(nav->location) ? nav->location : nav->url;
}

char *d_nav_take_template(DShellNav *nav)
{
    char *path = nav->template_path;
    nav->template_path = NULL;
    return path;
}

char *d_nav_failure(const DShellNav *nav)
{
    const DBuffer *text = &nav->diagnostics;
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

void d_nav_cleanup(DShellNav *nav)
{
    d_proc_free(nav->engine);
    if (nav->body_path) { d_fs_remove(nav->body_path); free(nav->body_path); }
    if (nav->request_path) { d_fs_remove(nav->request_path); free(nav->request_path); }
    dbuf_free(&nav->pending);
    dbuf_free(&nav->diagnostics);
    free(nav->url);
    free(nav->template_path);
    free(nav->location);
    *nav = (DShellNav){0};
}

void d_nav_cancel(DShellNav *nav)
{
    if (!nav->active) return;
    d_proc_terminate(nav->engine);
    if (nav->template_path) d_fs_remove(nav->template_path);
    d_nav_cleanup(nav);
}
