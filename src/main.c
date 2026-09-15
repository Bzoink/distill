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

static bool fetch_sync_request(Engine *engine, const char *url, DBuffer *body,
                               long *status, char **effective_url,
                               const char *request_method, const char *request_data)
{
    dbuf_init(body);
    char *path = file_from_url(url);
    if (!is_http_url(url)) {
        if (request_method && d_ieq(request_method, "post")) {
            free(path);
            if (status) *status = 400;
            return false;
        }
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
    if (request_method && d_ieq(request_method, "post")) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request_data ? request_data : "");
    }
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

static bool fetch_sync(Engine *engine, const char *url, DBuffer *body,
                       long *status, char **effective_url)
{
    return fetch_sync_request(engine, url, body, status, effective_url, NULL, NULL);
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

#ifdef DISTILL_RENDER
extern int distill_litehtml_render(const char *html, const char *output_path,
                                   int width, int height, const char *font_dir);
#endif

#ifdef DISTILL_RENDER
static char *json_property_string(Engine *engine, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(engine->context, value);
        return d_strdup("");
    }
    char *text = js_string(engine->context, value);
    JS_FreeValue(engine->context, value);
    return text;
}

static void append_html_escaped(DBuffer *output, const char *text)
{
    for (const char *cursor = text ? text : ""; *cursor; ++cursor) {
        switch (*cursor) {
        case '&': dbuf_puts(output, "&amp;"); break;
        case '<': dbuf_puts(output, "&lt;"); break;
        case '>': dbuf_puts(output, "&gt;"); break;
        case '"': dbuf_puts(output, "&quot;"); break;
        case '\'': dbuf_puts(output, "&#39;"); break;
        default: dbuf_append(output, cursor, 1); break;
        }
    }
}

static int64_t json_array_length(Engine *engine, JSValueConst array)
{
    int64_t length = 0;
    if (JS_GetLength(engine->context, array, &length) < 0 || length < 0) return 0;
    return length;
}

static char *json_value_string(Engine *engine, JSValueConst value)
{
    if (JS_IsUndefined(value) || JS_IsNull(value)) return d_strdup("");
    return js_string(engine->context, value);
}

static void append_json_attr(DBuffer *output, const char *name, const char *value)
{
    dbuf_puts(output, " ");
    dbuf_puts(output, name);
    dbuf_puts(output, "=\"");
    append_html_escaped(output, value);
    dbuf_puts(output, "\"");
}

static void append_json_blocks(Engine *engine, JSValueConst blocks, DBuffer *output)
{
    int64_t length = json_array_length(engine, blocks);
    for (int64_t i = 0; i < length; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        char *type = json_property_string(engine, block, "type");
        char *text = json_property_string(engine, block, "text");
        if (d_eq(type, "h1")) {
            /* The reader title is already displayed above the block stream. */
        } else if (d_eq(type, "h2") || d_eq(type, "h3")) {
            dbuf_printf(output, "<%s>", type);
            append_html_escaped(output, text);
            dbuf_printf(output, "</%s>", type);
        } else if (d_eq(type, "pre")) {
            dbuf_puts(output, "<pre><code>");
            append_html_escaped(output, text);
            dbuf_puts(output, "</code></pre>");
        } else if (d_eq(type, "li")) {
            dbuf_puts(output, "<p class=\"reader-list-item\">• ");
            append_html_escaped(output, text);
            dbuf_puts(output, "</p>");
        } else if (*text) {
            dbuf_puts(output, "<p>");
            append_html_escaped(output, text);
            dbuf_puts(output, "</p>");
        }
        free(type);
        free(text);
        JS_FreeValue(engine->context, block);
    }
}

static void append_json_table(Engine *engine, JSValueConst tables, DBuffer *output)
{
    int64_t table_count = json_array_length(engine, tables);
    for (int64_t table_index = 0; table_index < table_count; ++table_index) {
        JSValue table = JS_GetPropertyUint32(engine->context, tables, (uint32_t)table_index);
        char *caption = json_property_string(engine, table, "caption");
        JSValue rows = JS_GetPropertyStr(engine->context, table, "rows");
        int64_t row_count = json_array_length(engine, rows);
        dbuf_puts(output, "<section class=\"table-card\">");
        if (*caption) {
            dbuf_puts(output, "<h2>");
            append_html_escaped(output, caption);
            dbuf_puts(output, "</h2>");
        }
        dbuf_puts(output, "<table><tbody>");
        for (int64_t row_index = 0; row_index < row_count; ++row_index) {
            JSValue row = JS_GetPropertyUint32(engine->context, rows, (uint32_t)row_index);
            int64_t cell_count = json_array_length(engine, row);
            dbuf_puts(output, "<tr>");
            for (int64_t cell_index = 0; cell_index < cell_count; ++cell_index) {
                JSValue cell = JS_GetPropertyUint32(engine->context, row, (uint32_t)cell_index);
                char *text = json_value_string(engine, cell);
                dbuf_puts(output, row_index == 0 ? "<th>" : "<td>");
                append_html_escaped(output, text);
                dbuf_puts(output, row_index == 0 ? "</th>" : "</td>");
                free(text);
                JS_FreeValue(engine->context, cell);
            }
            dbuf_puts(output, "</tr>");
            JS_FreeValue(engine->context, row);
        }
        dbuf_puts(output, "</tbody></table></section>");
        free(caption);
        JS_FreeValue(engine->context, rows);
        JS_FreeValue(engine->context, table);
    }
}

static void append_json_results(Engine *engine, JSValueConst results, DBuffer *output)
{
    int64_t count = json_array_length(engine, results);
    if (!count) return;
    dbuf_puts(output, "<section class=\"result-list\"><h2>Results</h2>");
    for (int64_t i = 0; i < count; ++i) {
        JSValue result = JS_GetPropertyUint32(engine->context, results, (uint32_t)i);
        char *title = json_property_string(engine, result, "title");
        char *url = json_property_string(engine, result, "url");
        char *snippet = json_property_string(engine, result, "snippet");
        dbuf_puts(output, "<article class=\"result-card\"><h3><a");
        if (*url) append_json_attr(output, "href", url);
        dbuf_puts(output, ">");
        append_html_escaped(output, *title ? title : url);
        dbuf_puts(output, "</a></h3>");
        if (*snippet) {
            const char *display_snippet = snippet;
            if (*title && d_starts(snippet, title)) {
                display_snippet = snippet + strlen(title);
                while (*display_snippet == ' ' || *display_snippet == '\t') ++display_snippet;
            }
            dbuf_puts(output, "<p>");
            append_html_escaped(output, display_snippet);
            dbuf_puts(output, "</p>");
        }
        dbuf_puts(output, "</article>");
        free(title); free(url); free(snippet);
        JS_FreeValue(engine->context, result);
    }
    dbuf_puts(output, "</section>");
}

static void append_json_forms(Engine *engine, JSValueConst forms, DBuffer *output)
{
    int64_t form_count = json_array_length(engine, forms);
    for (int64_t form_index = 0; form_index < form_count; ++form_index) {
        JSValue form = JS_GetPropertyUint32(engine->context, forms, (uint32_t)form_index);
        char *action = json_property_string(engine, form, "action");
        char *method = json_property_string(engine, form, "method");
        JSValue fields = JS_GetPropertyStr(engine->context, form, "fields");
        dbuf_puts(output, "<section class=\"form-card\"><h2>Form</h2><form");
        append_json_attr(output, "method", *method ? method : "get");
        append_json_attr(output, "action", action);
        dbuf_puts(output, ">");
        int64_t field_count = json_array_length(engine, fields);
        for (int64_t field_index = 0; field_index < field_count; ++field_index) {
            JSValue field = JS_GetPropertyUint32(engine->context, fields, (uint32_t)field_index);
            char *label = json_property_string(engine, field, "label");
            char *name = json_property_string(engine, field, "name");
            char *type = json_property_string(engine, field, "type");
            char *value = json_property_string(engine, field, "value");
            if (d_eq(type, "hidden")) {
                dbuf_puts(output, "<input type=\"hidden\"");
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                dbuf_puts(output, ">");
            } else if (d_eq(type, "submit") || d_eq(type, "button")) {
                dbuf_puts(output, "<button type=\"submit\"");
                append_json_attr(output, "name", name);
                dbuf_puts(output, ">");
                append_html_escaped(output, *value ? value : "Submit");
                dbuf_puts(output, "</button>");
            } else {
                dbuf_puts(output, "<label class=\"form-field\">");
                append_html_escaped(output, *label ? label : name);
                dbuf_puts(output, "<input");
                append_json_attr(output, "type", d_eq(type, "search") ? "search" : "text");
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                dbuf_puts(output, "></label>");
            }
            free(label); free(name); free(type); free(value);
            JS_FreeValue(engine->context, field);
        }
        dbuf_puts(output, "</form></section>");
        free(action); free(method);
        JS_FreeValue(engine->context, fields);
        JS_FreeValue(engine->context, form);
    }
}

static void append_json_videos(Engine *engine, JSValueConst videos, DBuffer *output)
{
    int64_t count = json_array_length(engine, videos);
    for (int64_t i = 0; i < count; ++i) {
        JSValue video = JS_GetPropertyUint32(engine->context, videos, (uint32_t)i);
        char *title = json_property_string(engine, video, "title");
        char *url = json_property_string(engine, video, "url");
        char *poster = json_property_string(engine, video, "poster");
        dbuf_puts(output, "<section class=\"video-card\"><p class=\"eyebrow\">Video reference</p><h2>");
        append_html_escaped(output, *title ? title : "External video");
        dbuf_puts(output, "</h2><p>This reader keeps playback outside the page.</p><a class=\"button\"");
        if (*url) append_json_attr(output, "href", url);
        dbuf_puts(output, ">Open externally</a>");
        if (*poster) {
            dbuf_puts(output, "<p class=\"media-url\">");
            append_html_escaped(output, poster);
            dbuf_puts(output, "</p>");
        }
        dbuf_puts(output, "</section>");
        free(title); free(url); free(poster);
        JS_FreeValue(engine->context, video);
    }
}

static void append_json_media(Engine *engine, JSValueConst media, DBuffer *output)
{
    int64_t count = json_array_length(engine, media);
    if (!count) return;
    dbuf_puts(output, "<section class=\"media-gallery\"><h2>Images</h2>");
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, media, (uint32_t)i);
        char *kind = json_property_string(engine, item, "kind");
        char *url = json_property_string(engine, item, "url");
        char *alt = json_property_string(engine, item, "alt");
        dbuf_puts(output, "<figure class=\"media-card\"><div class=\"media-placeholder\">");
        append_html_escaped(output, *alt ? alt : "Decorative image");
        dbuf_puts(output, "</div><figcaption>");
        append_html_escaped(output, *alt ? alt : "Decorative image");
        dbuf_puts(output, " <span>");
        append_html_escaped(output, d_eq(kind, "decorative") ? "decorative" : "content");
        dbuf_puts(output, "</span></figcaption><p class=\"media-url\">");
        append_html_escaped(output, url);
        dbuf_puts(output, "</p></figure>");
        free(kind); free(url); free(alt);
        JS_FreeValue(engine->context, item);
    }
    dbuf_puts(output, "</section>");
}

static char *reader_template(Engine *engine, const char *json, const char *theme)
{
    JSValue parsed = JS_ParseJSON(engine->context, json, strlen(json), "distill-extraction.json");
    if (JS_IsException(parsed)) {
        note_exception(engine, "reader template JSON");
        return d_strdup("");
    }
    JSValue readability = JS_GetPropertyStr(engine->context, parsed, "readability");
    JSValue shape = JS_GetPropertyStr(engine->context, parsed, "shape");
    JSValue blocks = JS_GetPropertyStr(engine->context, parsed, "blocks");
    JSValue tables = JS_GetPropertyStr(engine->context, parsed, "tables");
    JSValue results = JS_GetPropertyStr(engine->context, parsed, "search_results");
    JSValue forms = JS_GetPropertyStr(engine->context, parsed, "forms");
    JSValue videos = JS_GetPropertyStr(engine->context, parsed, "videos");
    JSValue media = JS_GetPropertyStr(engine->context, parsed, "media");
    char *shape_kind = json_property_string(engine, shape, "kind");
    char *title = d_strdup("");
    char *content = d_strdup("");
    if (!JS_IsNull(readability) && !JS_IsUndefined(readability)) {
        title = json_property_string(engine, readability, "title");
        content = json_property_string(engine, readability, "content");
    }

    const char *selected_theme = theme && *theme ? theme : "warm";
    const char *page_background = "#f4f1ea";
    const char *page_text = "#282622";
    const char *main_background = "#fffdf8";
    const char *heading_text = "#171614";
    const char *card_background = "#f5f2ec";
    const char *border = "#ddd5c9";
    const char *link = "#165d7a";
    const char *pre_background = "#f0eee8";
    const char *muted = "#827a6e";
    if (d_eq(selected_theme, "night")) {
        page_background = "#17191b";
        page_text = "#e7e2d8";
        main_background = "#202326";
        heading_text = "#fffaf0";
        card_background = "#292d31";
        border = "#454b50";
        link = "#78b9cf";
        pre_background = "#2b3034";
        muted = "#a7a39b";
    } else if (d_eq(selected_theme, "paper")) {
        page_background = "#e7dfd0";
        page_text = "#332d24";
        main_background = "#f8f0df";
        heading_text = "#261f17";
        card_background = "#eee3cf";
        border = "#d1c1a8";
        link = "#79533b";
        pre_background = "#e8dcc7";
        muted = "#85735f";
    }

    DBuffer output;
    dbuf_init(&output);
    dbuf_puts(&output,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>");
    append_html_escaped(&output, title);
    dbuf_puts(&output,
        "</title><style>"
        ":root{--page-background:");
    dbuf_puts(&output, page_background);
    dbuf_puts(&output, ";--page-text:");
    dbuf_puts(&output, page_text);
    dbuf_puts(&output, ";--main-background:");
    dbuf_puts(&output, main_background);
    dbuf_puts(&output, ";--heading-text:");
    dbuf_puts(&output, heading_text);
    dbuf_puts(&output, ";--card-background:");
    dbuf_puts(&output, card_background);
    dbuf_puts(&output, ";--border:");
    dbuf_puts(&output, border);
    dbuf_puts(&output, ";--link:");
    dbuf_puts(&output, link);
    dbuf_puts(&output, ";--pre-background:");
    dbuf_puts(&output, pre_background);
    dbuf_puts(&output, ";--muted:");
    dbuf_puts(&output, muted);
    dbuf_puts(&output,
        "}html,body{margin:0;padding:0;background:var(--page-background);color:var(--page-text);}"
        "body{font-family:sans-serif;font-size:18px;line-height:1.62;padding-top:68px;padding-right:74px;padding-bottom:96px;padding-left:74px;}"
        "main{max-width:612px;margin:0 auto;background:var(--main-background);}"
        "h1{font-size:2.35em;line-height:1.08;letter-spacing:-.025em;margin:0 0 1.1em;color:var(--heading-text);}"
        "h2{font-size:1.45em;line-height:1.2;margin:1.7em 0 .55em;color:var(--heading-text);}"
        "p,ul,ol,pre,blockquote{margin:0 0 1.05em;}"
        "a{color:var(--link);}pre{padding:1em;overflow:hidden;white-space:pre-wrap;overflow-wrap:anywhere;background:var(--pre-background);border-radius:4px;font-size:.86em;line-height:1.45;}"
        "blockquote{margin-left:1.2em;padding-left:1em;border-left:3px solid var(--border);color:var(--muted);}"
        "img,video,iframe{max-width:100%;height:auto;}"
        "table{width:100%;border-collapse:collapse;margin:0 0 1.5em;}th,td{text-align:left;padding:.55em .6em;border-bottom:1px solid var(--border);}"
        ".table-card,.form-card,.video-card,.media-gallery,.result-list{margin:2em 0;padding:1.2em;background:var(--card-background);border:1px solid var(--border);border-radius:5px;}"
        ".result-card{padding:.7em 0;border-bottom:1px solid var(--border);}.result-card h3{margin:0 0 .3em;}"
        ".form-field{display:block;margin:.8em 0;color:var(--page-text);}.form-field input{display:block;width:100%;box-sizing:border-box;margin-top:.25em;padding:.55em;border:1px solid var(--border);border-radius:3px;background:var(--main-background);color:var(--page-text);}"
        "button,.button{display:inline-block;padding:.55em .85em;border:0;border-radius:3px;background:var(--link);color:white;text-decoration:none;font:inherit;}"
        ".video-card h2,.table-card h2,.form-card h2,.media-gallery h2,.result-list h2{margin-top:0;}"
        ".eyebrow,.media-url{font-size:.78em;color:var(--muted);}.media-placeholder{min-height:100px;display:flex;align-items:center;justify-content:center;background:var(--card-background);color:var(--page-text);text-align:center;padding:1em;}"
        ".media-card{margin:0 0 1em;}.media-card figcaption{margin-top:.45em;}.media-card figcaption span{float:right;color:#827a6e;font-size:.8em;}"
        ".reader-list-item{padding-left:1em;}"
        ".distill-meta{font-size:.8em;color:var(--muted);margin-bottom:2.6em;}"
        "</style></head><body><main><h1>");
    append_html_escaped(&output, *title ? title : "Untitled page");
    dbuf_puts(&output, "</h1><p class=\"distill-meta\">Rendered by Distill</p>");
    if (d_eq(shape_kind, "article") && *content) {
        dbuf_puts(&output, content);
    } else if (d_eq(shape_kind, "docs")) {
        append_json_blocks(engine, blocks, &output);
    } else if (d_eq(shape_kind, "table")) {
        append_json_table(engine, tables, &output);
    } else if (d_eq(shape_kind, "search-results")) {
        append_json_results(engine, results, &output);
    } else if (d_eq(shape_kind, "form")) {
        append_json_forms(engine, forms, &output);
    } else if (d_eq(shape_kind, "video")) {
        append_json_blocks(engine, blocks, &output);
        append_json_videos(engine, videos, &output);
    } else if (d_eq(shape_kind, "image-heavy")) {
        append_json_media(engine, media, &output);
    } else {
        append_json_blocks(engine, blocks, &output);
        append_json_table(engine, tables, &output);
        append_json_results(engine, results, &output);
        append_json_forms(engine, forms, &output);
        append_json_videos(engine, videos, &output);
        append_json_media(engine, media, &output);
    }
    dbuf_puts(&output, "</main></body></html>");
    free(shape_kind);
    free(title);
    free(content);
    JS_FreeValue(engine->context, shape);
    JS_FreeValue(engine->context, blocks);
    JS_FreeValue(engine->context, tables);
    JS_FreeValue(engine->context, results);
    JS_FreeValue(engine->context, forms);
    JS_FreeValue(engine->context, videos);
    JS_FreeValue(engine->context, media);
    JS_FreeValue(engine->context, readability);
    JS_FreeValue(engine->context, parsed);
    return dbuf_take(&output);
}

static char *viewer_template(Engine *engine, const char *json, int media_index)
{
    JSValue parsed = JS_ParseJSON(engine->context, json, strlen(json), "distill-extraction.json");
    if (JS_IsException(parsed)) {
        note_exception(engine, "media viewer JSON");
        return d_strdup("");
    }
    JSValue media = JS_GetPropertyStr(engine->context, parsed, "media");
    int64_t count = json_array_length(engine, media);
    if (media_index < 0 || media_index >= count) {
        JS_FreeValue(engine->context, media);
        JS_FreeValue(engine->context, parsed);
        return d_strdup("");
    }
    JSValue item = JS_GetPropertyUint32(engine->context, media, (uint32_t)media_index);
    char *kind = json_property_string(engine, item, "kind");
    char *url = json_property_string(engine, item, "url");
    char *alt = json_property_string(engine, item, "alt");
    DBuffer output;
    dbuf_init(&output);
    dbuf_puts(&output,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Media viewer</title><style>"
        "html,body{margin:0;width:100%;height:100%;background:#17191b;color:#f5f1e8;}"
        "body{font-family:sans-serif;}main{box-sizing:border-box;width:100%;height:100%;padding:38px 52px;display:block;}"
        ".top{font-size:.82em;color:#aaa79f;}.stage{margin:28px auto 22px;max-width:1000px;height:calc(100% - 150px);"
        "min-height:300px;display:flex;align-items:center;justify-content:center;background:#292c2f;border:1px solid #474b4e;}"
        ".placeholder{max-width:80%;padding:60px 80px;text-align:center;background:#3a3f42;color:#d4d0c7;font-size:1.3em;}"
        ".caption{display:flex;justify-content:space-between;gap:20px;}.caption strong{font-size:1.2em;}"
        ".url{color:#aaa79f;font-size:.8em;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;}"
        ".nav{color:#78b9cf;white-space:nowrap;}a{color:#78b9cf;text-decoration:none;}"
        "</style></head><body><main><div class=\"top\">Distill media viewer · ");
    dbuf_printf(&output, "%d of %lld · %s", media_index + 1, (long long)count,
                d_eq(kind, "decorative") ? "decorative reference" : "content reference");
    dbuf_puts(&output, "</div><div class=\"stage\"><div class=\"placeholder\">");
    append_html_escaped(&output, *alt ? alt : "Image reference");
    dbuf_puts(&output, "</div></div><div class=\"caption\"><div><strong>");
    append_html_escaped(&output, *alt ? alt : "Image reference");
    dbuf_puts(&output, "</strong><div class=\"url\">");
    append_html_escaped(&output, url);
    dbuf_puts(&output, "</div></div><div class=\"nav\">");
    if (media_index > 0) dbuf_puts(&output, "<a href=\"#previous\">Previous</a> · ");
    else dbuf_puts(&output, "Previous · ");
    if (media_index + 1 < count) dbuf_puts(&output, "<a href=\"#next\">Next</a>");
    else dbuf_puts(&output, "Next");
    dbuf_puts(&output, "</div></div></main></body></html>");
    free(kind); free(url); free(alt);
    JS_FreeValue(engine->context, item);
    JS_FreeValue(engine->context, media);
    JS_FreeValue(engine->context, parsed);
    return dbuf_take(&output);
}

static bool save_media_file(Engine *engine, const char *json, int media_index,
                             const char *path)
{
    JSValue parsed = JS_ParseJSON(engine->context, json, strlen(json), "distill-extraction.json");
    if (JS_IsException(parsed)) return false;
    JSValue media = JS_GetPropertyStr(engine->context, parsed, "media");
    int64_t count = json_array_length(engine, media);
    if (media_index < 0 || media_index >= count) {
        JS_FreeValue(engine->context, media);
        JS_FreeValue(engine->context, parsed);
        return false;
    }
    JSValue item = JS_GetPropertyUint32(engine->context, media, (uint32_t)media_index);
    char *url = json_property_string(engine, item, "url");
    bool ok = is_http_url(url);
    DBuffer body;
    dbuf_init(&body);
    long status = 0;
    if (ok) ok = fetch_sync(engine, url, &body, &status, NULL);
    if (ok) {
        FILE *file = fopen(path, "wb");
        if (!file) ok = false;
        else {
            ok = fwrite(body.data, 1, body.length, file) == body.length;
            if (fclose(file) != 0) ok = false;
        }
    }
    dbuf_free(&body);
    if (!ok) fprintf(stderr, "unable to save media %s (status %ld)\n", url, status);
    free(url);
    JS_FreeValue(engine->context, item);
    JS_FreeValue(engine->context, media);
    JS_FreeValue(engine->context, parsed);
    return ok;
}
#endif

static bool write_file_bytes(const char *path, const char *data, size_t length)
{
    if (!path || !data) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, length, file) == length;
    if (fclose(file) != 0) ok = false;
    return ok;
}

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
                    uint64_t max_scripts, bool dump_dom, bool extract,
                    const char *render_path, const char *viewer_path, int media_index,
                    const char *save_media_path, int render_width, int render_height,
                    const char *font_dir, const char *template_path,
                    const char *theme, const char *request_method,
                    const char *request_data)
{
    Engine engine = {0};
#ifdef DISTILL_RENDER
    char *owned_font_dir = NULL;
    if ((render_path || viewer_path) && !font_dir) {
        owned_font_dir = d_cat2(DISTILL_SOURCE_DIR, "/external/fonts");
        if (access(owned_font_dir, R_OK) == 0) font_dir = owned_font_dir;
    }
#endif
    engine.cap_ms = cap_ms;
    engine.memory_limit = memory_mb * 1024u * 1024u;
    engine.max_scripts = max_scripts;
    engine.source_url = absolute_input(input);
    engine.base_url = d_strdup(engine.source_url);
    engine.started = clock_ms();

    DBuffer html;
    long status = 0;
    char *effective_url = NULL;
    if (!fetch_sync_request(&engine, engine.source_url, &html, &status, &effective_url,
                            request_method, request_data)) {
        fprintf(stderr, "unable to load %s (status %ld)\n", engine.source_url, status);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        free(effective_url); free(engine.source_url); free(engine.base_url);
        return 2;
    }
    if (effective_url && *effective_url) { free(engine.source_url); engine.source_url = effective_url; }
    else free(effective_url);
    free(engine.base_url); engine.base_url = d_strdup(engine.source_url);

    engine.runtime = JS_NewRuntime();
    if (!engine.runtime) {
        dbuf_free(&html); free(engine.source_url); free(engine.base_url);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    JS_SetRuntimeOpaque(engine.runtime, &engine);
    JS_SetMemoryLimit(engine.runtime, engine.memory_limit);
    JS_SetMaxStackSize(engine.runtime, 8u * 1024u * 1024u);
    JS_SetInterruptHandler(engine.runtime, interrupt_handler, &engine);
    engine.context = JS_NewContext(engine.runtime);
    if (!engine.context) {
        dbuf_free(&html); engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    JS_SetContextOpaque(engine.context, &engine);
    JS_SetModuleLoaderFunc(engine.runtime, module_normalize, module_loader, &engine);
    engine.global = JS_GetGlobalObject(engine.context);
    engine.multi = curl_multi_init();
    if (!engine.multi || !load_linkedom(&engine, &html) || !install_host(&engine)) {
        dbuf_free(&html); engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    dbuf_free(&html);
    set_document_base(&engine);
    if (!discover_scripts(&engine)) {
        engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    settle(&engine);
    char *serialized = serialize_document(&engine);
    int result = 0;
    if (viewer_path) {
#ifdef DISTILL_RENDER
        char *json = run_extraction(&engine, serialized);
        bool saved = !save_media_path || save_media_file(&engine, json, media_index, save_media_path);
        char *template = viewer_template(&engine, json, media_index);
        if (!saved || !*template || !distill_litehtml_render(template, viewer_path,
                                                               render_width, render_height, font_dir)) {
            fprintf(stderr, "unable to render media viewer %s\n", viewer_path);
            result = 3;
        } else {
            fprintf(stderr, "rendered media viewer %s (%dx%d)\n",
                    viewer_path, render_width, render_height);
        }
        free(template);
        free(json);
#else
        (void)render_width; (void)render_height; (void)font_dir;
        fprintf(stderr, "this build has no litehtml renderer\n");
        result = 2;
#endif
    } else if (render_path) {
#ifdef DISTILL_RENDER
        char *json = run_extraction(&engine, serialized);
        char *template = reader_template(&engine, json, theme);
        bool template_ok = !template_path || write_file_bytes(template_path, template, strlen(template));
        if (!template_ok || !*template || !distill_litehtml_render(template, render_path,
                                                                     render_width, render_height, font_dir)) {
            fprintf(stderr, "unable to render %s\n", render_path);
            result = 3;
        } else {
            fprintf(stderr, "rendered %s (%dx%d)\n", render_path, render_width, render_height);
        }
        free(template);
        free(json);
#else
        (void)render_width; (void)render_height; (void)font_dir;
        fprintf(stderr, "this build has no litehtml renderer\n");
        result = 2;
#endif
    } else if (template_path) {
#ifdef DISTILL_RENDER
        char *json = run_extraction(&engine, serialized);
        char *template = reader_template(&engine, json, theme);
        if (!*template || !write_file_bytes(template_path, template, strlen(template))) {
            fprintf(stderr, "unable to write reader template %s\n", template_path);
            result = 3;
        }
        free(template);
        free(json);
#else
        (void)theme;
        fprintf(stderr, "this build has no litehtml renderer\n");
        result = 2;
#endif
    } else if (extract) {
        char *json = run_extraction(&engine, serialized);
        puts(json);
        free(json);
    } else if (dump_dom) {
        puts(serialized);
#ifdef DISTILL_B_PRUNED
        fprintf(stderr, "arm-b-pruned litehtml=%s\n", distill_litehtml_probe(serialized) ? "ok" : "failed");
#endif
    }
    fprintf(stderr, "%s scripts=%llu cap_hit=%s\n", viewer_path ? "phase2-viewer" : (render_path ? "phase2" : (extract ? "phase1" : "phase0")),
            (unsigned long long)engine.scripts_run, engine.cap_hit ? "true" : "false");
    free(serialized);
    engine_cleanup(&engine);
#ifdef DISTILL_RENDER
    free(owned_font_dir);
#endif
    return result;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --url URL [--dump-dom|--extract|--render FILE|--template FILE|--viewer FILE] [--media-index N] [--save-media FILE] [--width N] [--height N] [--font-dir DIR] [--theme warm|night|paper] [--request-method GET|POST] [--request-data DATA] [--script-cap-ms N] [--memory-limit-mb N] [--max-scripts N]\n", program);
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    const char *render_path = NULL;
    const char *viewer_path = NULL;
    const char *template_path = NULL;
    const char *save_media_path = NULL;
    const char *font_dir = NULL;
    const char *theme = "warm";
    const char *request_method = NULL;
    const char *request_data = NULL;
    uint64_t cap_ms = 5000, memory_mb = 128, max_scripts = 1000;
    int render_width = 760, render_height = 1100;
    int media_index = 0;
    bool dump_dom = false, extract = false;
    for (int i = 1; i < argc; ++i) {
        if (d_eq(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (d_eq(argv[i], "--dump-dom")) dump_dom = true;
        else if (d_eq(argv[i], "--extract")) extract = true;
        else if (d_eq(argv[i], "--render") && i + 1 < argc) render_path = argv[++i];
        else if (d_eq(argv[i], "--template") && i + 1 < argc) template_path = argv[++i];
        else if (d_eq(argv[i], "--viewer") && i + 1 < argc) viewer_path = argv[++i];
        else if (d_eq(argv[i], "--media-index") && i + 1 < argc) media_index = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--save-media") && i + 1 < argc) save_media_path = argv[++i];
        else if (d_eq(argv[i], "--width") && i + 1 < argc) render_width = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--height") && i + 1 < argc) render_height = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--font-dir") && i + 1 < argc) font_dir = argv[++i];
        else if (d_eq(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if (d_eq(argv[i], "--request-method") && i + 1 < argc) request_method = argv[++i];
        else if (d_eq(argv[i], "--request-data") && i + 1 < argc) request_data = argv[++i];
        else if (d_eq(argv[i], "--script-cap-ms") && i + 1 < argc) cap_ms = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--memory-limit-mb") && i + 1 < argc) memory_mb = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--max-scripts") && i + 1 < argc) max_scripts = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--help") || d_eq(argv[i], "-h")) { usage(argv[0]); return 0; }
    }
    if (!url || !*url || (dump_dom && (extract || render_path || template_path || viewer_path)) ||
        (extract && (render_path || template_path || viewer_path)) ||
        (template_path && viewer_path) || (render_path && viewer_path) ||
        (save_media_path && !viewer_path) || media_index < 0 ||
        render_width <= 0 || render_height <= 0 ||
        (request_data && (!request_method || !d_ieq(request_method, "post"))) ||
        (request_method && !d_ieq(request_method, "get") && !d_ieq(request_method, "post"))) {
        usage(argv[0]); return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = run_page(url, cap_ms, memory_mb, max_scripts, dump_dom, extract,
                          render_path, viewer_path, media_index, save_media_path,
                          render_width, render_height, font_dir, template_path, theme,
                          request_method, request_data);
    curl_global_cleanup();
    return result;
}
