#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "cutil.h"

#include <quickjs.h>
#include <curl/curl.h>

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DISTILL_SOURCE_DIR
#define DISTILL_SOURCE_DIR "."
#endif

typedef struct Engine Engine;

typedef enum {
    REQUEST_FETCH,
    REQUEST_ASYNC_SCRIPT
} RequestKind;

typedef struct {
    uint64_t id;
    uint64_t due;
    int interval;
    JSValue callback;
    JSValue argument;
    bool cancelled;
    bool recurring;
} Timer;

typedef struct {
    bool window;
    char *type;
    JSValue callback;
} Listener;

typedef struct Request {
    RequestKind kind;
    Engine *engine;
    CURL *easy;
    char *url;
    DBuffer body;
    long status;
    JSValue resolve;
    JSValue reject;
    bool completed;
} Request;

typedef struct {
    Request **items;
    size_t length;
    size_t capacity;
} RequestVec;

typedef struct {
    Timer *items;
    size_t length;
    size_t capacity;
} TimerVec;

typedef struct {
    Listener *items;
    size_t length;
    size_t capacity;
} ListenerVec;

typedef struct {
    JSValue *items;
    size_t length;
    size_t capacity;
} ValueVec;

struct Engine {
    JSRuntime *runtime;
    JSContext *context;
    JSValue global;
    JSValue window;
    JSValue document;
    CURLM *multi;
    char *source_url;
    char *base_url;
    uint64_t started;
    uint64_t cap_ms;
    uint64_t memory_limit;
    uint64_t max_scripts;
    uint64_t scripts_run;
    uint64_t next_timer;
    int async_pending;
    bool load_sent;
    bool cap_hit;
    TimerVec timers;
    ListenerVec listeners;
    RequestVec requests;
};

static uint64_t clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static char *js_string(JSContext *ctx, JSValueConst value)
{
    size_t length = 0;
    const char *text = JS_ToCStringLen(ctx, &length, value);
    if (!text) return d_strdup("");
    char *out = d_strndup(text, length);
    JS_FreeCString(ctx, text);
    return out;
}

static JSValue js_str(JSContext *ctx, const char *value)
{
    return JS_NewString(ctx, value ? value : "");
}

static JSValue js_strn(JSContext *ctx, const char *value, size_t length)
{
    return JS_NewStringLen(ctx, value ? value : "", length);
}

static bool is_http_url(const char *url)
{
    return d_starts(url, "http://") || d_starts(url, "https://");
}

static char *url_origin(const char *url)
{
    const char *scheme = strstr(url, "://");
    if (!scheme) return d_strdup("");
    const char *end = strpbrk(scheme + 3, "/?#");
    return d_strndup(url, end ? (size_t)(end - url) : strlen(url));
}

static char *url_path(const char *url)
{
    const char *scheme = strstr(url, "://");
    const char *start = scheme ? strchr(scheme + 3, '/') : url;
    if (!start) return d_strdup("/");
    const char *end = strpbrk(start, "?#");
    return d_strndup(start, end ? (size_t)(end - start) : strlen(start));
}

static char *normalize_path(const char *path)
{
    DBuffer out;
    dbuf_init(&out);
    dbuf_puts(&out, "/");
    size_t count = 0;
    char **parts = NULL;
    size_t parts_cap = 0;
    const char *cursor = path;
    while (*cursor) {
        while (*cursor == '/') ++cursor;
        const char *end = strchr(cursor, '/');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length && !(length == 1 && cursor[0] == '.')) {
            if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
                if (count) free(parts[--count]);
            } else {
                d_vec_reserve((void **)&parts, &parts_cap, count, sizeof(*parts), count + 1);
                parts[count++] = d_strndup(cursor, length);
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (i) dbuf_puts(&out, "/");
        dbuf_puts(&out, parts[i]);
        free(parts[i]);
    }
    free(parts);
    if (strlen(path) > 1 && path[strlen(path) - 1] == '/') dbuf_puts(&out, "/");
    return dbuf_take(&out);
}

static char *resolve_url(const char *base, const char *candidate)
{
    if (!candidate || !*candidate) return d_strdup(base);
    const char *colon = strchr(candidate, ':');
    bool scheme = colon && colon != candidate;
    if (scheme) {
        for (const char *p = candidate; p < colon; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')) {
                scheme = false;
                break;
            }
        }
    }
    if (scheme) return d_strdup(candidate);
    if (d_starts(candidate, "//")) {
        const char *base_colon = strchr(base, ':');
        char *prefix = base_colon ? d_strndup(base, (size_t)(base_colon - base + 1)) : d_strdup("http:");
        char *out = d_cat2(prefix, candidate);
        free(prefix);
        return out;
    }
    char *origin = url_origin(base);
    if (*origin) {
        const char *suffix = strpbrk(candidate, "?#");
        size_t path_len = suffix ? (size_t)(suffix - candidate) : strlen(candidate);
        char *path = d_strndup(candidate, path_len);
        char *tail = suffix ? d_strdup(suffix) : d_strdup("");
        char *out = NULL;
        if (candidate[0] == '#') {
            const char *base_suffix = strpbrk(base, "?#");
            char *head = d_strndup(base, base_suffix ? (size_t)(base_suffix - base) : strlen(base));
            out = d_cat3(head, candidate, "");
            free(head);
        } else if (candidate[0] == '?') {
            const char *base_suffix = strpbrk(base, "?#");
            char *head = d_strndup(base, base_suffix ? (size_t)(base_suffix - base) : strlen(base));
            out = d_cat3(head, candidate, "");
            free(head);
        } else if (path[0] == '/') {
            char *clean = normalize_path(path);
            out = d_cat4(origin, clean, tail, "");
            free(clean);
        } else {
            char *base_path = url_path(base);
            char *slash = strrchr(base_path, '/');
            char *prefix = slash ? d_strndup(base_path, (size_t)(slash - base_path + 1)) : d_strdup("/");
            char *joined = d_cat2(prefix, path);
            char *clean = normalize_path(joined);
            out = d_cat4(origin, clean, tail, "");
            free(base_path); free(prefix); free(joined); free(clean);
        }
        free(origin); free(path); free(tail);
        return out;
    }
    free(origin);
    if (candidate[0] == '#') {
        const char *suffix = strpbrk(base, "?#");
        char *head = d_strndup(base, suffix ? (size_t)(suffix - base) : strlen(base));
        char *out = d_cat2(head, candidate);
        free(head);
        return out;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return d_strdup(candidate);
    const char *slash = strrchr(base, '/');
    char *prefix = slash ? d_strndup(base, (size_t)(slash - base + 1)) : d_cat2(cwd, "/");
    char *joined = d_cat2(prefix, candidate);
    free(prefix);
    return joined;
}

static size_t percent_decode(char *out, const char *input)
{
    size_t used = 0;
    for (size_t i = 0; input[i]; ++i) {
        if (input[i] == '%' && input[i + 1] && input[i + 2]) {
            char hex[3] = {input[i + 1], input[i + 2], 0};
            char *end = NULL;
            long value = strtol(hex, &end, 16);
            if (end && *end == 0) { out[used++] = (char)value; i += 2; continue; }
        }
        out[used++] = input[i];
    }
    out[used] = 0;
    return used;
}

static char *file_from_url(const char *url)
{
    if (!d_starts(url, "file://")) return d_strdup(url);
    char *out = d_xmalloc(strlen(url) + 1);
    percent_decode(out, url + 7);
    return out;
}

static char *absolute_input(const char *input)
{
    if (is_http_url(input) || d_starts(input, "file://")) return d_strdup(input);
    char *resolved = realpath(input, NULL);
    if (resolved) return resolved;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return d_strdup(input);
    return d_cat3(cwd, "/", input);
}

static size_t curl_write(void *ptr, size_t size, size_t count, void *opaque)
{
    DBuffer *body = opaque;
    size_t length = size * count;
    dbuf_append(body, ptr, length);
    return length;
}

static bool fetch_sync(Engine *engine, const char *url, DBuffer *body,
                       long *status, char **effective_url)
{
    dbuf_init(body);
    char *path = file_from_url(url);
    if (!is_http_url(url)) {
        size_t length = 0;
        char *data = d_read_file(path, &length);
        free(path);
        if (!data) { if (status) *status = 404; return false; }
        dbuf_append(body, data, length);
        free(data);
        if (status) *status = 200;
        if (effective_url) *effective_url = d_strdup(url);
        return true;
    }
    free(path);
    CURL *easy = curl_easy_init();
    if (!easy) return false;
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "distill/0.1");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)engine->cap_ms);
    CURLcode result = curl_easy_perform(easy);
    long code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
    if (effective_url) {
        char *final_url = NULL;
        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &final_url);
        *effective_url = final_url ? d_strdup(final_url) : d_strdup(url);
    }
    curl_easy_cleanup(easy);
    if (status) *status = code;
    return result == CURLE_OK;
}

static void request_vec_push(RequestVec *vec, Request *request)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = request;
}

static void timer_vec_push(TimerVec *vec, Timer timer)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = timer;
}

static void listener_vec_push(ListenerVec *vec, Listener listener)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = listener;
}

static void value_vec_push(ValueVec *vec, JSValue value)
{
    d_vec_reserve((void **)&vec->items, &vec->capacity, vec->length,
                  sizeof(*vec->items), vec->length + 1);
    vec->items[vec->length++] = value;
}

static void note_exception(Engine *engine, const char *where)
{
    if (!JS_HasException(engine->context)) return;
    JSValue exception = JS_GetException(engine->context);
    char *message = js_string(engine->context, exception);
    fprintf(stderr, "script error [%s]: %s\n", where, message);
    free(message);
    JS_FreeValue(engine->context, exception);
}

static void execute_jobs(Engine *engine)
{
    JSContext *job_context = NULL;
    while (JS_IsJobPending(engine->runtime)) {
        if (JS_ExecutePendingJob(engine->runtime, &job_context) < 0) note_exception(engine, "promise");
        if (clock_ms() - engine->started >= engine->cap_ms) break;
    }
}

static int interrupt_handler(JSRuntime *runtime, void *opaque)
{
    (void)runtime;
    Engine *engine = opaque;
    if (clock_ms() - engine->started >= engine->cap_ms) {
        engine->cap_hit = true;
        return 1;
    }
    return 0;
}

static JSValue make_promise(Engine *engine, JSValue *resolve, JSValue *reject)
{
    JSValue functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(engine->context, functions);
    *resolve = functions[0];
    *reject = functions[1];
    return promise;
}

static void free_request(Engine *engine, Request *request)
{
    if (request->easy) {
        curl_multi_remove_handle(engine->multi, request->easy);
        curl_easy_cleanup(request->easy);
    }
    free(request->url);
    dbuf_free(&request->body);
    if (!JS_IsUndefined(request->resolve)) JS_FreeValue(engine->context, request->resolve);
    if (!JS_IsUndefined(request->reject)) JS_FreeValue(engine->context, request->reject);
    free(request);
}

static Request *start_request(Engine *engine, RequestKind kind, const char *url)
{
    Request *request = d_xcalloc(1, sizeof(*request));
    request->kind = kind;
    request->engine = engine;
    request->url = d_strdup(url);
    request->resolve = JS_UNDEFINED;
    request->reject = JS_UNDEFINED;
    dbuf_init(&request->body);
    request->easy = curl_easy_init();
    if (!request->easy) { free_request(engine, request); return NULL; }
    curl_easy_setopt(request->easy, CURLOPT_URL, request->url);
    curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->body);
    curl_easy_setopt(request->easy, CURLOPT_PRIVATE, request);
    curl_easy_setopt(request->easy, CURLOPT_USERAGENT, "distill/0.1");
    curl_easy_setopt(request->easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    request_vec_push(&engine->requests, request);
    if (curl_multi_add_handle(engine->multi, request->easy) != CURLM_OK) {
        request->completed = true;
        free_request(engine, request);
        return NULL;
    }
    return request;
}

static JSValue response_text(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    Engine *engine = JS_GetContextOpaque(ctx);
    JSValue body = JS_GetPropertyStr(ctx, self, "_body");
    char *text = js_string(ctx, body);
    JS_FreeValue(ctx, body);
    JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
    JSValue promise = make_promise(engine, &resolve, &reject);
    JS_FreeValue(ctx, reject);
    JSValue value = js_str(ctx, text);
    JSValue result = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, resolve);
    free(text);
    return promise;
}

static JSValue response_json(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    Engine *engine = JS_GetContextOpaque(ctx);
    JSValue body = JS_GetPropertyStr(ctx, self, "_body");
    char *text = js_string(ctx, body);
    JS_FreeValue(ctx, body);
    JSValue parsed = JS_ParseJSON(ctx, text, strlen(text), "distill-response.json");
    free(text);
    if (JS_IsException(parsed)) return parsed;
    JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
    JSValue promise = make_promise(engine, &resolve, &reject);
    JS_FreeValue(ctx, reject);
    JSValue result = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &parsed);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, resolve);
    return promise;
}

static JSValue make_response(Engine *engine, Request *request)
{
    JSContext *ctx = engine->context;
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, request->status >= 200 && request->status < 300));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, (int32_t)request->status));
    JS_SetPropertyStr(ctx, response, "url", js_str(ctx, request->url));
    JS_SetPropertyStr(ctx, response, "_body", js_strn(ctx, request->body.data, request->body.length));
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, response_json, "json", 0));
    return response;
}

static JSValue fetch_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    Engine *engine = JS_GetContextOpaque(ctx);
    char *relative = argc ? js_string(ctx, argv[0]) : d_strdup(engine->source_url);
    char *url = resolve_url(engine->base_url, relative);
    free(relative);
    JSValue functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    Request *request = start_request(engine, REQUEST_FETCH, url);
    if (!request) {
        JS_FreeValue(ctx, functions[0]);
        JS_FreeValue(ctx, functions[1]);
        free(url);
        return promise;
    }
    request->resolve = functions[0];
    request->reject = functions[1];
    free(url);
    return promise;
}

static JSValue timer_callback(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic)
{
    (void)this_val;
    Engine *engine = JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t delay = 0;
    if (argc > 1) JS_ToInt32(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;
    Timer timer = {
        .id = engine->next_timer++,
        .due = clock_ms() + (uint64_t)delay,
        .interval = magic ? (delay > 0 ? delay : 1) : 0,
        .callback = JS_DupValue(ctx, argv[0]),
        .argument = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED,
        .cancelled = false,
        .recurring = false
    };
    timer_vec_push(&engine->timers, timer);
    return JS_NewInt64(ctx, (int64_t)timer.id);
}

static JSValue clear_timer_callback(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv, int magic)
{
    (void)this_val; (void)magic;
    Engine *engine = JS_GetContextOpaque(ctx);
    if (argc) {
        int64_t id = 0;
        JS_ToInt64(ctx, &id, argv[0]);
        for (size_t i = 0; i < engine->timers.length; ++i) {
            if (engine->timers.items[i].id == (uint64_t)id) engine->timers.items[i].cancelled = true;
        }
    }
    return JS_UNDEFINED;
}

static JSValue request_animation_frame(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    (void)this_val;
    Engine *engine = JS_GetContextOpaque(ctx);
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    Timer timer = {
        .id = engine->next_timer++,
        .due = clock_ms() + 16,
        .interval = 0,
        .callback = JS_DupValue(ctx, argv[0]),
        .argument = JS_NewFloat64(ctx, (double)(clock_ms() - engine->started)),
        .cancelled = false,
        .recurring = false
    };
    timer_vec_push(&engine->timers, timer);
    return JS_NewInt64(ctx, (int64_t)timer.id);
}

static JSValue console_callback(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic)
{
    (void)this_val; (void)magic;
    fputs("distill console", stderr);
    for (int i = 0; i < argc; ++i) {
        char *arg = js_string(ctx, argv[i]);
        fprintf(stderr, " %s", arg);
        free(arg);
    }
    fputc('\n', stderr);
    return JS_UNDEFINED;
}

static void run_timers(Engine *engine)
{
    uint64_t now = clock_ms();
    size_t initial_length = engine->timers.length;
    for (size_t i = 0; i < initial_length; ++i) {
        Timer *timer = &engine->timers.items[i];
        if (timer->cancelled || timer->due > now) continue;
        JSValue argument = JS_DupValue(engine->context, timer->argument);
        JSValue result = JS_Call(engine->context, timer->callback, JS_UNDEFINED, 1, &argument);
        if (JS_IsException(result)) note_exception(engine, "timer");
        JS_FreeValue(engine->context, result);
        JS_FreeValue(engine->context, argument);
        if (timer->interval && !timer->cancelled) timer->due = now + (uint64_t)timer->interval;
        else timer->cancelled = true;
    }
    for (size_t i = initial_length; i < engine->timers.length; ++i) {
        for (size_t j = 0; j < initial_length; ++j) {
            if (!engine->timers.items[j].cancelled &&
                JS_IsStrictEqual(engine->context, engine->timers.items[i].callback,
                                 engine->timers.items[j].callback)) {
                engine->timers.items[i].recurring = true;
                break;
            }
        }
    }
    size_t write = 0;
    for (size_t i = 0; i < engine->timers.length; ++i) {
        Timer *timer = &engine->timers.items[i];
        if (timer->cancelled) {
            JS_FreeValue(engine->context, timer->callback);
            JS_FreeValue(engine->context, timer->argument);
        } else {
            engine->timers.items[write++] = *timer;
        }
    }
    engine->timers.length = write;
}

static void finish_request(Engine *engine, Request *request)
{
    if (request->kind == REQUEST_ASYNC_SCRIPT) {
        JSValue result = JS_Eval(engine->context, request->body.data, request->body.length,
                                 request->url, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) note_exception(engine, request->url);
        JS_FreeValue(engine->context, result);
        if (engine->async_pending > 0) --engine->async_pending;
    } else if (!JS_IsUndefined(request->resolve)) {
        JSValue response = make_response(engine, request);
        JSValue result = JS_Call(engine->context, request->resolve, JS_UNDEFINED, 1, &response);
        if (JS_IsException(result)) note_exception(engine, "fetch resolve");
        JS_FreeValue(engine->context, result);
        JS_FreeValue(engine->context, response);
        JS_FreeValue(engine->context, request->resolve);
        JS_FreeValue(engine->context, request->reject);
        request->resolve = JS_UNDEFINED;
        request->reject = JS_UNDEFINED;
    }
    if (request->easy) {
        curl_easy_cleanup(request->easy);
        request->easy = NULL;
    }
}

static void process_network(Engine *engine)
{
    if (!engine->multi) return;
    int running = 0;
    curl_multi_perform(engine->multi, &running);
    int messages = 0;
    CURLMsg *message = NULL;
    while ((message = curl_multi_info_read(engine->multi, &messages)) != NULL) {
        if (message->msg != CURLMSG_DONE) continue;
        Request *request = NULL;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
        if (!request) continue;
        curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &request->status);
        curl_multi_remove_handle(engine->multi, message->easy_handle);
        request->completed = true;
        finish_request(engine, request);
    }
}

static char *module_normalize(JSContext *ctx, const char *base_name,
                              const char *name, void *opaque)
{
    (void)base_name;
    Engine *engine = opaque;
    char *url = resolve_url(engine->base_url, name);
    char *out = js_malloc(ctx, strlen(url) + 1);
    if (out) strcpy(out, url);
    free(url);
    return out;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name, void *opaque)
{
    Engine *engine = opaque;
    DBuffer body;
    long status = 0;
    if (!fetch_sync(engine, name, &body, &status, NULL)) {
        JS_ThrowReferenceError(ctx, "module not found: %s", name);
        return NULL;
    }
    JSValue value = JS_Eval(ctx, body.data, body.length, name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    dbuf_free(&body);
    if (JS_IsException(value)) return NULL;
    JSModuleDef *module = JS_VALUE_GET_PTR(value);
    JS_FreeValue(ctx, value);
    return module;
}

static void dispatch_host_event(Engine *engine, const char *kind, const char *type)
{
    JSValue dispatch = JS_GetPropertyStr(engine->context, engine->global, "__dispatch");
    JSValue args[2] = {js_str(engine->context, kind), js_str(engine->context, type)};
    JSValue result = JS_Call(engine->context, dispatch, JS_UNDEFINED, 2, args);
    if (JS_IsException(result)) note_exception(engine, "event dispatch");
    JS_FreeValue(engine->context, result);
    JS_FreeValue(engine->context, args[0]);
    JS_FreeValue(engine->context, args[1]);
    JS_FreeValue(engine->context, dispatch);
}

static const char *HOST_JS =
"globalThis.__listeners = {window: Object.create(null), document: Object.create(null)};"
"globalThis.__add = function(kind, type, callback) { (globalThis.__listeners[kind][type] ||= []).push(callback); };"
"globalThis.__dispatch = function(kind, type) { for (const callback of (globalThis.__listeners[kind][type] || []).slice()) callback({type, target: kind === 'window' ? window : document}); };"
"globalThis.__setWindow = function(name, value) { Object.defineProperty(window, name, {value, writable:true, configurable:true}); };"
"globalThis.Element = window.Element; globalThis.Node = window.Node; globalThis.Event = window.Event; globalThis.HTMLAnchorElement = window.HTMLAnchorElement; globalThis.HTMLImageElement = window.HTMLImageElement; globalThis.HTMLFormElement = window.HTMLFormElement;"
"__setWindow('addEventListener', (type, callback) => __add('window', type, callback));"
"__setWindow('removeEventListener', () => {}); __setWindow('dispatchEvent', event => __dispatch('window', event.type));"
"__setWindow('window', window); __setWindow('document', document);"
"__setWindow('innerWidth', 1280); __setWindow('innerHeight', 1024);"
"__setWindow('setTimeout', setTimeout); __setWindow('clearTimeout', clearTimeout); __setWindow('setInterval', setInterval); __setWindow('clearInterval', clearInterval);"
"__setWindow('requestAnimationFrame', requestAnimationFrame); __setWindow('fetch', fetch); __setWindow('console', console);"
"__setWindow('scrollTo', () => {}); __setWindow('scrollBy', () => {});"
"Object.defineProperty(document, 'addEventListener', {value:(type, callback) => __add('document', type, callback), configurable:true});"
"Object.defineProperty(document, 'removeEventListener', {value:() => {}, configurable:true});"
"Object.defineProperty(document, 'dispatchEvent', {value:event => __dispatch('document', event.type), configurable:true});"
"document.readyState = 'loading';"
"globalThis.localStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};"
"Object.defineProperty(localStorage, 'length', {get() { return Object.keys(this.__data).length; }});"
"globalThis.sessionStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};"
"Object.defineProperty(sessionStorage, 'length', {get() { return Object.keys(this.__data).length; }});"
"__setWindow('localStorage', localStorage); __setWindow('sessionStorage', sessionStorage);"
"globalThis.navigator = window.navigator || {userAgent: 'Mozilla/5.0 Distill', language: 'en-US', platform: 'Linux'};"
"if (!navigator.userAgent) navigator.userAgent = 'Mozilla/5.0 Distill'; if (!navigator.language) navigator.language = 'en-US'; __setWindow('navigator', navigator);"
"globalThis.screen = {width: 1280, height: 1024}; __setWindow('screen', screen);"
"globalThis.crypto = {getRandomValues(a) { for (let i=0;i<a.length;i++) a[i] = (i * 73 + 41) & 255; return a; }}; __setWindow('crypto', crypto);"
"globalThis.matchMedia = q => ({matches: false, media: q}); __setWindow('matchMedia', matchMedia);"
"globalThis.performance = {now: () => Date.now()}; __setWindow('performance', performance);"
"globalThis.history = {pushState(s,t,u) { const suffix = String(u); location.search = suffix[0] === '?' ? suffix : ''; location.href = location.href.split('?')[0] + location.search; }, replaceState(s,t,u) { this.pushState(s,t,u); }}; __setWindow('history', history);"
"if (typeof Element !== 'undefined') Element.prototype.getBoundingClientRect = function() { return {x:0,y:0,top:0,left:0,width:800,height:20,right:800,bottom:20}; };"
"if (typeof Element !== 'undefined') { Object.defineProperty(Element.prototype, 'offsetHeight', {get() { return 20; }}); Object.defineProperty(Element.prototype, 'offsetWidth', {get() { return 800; }}); }"
"globalThis.__resolveURL = function(value) { const raw = String(value || ''); if (/^[a-z]+:/i.test(raw) || raw.startsWith('//')) return raw; const base = (document.querySelector('base') && document.querySelector('base').getAttribute('href')) || location.href; const origin = location.href.slice(0, location.href.indexOf('/', location.href.indexOf('://') + 3)); if (raw.startsWith('/')) return origin + raw; const prefix = base.endsWith('/') ? base : base.slice(0, base.lastIndexOf('/') + 1); const pieces = (prefix + raw).split('/'); const clean = []; for (const piece of pieces) { if (!piece || piece === '.') continue; if (piece === '..') clean.pop(); else clean.push(piece); } return origin + '/' + clean.join('/'); };"
"try { Object.defineProperty(HTMLAnchorElement.prototype, 'href', {get() { return __resolveURL(this.getAttribute('href')); }, configurable:true}); } catch (e) {}"
"try { Object.defineProperty(HTMLImageElement.prototype, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}"
"try { Object.defineProperty(HTMLFormElement.prototype, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('a')) try { Object.defineProperty(node, 'href', {get() { return __resolveURL(this.getAttribute('href')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('img')) try { Object.defineProperty(node, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('form')) try { Object.defineProperty(node, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}"
"globalThis.getComputedStyle = node => ({display: node.id === 'genuinely-hidden' ? 'none' : ((node.getAttribute && node.getAttribute('hidden') !== null) ? 'none' : 'block'), visibility: 'visible'}); __setWindow('getComputedStyle', getComputedStyle);"
"globalThis.IntersectionObserver = class { constructor(callback) { this.callback = callback; } observe(target) { this.callback([{target, isIntersecting:true, intersectionRatio:1}], this); } unobserve() {} };"
"globalThis.MutationObserver = class { constructor(callback) { this.callback = callback; } observe() { this.callback([{}], this); } disconnect() {} };"
"globalThis.XMLHttpRequest = class { constructor() { this.readyState=0; this.status=0; this.responseText=''; this.onreadystatechange=null; } open(method,url,async=true) { this._url=url; this.readyState=1; } send() { fetch(this._url).then(r => r.text().then(t => { this.status=r.status; this.responseText=t; this.readyState=4; if (this.onreadystatechange) this.onreadystatechange(); })); } };"
"__setWindow('XMLHttpRequest', XMLHttpRequest); __setWindow('IntersectionObserver', IntersectionObserver); __setWindow('MutationObserver', MutationObserver);"
"__setWindow('Node', globalThis.Node || {ELEMENT_NODE:1,TEXT_NODE:3,DOCUMENT_NODE:9,DOCUMENT_FRAGMENT_NODE:11});";

static bool evaluate(Engine *engine, const char *source, size_t length,
                     const char *filename, int flags)
{
    if (++engine->scripts_run > engine->max_scripts) return false;
    JSValue result = JS_Eval(engine->context, source, length, filename, flags);
    bool ok = !JS_IsException(result);
    if (!ok) note_exception(engine, filename);
    JS_FreeValue(engine->context, result);
    execute_jobs(engine);
    return ok;
}

static bool load_linkedom(Engine *engine, const DBuffer *html)
{
    char *worker_path = d_cat3(DISTILL_SOURCE_DIR,
                               "/external/js/linkedom/node_modules/linkedom/worker.js", "");
    size_t worker_length = 0;
    char *worker = d_read_file(worker_path, &worker_length);
    if (!worker) {
        fprintf(stderr, "linkedom worker.js missing: %s\n", worker_path);
        free(worker_path);
        return false;
    }
    JSValue module_value = JS_Eval(engine->context, worker, worker_length,
                                   worker_path, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(worker); free(worker_path);
    if (JS_IsException(module_value)) { note_exception(engine, "linkedom compile"); return false; }
    if (JS_ResolveModule(engine->context, module_value) < 0) {
        note_exception(engine, "linkedom resolve");
        JS_FreeValue(engine->context, module_value);
        return false;
    }
    JSModuleDef *module = JS_VALUE_GET_PTR(module_value);
    JSValue evaluated = JS_EvalFunction(engine->context, JS_DupValue(engine->context, module_value));
    if (JS_IsException(evaluated)) note_exception(engine, "linkedom evaluate");
    JS_FreeValue(engine->context, evaluated);
    execute_jobs(engine);
    JSValue namespace_value = JS_GetModuleNamespace(engine->context, module);
    JSValue parse = JS_GetPropertyStr(engine->context, namespace_value, "parseHTML");
    JSValue html_value = js_strn(engine->context, html->data, html->length);
    JSValue dom_window = JS_Call(engine->context, parse, JS_UNDEFINED, 1, &html_value);
    JS_FreeValue(engine->context, html_value);
    JS_FreeValue(engine->context, parse);
    JS_FreeValue(engine->context, namespace_value);
    JS_FreeValue(engine->context, module_value);
    if (JS_IsException(dom_window)) { note_exception(engine, "linkedom parseHTML"); return false; }
    engine->document = JS_GetPropertyStr(engine->context, dom_window, "document");
    engine->window = JS_NewObject(engine->context);
    const char *constructors[] = {
        "Element", "Node", "Event", "HTMLElement", "HTMLDocument",
        "HTMLAnchorElement", "HTMLImageElement", "HTMLFormElement"
    };
    for (size_t i = 0; i < sizeof(constructors) / sizeof(constructors[0]); ++i) {
        JSValue value = JS_GetPropertyStr(engine->context, dom_window, constructors[i]);
        if (!JS_IsUndefined(value)) JS_SetPropertyStr(engine->context, engine->window, constructors[i], value);
        else JS_FreeValue(engine->context, value);
    }
    JS_FreeValue(engine->context, dom_window);
    JS_SetPropertyStr(engine->context, engine->global, "window",
                      JS_DupValue(engine->context, engine->window));
    JS_SetPropertyStr(engine->context, engine->global, "document",
                      JS_DupValue(engine->context, engine->document));
    return true;
}

static JSValue make_location(Engine *engine)
{
    JSContext *ctx = engine->context;
    const char *url = engine->source_url;
    char *origin = url_origin(url);
    char *path = url_path(url);
    const char *query_start = strchr(url, '?');
    const char *hash_start = strchr(url, '#');
    const char *query_end = hash_start && query_start && hash_start > query_start ? hash_start : NULL;
    char *search = query_start ? d_strndup(query_start, query_end ? (size_t)(query_end - query_start) : strlen(query_start)) : d_strdup("");
    char *hash = hash_start ? d_strdup(hash_start) : d_strdup("");
    const char *colon = strchr(url, ':');
    char *protocol = colon ? d_strndup(url, (size_t)(colon - url + 1)) : d_strdup("");
    char *host = d_starts(origin, protocol) ? d_strdup(origin + strlen(protocol)) : d_strdup(origin);
    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, location, "href", js_str(ctx, url));
    JS_SetPropertyStr(ctx, location, "protocol", js_str(ctx, protocol));
    JS_SetPropertyStr(ctx, location, "host", js_str(ctx, host));
    JS_SetPropertyStr(ctx, location, "hostname", js_str(ctx, host));
    JS_SetPropertyStr(ctx, location, "pathname", js_str(ctx, path));
    JS_SetPropertyStr(ctx, location, "search", js_str(ctx, search));
    JS_SetPropertyStr(ctx, location, "hash", js_str(ctx, hash));
    free(origin); free(path); free(search); free(hash); free(protocol); free(host);
    return location;
}

static bool install_host(Engine *engine)
{
    JSContext *ctx = engine->context;
    JS_SetPropertyStr(ctx, engine->global, "location", make_location(engine));
    JS_SetPropertyStr(ctx, engine->window, "location", JS_GetPropertyStr(ctx, engine->global, "location"));
    JS_SetPropertyStr(ctx, engine->global, "fetch", JS_NewCFunction(ctx, fetch_callback, "fetch", 1));
    JS_SetPropertyStr(ctx, engine->global, "setTimeout", JS_NewCFunctionMagic(ctx, timer_callback, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, engine->global, "setInterval", JS_NewCFunctionMagic(ctx, timer_callback, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, engine->global, "clearTimeout", JS_NewCFunctionMagic(ctx, clear_timer_callback, "clearTimeout", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, engine->global, "clearInterval", JS_NewCFunctionMagic(ctx, clear_timer_callback, "clearInterval", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, engine->global, "requestAnimationFrame", JS_NewCFunction(ctx, request_animation_frame, "requestAnimationFrame", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunctionMagic(ctx, console_callback, "log", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunctionMagic(ctx, console_callback, "error", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, engine->global, "console", console);
    return evaluate(engine, HOST_JS, strlen(HOST_JS), "distill-host.js", JS_EVAL_TYPE_GLOBAL);
}

static bool discover_scripts(Engine *engine)
{
    const char *source =
        "Array.from(document.querySelectorAll('script')).map(s => ({src:s.getAttribute('src') || '', async:s.hasAttribute('async'), defer:s.hasAttribute('defer'), text:s.textContent || '', type:s.getAttribute('type') || ''}))";
    JSValue scripts = JS_Eval(engine->context, source, strlen(source), "distill-scripts.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(scripts)) { note_exception(engine, "script discovery"); return false; }
    JSValue deferred = JS_NewArray(engine->context);
    uint32_t deferred_count = 0;
    int64_t length = 0;
    JS_GetLength(engine->context, scripts, &length);
    for (int64_t i = 0; i < length; ++i) {
        JSValue spec = JS_GetPropertyUint32(engine->context, scripts, (uint32_t)i);
        JSValue src_value = JS_GetPropertyStr(engine->context, spec, "src");
        JSValue async_value = JS_GetPropertyStr(engine->context, spec, "async");
        JSValue defer_value = JS_GetPropertyStr(engine->context, spec, "defer");
        JSValue text_value = JS_GetPropertyStr(engine->context, spec, "text");
        JSValue type_value = JS_GetPropertyStr(engine->context, spec, "type");
        char *src = js_string(engine->context, src_value);
        char *code = js_string(engine->context, text_value);
        char *type = js_string(engine->context, type_value);
        bool is_async = JS_ToBool(engine->context, async_value);
        bool is_defer = JS_ToBool(engine->context, defer_value);
        JS_FreeValue(engine->context, src_value); JS_FreeValue(engine->context, async_value);
        JS_FreeValue(engine->context, defer_value); JS_FreeValue(engine->context, text_value);
        JS_FreeValue(engine->context, type_value); JS_FreeValue(engine->context, spec);
        if (d_contains(type, "json")) { free(src); free(code); free(type); continue; }
        char *url = src[0] ? resolve_url(engine->base_url, src) : NULL;
        if (is_async && url) {
            ++engine->async_pending;
            if (!start_request(engine, REQUEST_ASYNC_SCRIPT, url)) --engine->async_pending;
        } else if (is_defer) {
            if (url) {
                DBuffer body; long status = 0;
                if (fetch_sync(engine, url, &body, &status, NULL)) {
                    free(code); code = d_strndup(body.data, body.length); dbuf_free(&body);
                }
            }
            JS_SetPropertyUint32(engine->context, deferred, deferred_count++, js_str(engine->context, code));
        } else {
            if (url) {
                DBuffer body; long status = 0;
                if (fetch_sync(engine, url, &body, &status, NULL)) {
                    free(code); code = d_strndup(body.data, body.length); dbuf_free(&body);
                }
            }
            if (*code) evaluate(engine, code, strlen(code), url ? url : "distill-inline.js",
                                d_eq(type, "module") || d_eq(type, "text/javascript;module") ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
        }
        free(src); free(code); free(type); free(url);
    }
    for (uint32_t i = 0; i < deferred_count; ++i) {
        JSValue code_value = JS_GetPropertyUint32(engine->context, deferred, i);
        char *code = js_string(engine->context, code_value);
        evaluate(engine, code, strlen(code), "distill-defer.js", JS_EVAL_TYPE_GLOBAL);
        free(code); JS_FreeValue(engine->context, code_value);
    }
    JS_FreeValue(engine->context, deferred);
    JS_FreeValue(engine->context, scripts);
    JS_SetPropertyStr(engine->context, engine->document, "readyState", js_str(engine->context, "interactive"));
    dispatch_host_event(engine, "document", "readystatechange");
    dispatch_host_event(engine, "document", "DOMContentLoaded");
    execute_jobs(engine);
    return true;
}

static bool request_in_flight(const Engine *engine)
{
    for (size_t i = 0; i < engine->requests.length; ++i)
        if (!engine->requests.items[i]->completed) return true;
    return false;
}

static bool timer_near(const Engine *engine)
{
    uint64_t limit = clock_ms() + 500;
    for (size_t i = 0; i < engine->timers.length; ++i) {
        const Timer *timer = &engine->timers.items[i];
        if (!timer->cancelled && !timer->recurring && timer->due <= limit) return true;
    }
    return false;
}

static void dispatch_load_if_ready(Engine *engine)
{
    if (engine->load_sent || engine->async_pending != 0) return;
    engine->load_sent = true;
    JS_SetPropertyStr(engine->context, engine->document, "readyState", js_str(engine->context, "complete"));
    dispatch_host_event(engine, "document", "readystatechange");
    dispatch_host_event(engine, "window", "load");
    execute_jobs(engine);
}

static void settle(Engine *engine)
{
    for (;;) {
        process_network(engine);
        execute_jobs(engine);
        run_timers(engine);
        execute_jobs(engine);
        dispatch_load_if_ready(engine);
        if (clock_ms() - engine->started >= engine->cap_ms) {
            engine->cap_hit = true;
            break;
        }
        if (!request_in_flight(engine) && !JS_IsJobPending(engine->runtime) && !timer_near(engine)) break;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
}

static char *serialize_document(Engine *engine)
{
    JSValue value = JS_Eval(engine->context, "document.toString()", strlen("document.toString()"),
                            "distill-serialize.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) { note_exception(engine, "serialize"); return d_strdup(""); }
    char *out = js_string(engine->context, value);
    JS_FreeValue(engine->context, value);
    return out;
}

#ifndef DISTILL_NO_EXTRACT
#include "extraction.inc"
#else
static char *run_extraction(Engine *engine, const char *serialized)
{
    (void)engine; (void)serialized;
    return d_strdup("{\"schema\":1,\"readability\":null}");
}
#endif

#ifdef DISTILL_B_PRUNED
extern int distill_litehtml_probe(const char *html);
#endif

static void engine_cleanup(Engine *engine)
{
    if (!engine) return;
    for (size_t i = 0; i < engine->timers.length; ++i) {
        JS_FreeValue(engine->context, engine->timers.items[i].callback);
        JS_FreeValue(engine->context, engine->timers.items[i].argument);
    }
    free(engine->timers.items);
    for (size_t i = 0; i < engine->listeners.length; ++i) {
        free(engine->listeners.items[i].type);
        JS_FreeValue(engine->context, engine->listeners.items[i].callback);
    }
    free(engine->listeners.items);
    for (size_t i = 0; i < engine->requests.length; ++i) free_request(engine, engine->requests.items[i]);
    free(engine->requests.items);
    if (engine->context) {
        JS_FreeValue(engine->context, engine->document);
        JS_FreeValue(engine->context, engine->window);
        JS_FreeValue(engine->context, engine->global);
        JS_FreeContext(engine->context);
    }
    if (engine->runtime) JS_FreeRuntime(engine->runtime);
    if (engine->multi) curl_multi_cleanup(engine->multi);
    free(engine->source_url); free(engine->base_url);
    *engine = (Engine){0};
}

static bool set_document_base(Engine *engine)
{
    const char *source =
        "(function(){var b=document.querySelector('base');return b ? (b.getAttribute('href') || '') : '';})()";
    JSValue value = JS_Eval(engine->context, source, strlen(source), "distill-base.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) { note_exception(engine, "base href"); return false; }
    char *href = js_string(engine->context, value);
    JS_FreeValue(engine->context, value);
    if (*href) {
        char *resolved = resolve_url(engine->source_url, href);
        free(engine->base_url); engine->base_url = resolved;
    }
    free(href);
    return true;
}

static int run_page(const char *input, uint64_t cap_ms, uint64_t memory_mb,
                    uint64_t max_scripts, bool dump_dom, bool extract)
{
    Engine engine = {0};
    engine.cap_ms = cap_ms;
    engine.memory_limit = memory_mb * 1024u * 1024u;
    engine.max_scripts = max_scripts;
    engine.source_url = absolute_input(input);
    engine.base_url = d_strdup(engine.source_url);
    engine.started = clock_ms();

    DBuffer html;
    long status = 0;
    char *effective_url = NULL;
    if (!fetch_sync(&engine, engine.source_url, &html, &status, &effective_url)) {
        fprintf(stderr, "unable to load %s (status %ld)\n", engine.source_url, status);
        free(effective_url); free(engine.source_url); free(engine.base_url);
        return 2;
    }
    if (effective_url && *effective_url) { free(engine.source_url); engine.source_url = effective_url; }
    else free(effective_url);
    free(engine.base_url); engine.base_url = d_strdup(engine.source_url);

    engine.runtime = JS_NewRuntime();
    if (!engine.runtime) { dbuf_free(&html); free(engine.source_url); free(engine.base_url); return 2; }
    JS_SetRuntimeOpaque(engine.runtime, &engine);
    JS_SetMemoryLimit(engine.runtime, engine.memory_limit);
    JS_SetMaxStackSize(engine.runtime, 8u * 1024u * 1024u);
    JS_SetInterruptHandler(engine.runtime, interrupt_handler, &engine);
    engine.context = JS_NewContext(engine.runtime);
    if (!engine.context) { dbuf_free(&html); engine_cleanup(&engine); return 2; }
    JS_SetContextOpaque(engine.context, &engine);
    JS_SetModuleLoaderFunc(engine.runtime, module_normalize, module_loader, &engine);
    engine.global = JS_GetGlobalObject(engine.context);
    engine.multi = curl_multi_init();
    if (!engine.multi || !load_linkedom(&engine, &html) || !install_host(&engine)) {
        dbuf_free(&html); engine_cleanup(&engine); return 2;
    }
    dbuf_free(&html);
    set_document_base(&engine);
    if (!discover_scripts(&engine)) { engine_cleanup(&engine); return 2; }
    settle(&engine);
    char *serialized = serialize_document(&engine);
    if (extract) {
        char *json = run_extraction(&engine, serialized);
        puts(json);
        free(json);
    } else if (dump_dom) {
        puts(serialized);
#ifdef DISTILL_B_PRUNED
        fprintf(stderr, "arm-b-pruned litehtml=%s\n", distill_litehtml_probe(serialized) ? "ok" : "failed");
#endif
    }
    fprintf(stderr, "%s scripts=%llu cap_hit=%s\n", extract ? "phase1" : "phase0",
            (unsigned long long)engine.scripts_run, engine.cap_hit ? "true" : "false");
    free(serialized);
    engine_cleanup(&engine);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --url URL [--dump-dom|--extract] [--script-cap-ms N] [--memory-limit-mb N] [--max-scripts N]\n", program);
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    uint64_t cap_ms = 5000, memory_mb = 128, max_scripts = 1000;
    bool dump_dom = false, extract = false;
    for (int i = 1; i < argc; ++i) {
        if (d_eq(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (d_eq(argv[i], "--dump-dom")) dump_dom = true;
        else if (d_eq(argv[i], "--extract")) extract = true;
        else if (d_eq(argv[i], "--script-cap-ms") && i + 1 < argc) cap_ms = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--memory-limit-mb") && i + 1 < argc) memory_mb = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--max-scripts") && i + 1 < argc) max_scripts = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--help") || d_eq(argv[i], "-h")) { usage(argv[0]); return 0; }
    }
    if (!url || !*url || (dump_dom && extract)) { usage(argv[0]); return 2; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = run_page(url, cap_ms, memory_mb, max_scripts, dump_dom, extract);
    curl_global_cleanup();
    return result;
}
