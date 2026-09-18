#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "cutil.h"
#include "distill_version.h"
#include "reader_cells.h"
#include "reader_walk.h"

#include <quickjs.h>
#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DISTILL_SOURCE_DIR
#define DISTILL_SOURCE_DIR "."
#endif

/* What we tell servers we are. The HTTP header and navigator.userAgent must
   carry the same string: a page that compares what the server saw against what
   the DOM reports is checking exactly this pair, and two literals drifting
   apart is not a difference anyone would notice until a site did.

   Claiming Chrome is a content-fidelity decision, not a disguise -- it
   materially changes what HTML servers send. The forced HTTP/2 below belongs
   to the same decision. */
#define DISTILL_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36"

/* Reader zoom, as a percentage of the design size. The bounds are part of the
   command line, so they are defined outside the renderer's build. */
#define READER_ZOOM_MIN 50
#define READER_ZOOM_MAX 300

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

typedef struct {
    char *name;
    char *value;
} Header;

typedef struct {
    Header *items;
    size_t length;
    size_t capacity;
} HeaderVec;

static void header_vec_free(HeaderVec *headers)
{
    for (size_t i = 0; i < headers->length; ++i) {
        free(headers->items[i].name);
        free(headers->items[i].value);
    }
    free(headers->items);
    *headers = (HeaderVec){0};
}

static void header_vec_clear(HeaderVec *headers)
{
    header_vec_free(headers);
}

static void header_vec_push(HeaderVec *headers, const char *name, const char *value)
{
    d_vec_reserve((void **)&headers->items, &headers->capacity, headers->length,
                  sizeof(*headers->items), headers->length + 1);
    headers->items[headers->length++] = (Header){d_strdup(name), d_strdup(value)};
}

typedef struct Request {
    RequestKind kind;
    bool module_script;
    bool async_evaluated;
    size_t async_order;
    Engine *engine;
    CURL *easy;
    char *url;
    char *method;
    char *request_data;
    struct curl_slist *headers;
    DBuffer body;
    long status;
    JSValue resolve;
    JSValue reject;
    bool completed;
    unsigned attempts;
    char *effective_url;
    struct curl_slist *wire_headers;
    HeaderVec response_headers;
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
    CURLSH *share;
    char *source_url;
    char *base_url;
    char *cookie_jar_path;
    bool cookie_jar_persistent;
    char *script_cookies;
    uint64_t started;
    uint64_t cap_ms;
    uint64_t memory_limit;
    uint64_t max_scripts;
    uint64_t scripts_run;
    uint64_t script_errors;
    size_t async_sequence;
    size_t async_next_order;
    uint64_t next_timer;
    int async_pending;
    bool load_sent;
    bool cap_hit;
    /* Progress reporting, off unless the caller asked for it. */
    bool progress;
    int progress_percent;
    const char *progress_phase;
    const char *challenge;
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

/* One line per step on stdout: "progress PERCENT PHASE". The caller wanted to
   know the run is alive and roughly how far along it is, and a one-shot child
   process cannot say so any other way.

   The percentage is the pipeline's shape, not a measurement: each phase owns
   a band and moves inside it. What it honestly reports is which phase the run
   is in and that the phases happen in this order -- the fetch band tracks the
   bytes when the server declares a length, and the script band tracks the
   settle budget, which is a real upper bound the run usually beats. */
#define PROGRESS_FETCH_START 2
#define PROGRESS_FETCH_END 35
#define PROGRESS_SCRIPTS_END 75
#define PROGRESS_EXTRACT_END 85

static void report_progress(Engine *engine, int percent, const char *phase)
{
    if (!engine || !engine->progress) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Never walk backwards: a redirect restarts the transfer and the reader
       should not watch the page load twice. */
    if (percent < engine->progress_percent) percent = engine->progress_percent;
    if (percent == engine->progress_percent && d_eq(phase, engine->progress_phase)) return;
    engine->progress_percent = percent;
    engine->progress_phase = phase;
    printf("progress %d %s\n", percent, phase);
    fflush(stdout);
}

/* Where the page actually came from. A redirect -- or a scheme this run had
   to correct -- leaves the reader somewhere other than the address that was
   asked for, and the shell needs the real one: reloading the address that
   merely pointed here is not the same request. */
static void report_location(Engine *engine)
{
    if (!engine || !engine->progress || !engine->source_url) return;
    if (strchr(engine->source_url, '\n')) return;
    printf("location %s\n", engine->source_url);
    fflush(stdout);
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

static int64_t json_array_length_for_context(JSContext *ctx, JSValueConst array)
{
    int64_t length = 0;
    if (JS_GetLength(ctx, array, &length) < 0 || length < 0) return 0;
    return length;
}

static bool is_http_url(const char *url)
{
    return d_starts(url, "http://") || d_starts(url, "https://");
}

static char *url_origin(const char *url);

static char *url_cookie_domain(const char *url)
{
    char *origin = url_origin(url);
    const char *scheme = strstr(origin, "://");
    const char *start = scheme ? scheme + 3 : origin;
    char *authority = d_strdup(start);
    char *at = strrchr(authority, '@');
    if (at) memmove(authority, at + 1, strlen(at + 1) + 1);
    if (authority[0] == '[') {
        char *end = strchr(authority, ']');
        if (end) end[1] = 0;
    } else {
        char *port = strrchr(authority, ':');
        if (port) *port = 0;
    }
    free(origin);
    return authority;
}

static char *url_host_for_cookie(const char *url)
{
    return url_cookie_domain(url);
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

static long connection_timeout_ms(const Engine *engine)
{
    uint64_t timeout = engine->cap_ms < 5000 ? engine->cap_ms : 5000;
    return (long)(timeout ? timeout : 1);
}

static bool header_list_has(const struct curl_slist *headers, const char *name)
{
    for (const struct curl_slist *item = headers; item; item = item->next) {
        const char *colon = strchr(item->data, ':');
        size_t length = colon ? (size_t)(colon - item->data) : strlen(item->data);
        char *candidate = d_strndup(item->data, length);
        bool match = d_ieq(candidate, name);
        free(candidate);
        if (match) return true;
    }
    return false;
}

static void curl_header_append(struct curl_slist **headers, const char *name,
                               const char *value)
{
    DBuffer line;
    dbuf_init(&line);
    dbuf_printf(&line, "%s: %s", name, value ? value : "");
    struct curl_slist *updated = curl_slist_append(*headers, line.data);
    dbuf_free(&line);
    if (updated) *headers = updated;
}

static struct curl_slist *default_request_headers(const char *referer,
                                                   const struct curl_slist *custom,
                                                   bool document_request)
{
    struct curl_slist *headers = NULL;
    if (!header_list_has(custom, "Accept")) {
        curl_header_append(&headers, "Accept",
                           document_request
                               ? "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8"
                               : "*/*");
    }
    if (!header_list_has(custom, "Accept-Language")) {
        curl_header_append(&headers, "Accept-Language", "en-US,en;q=0.9");
    }
    if (referer && *referer && !header_list_has(custom, "Referer")) {
        curl_header_append(&headers, "Referer", referer);
    }
    for (const struct curl_slist *item = custom; item; item = item->next) {
        struct curl_slist *updated = curl_slist_append(headers, item->data);
        if (updated) headers = updated;
    }
    return headers;
}

static void configure_curl_common(Engine *engine, CURL *easy, const char *referer)
{
    curl_easy_setopt(easy, CURLOPT_SHARE, engine->share);
    /* An empty COOKIEFILE enables libcurl's in-memory cookie engine. The
       share handle makes it common to the main request and all subresources.
       A session file is named instead, so cookies written by an earlier run
       are read back in -- which is what lets a login survive to the next
       navigation. */
    curl_easy_setopt(easy, CURLOPT_COOKIEFILE,
                     engine->cookie_jar_persistent && engine->cookie_jar_path
                         ? engine->cookie_jar_path : "");
    if (engine->cookie_jar_path) {
        curl_easy_setopt(easy, CURLOPT_COOKIEJAR, engine->cookie_jar_path);
    }
    curl_easy_setopt(easy, CURLOPT_USERAGENT, DISTILL_USER_AGENT);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    /* A client presenting a Chrome user agent should negotiate the protocol
       version Chrome negotiates. HTTP/1.1-only traffic from a self-declared
       Chrome is an inconsistency some origins reject on its own. */
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    /* FOLLOWLOCATION without a bound follows a redirect loop until the
       request timeout expires, spending the whole settle budget. */
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(easy, CURLOPT_AUTOREFERER, 1L);
    (void)referer;
}

static bool cookie_domain_matches(const char *cookie_domain, const char *host)
{
    while (*cookie_domain == '.') ++cookie_domain;
    size_t domain_length = strlen(cookie_domain);
    size_t host_length = strlen(host);
    if (domain_length > host_length) return false;
    const char *suffix = host + host_length - domain_length;
    return d_ieq(suffix, cookie_domain) &&
           (suffix == host || suffix[-1] == '.');
}

static bool cookie_path_matches(const char *cookie_path, const char *path)
{
    if (!cookie_path || !*cookie_path) cookie_path = "/";
    if (!path || !*path) path = "/";
    size_t length = strlen(cookie_path);
    if (strncmp(cookie_path, path, length) != 0) return false;
    return length == 1 || path[length] == '\0' || cookie_path[length - 1] == '/' ||
           path[length] == '/';
}

static char *cookie_string_from_list(const struct curl_slist *list, const char *url)
{
    char *host = url_host_for_cookie(url);
    char *path = url_path(url);
    bool secure_url = d_starts(url, "https://");
    DBuffer out;
    dbuf_init(&out);
    time_t now = time(NULL);
    for (const struct curl_slist *item = list; item; item = item->next) {
        char *line = d_strdup(item->data);
        char *fields[7] = {0};
        size_t field_count = 0;
        char *cursor = line;
        while (field_count < 7) {
            fields[field_count++] = cursor;
            char *tab = strchr(cursor, '\t');
            if (!tab) break;
            *tab = '\0';
            cursor = tab + 1;
        }
        if (field_count == 7) {
            const char *domain = fields[0];
            if (d_starts(domain, "#HttpOnly_")) domain += strlen("#HttpOnly_");
            long long expires = strtoll(fields[4], NULL, 10);
            bool valid_expiry = expires <= 0 || (time_t)expires > now;
            bool domain_ok = cookie_domain_matches(domain, host);
            bool path_ok = cookie_path_matches(fields[2], path);
            bool secure_ok = !d_ieq(fields[3], "TRUE") || secure_url;
            if (valid_expiry && domain_ok && path_ok && secure_ok && *fields[5]) {
                if (out.length) dbuf_puts(&out, "; ");
                dbuf_printf(&out, "%s=%s", fields[5], fields[6]);
            }
        }
        free(line);
    }
    free(host);
    free(path);
    return dbuf_take(&out);
}

static char *cookie_string_for_url(Engine *engine, const char *url)
{
    if (!is_http_url(url) || !engine->share) return d_strdup(engine->script_cookies);
    CURL *probe = curl_easy_init();
    if (!probe) return d_strdup(engine->script_cookies);
    configure_curl_common(engine, probe, NULL);
    struct curl_slist *list = NULL;
    CURLcode result = curl_easy_getinfo(probe, CURLINFO_COOKIELIST, &list);
    char *cookies = result == CURLE_OK ? cookie_string_from_list(list, url) : d_strdup("");
    curl_slist_free_all(list);
    curl_easy_cleanup(probe);
    if (!*cookies && engine->script_cookies && *engine->script_cookies) {
        free(cookies);
        cookies = d_strdup(engine->script_cookies);
    }
    return cookies;
}

static char *cookie_pair(const char *value)
{
    const char *end = strchr(value ? value : "", ';');
    char *raw = d_strndup(value ? value : "", end ? (size_t)(end - value) : strlen(value ? value : ""));
    char *pair = d_trim(raw);
    free(raw);
    const char *equals = strchr(pair, '=');
    if (!equals || equals == pair) {
        free(pair);
        return d_strdup("");
    }
    return pair;
}

static void remember_script_cookie(Engine *engine, const char *pair)
{
    const char *equals = strchr(pair, '=');
    if (!equals || equals == pair) return;
    size_t name_length = (size_t)(equals - pair);
    DBuffer out;
    dbuf_init(&out);
    const char *cursor = engine->script_cookies ? engine->script_cookies : "";
    bool replaced = false;
    while (*cursor) {
        const char *end = strchr(cursor, ';');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char *old = d_trim(d_strndup(cursor, length));
        const char *old_equals = strchr(old, '=');
        bool same = old_equals && (size_t)(old_equals - old) == name_length &&
                    strncmp(old, pair, name_length) == 0;
        if (!same && *old) {
            if (out.length) dbuf_puts(&out, "; ");
            dbuf_puts(&out, old);
        } else if (same) {
            replaced = true;
        }
        free(old);
        cursor = end ? end + 1 : cursor + length;
        if (!end) break;
    }
    if (!replaced && out.length) dbuf_puts(&out, "; ");
    dbuf_puts(&out, pair);
    free(engine->script_cookies);
    engine->script_cookies = dbuf_take(&out);
}

static void import_script_cookie(Engine *engine, const char *pair)
{
    if (!engine->share || !is_http_url(engine->source_url)) return;
    char *domain = url_cookie_domain(engine->source_url);
    DBuffer line;
    dbuf_init(&line);
    dbuf_printf(&line, "Set-Cookie: %s; Domain=%s; Path=/", pair, domain);
    if (d_starts(engine->source_url, "https://")) dbuf_puts(&line, "; Secure");
    CURL *easy = curl_easy_init();
    if (easy) {
        configure_curl_common(engine, easy, NULL);
        curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.data);
        curl_easy_cleanup(easy);
    }
    dbuf_free(&line);
    free(domain);
}

static JSValue cookie_get_callback(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    Engine *engine = JS_GetContextOpaque(ctx);
    char *cookies = cookie_string_for_url(engine, engine->source_url);
    JSValue result = js_str(ctx, cookies);
    free(cookies);
    return result;
}

static JSValue cookie_set_callback(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!argc) return JS_UNDEFINED;
    Engine *engine = JS_GetContextOpaque(ctx);
    char *value = js_string(ctx, argv[0]);
    char *pair = cookie_pair(value);
    if (*pair) {
        remember_script_cookie(engine, pair);
        import_script_cookie(engine, pair);
    }
    free(pair);
    free(value);
    return JS_UNDEFINED;
}

static size_t response_header_callback(void *ptr, size_t size, size_t count,
                                       void *opaque)
{
    Request *request = opaque;
    size_t length = size * count;
    const char *line = ptr;
    if (length >= 5 && strncmp(line, "HTTP/", 5) == 0) {
        header_vec_clear(&request->response_headers);
        return length;
    }
    if (!length || line[0] == '\r' || line[0] == '\n') return length;
    const char *colon = memchr(line, ':', length);
    if (!colon || colon == line) return length;
    char *name = d_trim(d_strndup(line, (size_t)(colon - line)));
    char *value = d_trim(d_strndup(colon + 1, length - (size_t)(colon - line) - 1));
    if (*name) {
        char *normalized = d_lower(name);
        header_vec_push(&request->response_headers, normalized, value);
        free(normalized);
    }
    free(name);
    free(value);
    return length;
}

static int curl_transfer_progress(void *opaque, curl_off_t download_total,
                                  curl_off_t download_now, curl_off_t upload_total,
                                  curl_off_t upload_now)
{
    (void)upload_total; (void)upload_now;
    Engine *engine = opaque;
    /* A server that declares no length gives nothing to divide by; the phase
       still says what is happening, which is the part that always holds. */
    if (download_total > 0 && download_now >= 0) {
        int span = PROGRESS_FETCH_END - PROGRESS_FETCH_START;
        int percent = PROGRESS_FETCH_START +
                      (int)((double)span * (double)download_now / (double)download_total);
        report_progress(engine, percent, "fetch");
    }
    return 0;
}

/* A connection that was never established. The server said nothing about the
   page because it never spoke at all, which is a different failure from one
   it answered with. */
static bool transport_never_opened(CURLcode result)
{
    switch (result) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
        return true;
    default:
        return false;
    }
}

static bool fetch_sync_request(Engine *engine, const char *url, DBuffer *body,
                               long *status, char **effective_url,
                               const char *request_method, const char *request_data,
                               CURLcode *transport)
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
    configure_curl_common(engine, easy, engine->source_url == url ? NULL : engine->source_url);
    bool document_request = d_eq(engine->source_url, url) &&
                            (!request_method || d_ieq(request_method, "GET"));
    /* Only the page itself reports bytes. A sub-resource fetched later is
       part of the script phase and has its own band. */
    if (document_request && engine->progress) {
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, curl_transfer_progress);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, engine);
    }
    struct curl_slist *headers = default_request_headers(
        document_request ? NULL : engine->source_url, NULL, document_request);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connection_timeout_ms(engine));
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
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    if (status) *status = code;
    if (transport) *transport = result;
    return result == CURLE_OK;
}

static bool fetch_sync(Engine *engine, const char *url, DBuffer *body,
                       long *status, char **effective_url)
{
    return fetch_sync_request(engine, url, body, status, effective_url, NULL, NULL, NULL);
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
    ++engine->script_errors;
    JSValue exception = JS_GetException(engine->context);
    char *message = js_string(engine->context, exception);
    fprintf(stderr, "script error [%s]: %s\n", where, message);
    JSValue stack_value = JS_GetPropertyStr(engine->context, exception, "stack");
    char *stack = js_string(engine->context, stack_value);
    if (*stack && !d_eq(stack, message)) fprintf(stderr, "script stack [%s]: %s\n", where, stack);
    free(stack);
    JS_FreeValue(engine->context, stack_value);
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
    curl_slist_free_all(request->headers);
    curl_slist_free_all(request->wire_headers);
    header_vec_free(&request->response_headers);
    free(request->url);
    free(request->method);
    free(request->request_data);
    free(request->effective_url);
    dbuf_free(&request->body);
    if (!JS_IsUndefined(request->resolve)) JS_FreeValue(engine->context, request->resolve);
    if (!JS_IsUndefined(request->reject)) JS_FreeValue(engine->context, request->reject);
    free(request);
}

static bool attach_request_handle(Engine *engine, Request *request)
{
    request->easy = curl_easy_init();
    if (!request->easy) return false;
    header_vec_clear(&request->response_headers);
    free(request->effective_url);
    request->effective_url = NULL;
    curl_easy_setopt(request->easy, CURLOPT_URL, request->url);
    curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->body);
    curl_easy_setopt(request->easy, CURLOPT_PRIVATE, request);
    configure_curl_common(engine, request->easy, engine->source_url);
    curl_easy_setopt(request->easy, CURLOPT_HEADERFUNCTION, response_header_callback);
    curl_easy_setopt(request->easy, CURLOPT_HEADERDATA, request);
    curl_slist_free_all(request->wire_headers);
    request->wire_headers = default_request_headers(engine->source_url, request->headers, false);
    curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->wire_headers);
    curl_easy_setopt(request->easy, CURLOPT_CONNECTTIMEOUT_MS, connection_timeout_ms(engine));
    if (request->method && !d_ieq(request->method, "GET")) {
        if (d_ieq(request->method, "POST")) {
            curl_easy_setopt(request->easy, CURLOPT_POST, 1L);
            curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_data ? request->request_data : "");
        } else {
            curl_easy_setopt(request->easy, CURLOPT_CUSTOMREQUEST, request->method);
            if (request->request_data) curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_data);
        }
    }
    if (curl_multi_add_handle(engine->multi, request->easy) != CURLM_OK) {
        curl_easy_cleanup(request->easy);
        request->easy = NULL;
        return false;
    }
    return true;
}

static Request *start_request(Engine *engine, RequestKind kind, const char *url,
                              const char *method, const char *request_data,
                              struct curl_slist *headers)
{
    Request *request = d_xcalloc(1, sizeof(*request));
    request->kind = kind;
    request->engine = engine;
    request->url = d_strdup(url);
    request->method = d_strdup(method && *method ? method : "GET");
    request->request_data = request_data ? d_strdup(request_data) : NULL;
    request->headers = headers;
    request->resolve = JS_UNDEFINED;
    request->reject = JS_UNDEFINED;
    dbuf_init(&request->body);
    if (!attach_request_handle(engine, request)) { free_request(engine, request); return NULL; }
    request_vec_push(&engine->requests, request);
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

static JSValue response_header_get(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    char *needle = js_string(ctx, argv[0]);
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    DBuffer value;
    dbuf_init(&value);
    bool found = false;
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue name_value = JS_GetPropertyStr(ctx, item, "name");
        JSValue item_value = JS_GetPropertyStr(ctx, item, "value");
        char *name = js_string(ctx, name_value);
        if (d_ieq(name, needle)) {
            char *text = js_string(ctx, item_value);
            if (found) dbuf_puts(&value, ", ");
            dbuf_puts(&value, text);
            found = true;
            free(text);
        }
        free(name);
        JS_FreeValue(ctx, name_value);
        JS_FreeValue(ctx, item_value);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    free(needle);
    if (!found) {
        dbuf_free(&value);
        return JS_NULL;
    }
    char *text = dbuf_take(&value);
    JSValue result = js_str(ctx, text);
    free(text);
    return result;
}

static JSValue response_header_has(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv)
{
    if (argc < 1) return JS_FALSE;
    char *needle = js_string(ctx, argv[0]);
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    bool found = false;
    for (int64_t i = 0; i < length && !found; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue name_value = JS_GetPropertyStr(ctx, item, "name");
        char *name = js_string(ctx, name_value);
        found = d_ieq(name, needle);
        free(name);
        JS_FreeValue(ctx, name_value);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    free(needle);
    return JS_NewBool(ctx, found);
}

static JSValue response_header_for_each(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue name_value = JS_GetPropertyStr(ctx, item, "name");
        JSValue item_value = JS_GetPropertyStr(ctx, item, "value");
        JSValue args[3] = {
            JS_DupValue(ctx, item_value),
            JS_DupValue(ctx, name_value),
            JS_DupValue(ctx, this_val)
        };
        JSValue result = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
        for (size_t j = 0; j < 3; ++j) JS_FreeValue(ctx, args[j]);
        JS_FreeValue(ctx, name_value);
        JS_FreeValue(ctx, item_value);
        JS_FreeValue(ctx, item);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, items);
            return result;
        }
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, items);
    return JS_UNDEFINED;
}

static JSValue response_header_entries(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    JSValue result = JS_NewArray(ctx);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue name = JS_GetPropertyStr(ctx, item, "name");
        JSValue value = JS_GetPropertyStr(ctx, item, "value");
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, name);
        JS_SetPropertyUint32(ctx, pair, 1, value);
        JS_SetPropertyUint32(ctx, result, (uint32_t)i, pair);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    return result;
}

static JSValue response_header_keys(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    JSValue result = JS_NewArray(ctx);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue name = JS_GetPropertyStr(ctx, item, "name");
        JS_SetPropertyUint32(ctx, result, (uint32_t)i, name);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    return result;
}

static JSValue response_header_values(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue items = JS_GetPropertyStr(ctx, this_val, "_items");
    int64_t length = json_array_length_for_context(ctx, items);
    JSValue result = JS_NewArray(ctx);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
        JSValue value = JS_GetPropertyStr(ctx, item, "value");
        JS_SetPropertyUint32(ctx, result, (uint32_t)i, value);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, items);
    return result;
}

static bool forbidden_request_header(const char *name)
{
    char *lower = d_lower(name);
    const char *forbidden[] = {
        "connection", "content-length", "cookie", "host", "origin",
        "referer", "user-agent", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "proxy-", "sec-"
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (forbidden[i][strlen(forbidden[i]) - 1] == '-') {
            if (d_starts(lower, forbidden[i])) { free(lower); return true; }
        } else if (d_ieq(lower, forbidden[i])) {
            free(lower);
            return true;
        }
    }
    free(lower);
    return false;
}

static void append_page_header(struct curl_slist **headers, const char *name,
                               const char *value)
{
    if (!name || !*name || !value || forbidden_request_header(name) ||
        strpbrk(name, "\r\n") || strpbrk(value, "\r\n")) return;
    DBuffer line;
    dbuf_init(&line);
    dbuf_printf(&line, "%s: %s", name, value);
    struct curl_slist *updated = curl_slist_append(*headers, line.data);
    dbuf_free(&line);
    if (updated) *headers = updated;
}

static struct curl_slist *page_headers_from_js(JSContext *ctx, JSValueConst value)
{
    const char *source =
        "(function(h){if(h==null)return [];"
        "if(Array.isArray(h))return h.map(p=>[p[0],p[1]]);"
        "if(typeof h.forEach==='function'){var a=[];h.forEach((v,k)=>a.push([k,v]));return a;}"
        "return Object.keys(h).map(k=>[k,h[k]]);})";
    JSValue converter = JS_Eval(ctx, source, strlen(source), "distill-fetch-headers.js",
                                JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(converter)) {
        JS_FreeValue(ctx, converter);
        return NULL;
    }
    JSValue pairs = JS_Call(ctx, converter, JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, converter);
    if (JS_IsException(pairs)) {
        JS_FreeValue(ctx, pairs);
        return NULL;
    }
    int64_t length = 0;
    JS_GetLength(ctx, pairs, &length);
    struct curl_slist *headers = NULL;
    for (int64_t i = 0; i < length; ++i) {
        JSValue pair = JS_GetPropertyUint32(ctx, pairs, (uint32_t)i);
        JSValue name_value = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue header_value = JS_GetPropertyUint32(ctx, pair, 1);
        char *name = js_string(ctx, name_value);
        char *text = js_string(ctx, header_value);
        append_page_header(&headers, name, text);
        free(name); free(text);
        JS_FreeValue(ctx, name_value);
        JS_FreeValue(ctx, header_value);
        JS_FreeValue(ctx, pair);
    }
    JS_FreeValue(ctx, pairs);
    return headers;
}

static JSValue make_response(Engine *engine, Request *request)
{
    JSContext *ctx = engine->context;
    JSValue response = JS_NewObject(ctx);
    const char *response_url = request->effective_url && *request->effective_url
                                   ? request->effective_url : request->url;
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, request->status >= 200 && request->status < 300));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, (int32_t)request->status));
    JS_SetPropertyStr(ctx, response, "url", js_str(ctx, response_url));
    JS_SetPropertyStr(ctx, response, "_body", js_strn(ctx, request->body.data, request->body.length));
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, response_json, "json", 0));
    JS_SetPropertyStr(ctx, response, "redirected", JS_NewBool(ctx, !d_eq(response_url, request->url)));
    JS_SetPropertyStr(ctx, response, "type", js_str(ctx, "basic"));
    JSValue headers = JS_NewObject(ctx);
    JSValue items = JS_NewArray(ctx);
    for (size_t i = 0; i < request->response_headers.length; ++i) {
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "name", js_str(ctx, request->response_headers.items[i].name));
        JS_SetPropertyStr(ctx, item, "value", js_str(ctx, request->response_headers.items[i].value));
        JS_SetPropertyUint32(ctx, items, (uint32_t)i, item);
    }
    JS_SetPropertyStr(ctx, headers, "_items", items);
    JS_SetPropertyStr(ctx, headers, "get", JS_NewCFunction(ctx, response_header_get, "get", 1));
    JS_SetPropertyStr(ctx, headers, "has", JS_NewCFunction(ctx, response_header_has, "has", 1));
    JS_SetPropertyStr(ctx, headers, "forEach", JS_NewCFunction(ctx, response_header_for_each, "forEach", 1));
    JS_SetPropertyStr(ctx, headers, "entries", JS_NewCFunction(ctx, response_header_entries, "entries", 0));
    JS_SetPropertyStr(ctx, headers, "keys", JS_NewCFunction(ctx, response_header_keys, "keys", 0));
    JS_SetPropertyStr(ctx, headers, "values", JS_NewCFunction(ctx, response_header_values, "values", 0));
    JS_SetPropertyStr(ctx, response, "headers", headers);
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
    char *method = d_strdup("GET");
    char *request_data = NULL;
    struct curl_slist *headers = NULL;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue method_value = JS_GetPropertyStr(ctx, argv[1], "method");
        JSValue body_value = JS_GetPropertyStr(ctx, argv[1], "body");
        JSValue headers_value = JS_GetPropertyStr(ctx, argv[1], "headers");
        free(method);
        method = (JS_IsUndefined(method_value) || JS_IsNull(method_value))
                     ? d_strdup("GET") : js_string(ctx, method_value);
        if (!*method) { free(method); method = d_strdup("GET"); }
        if (!JS_IsUndefined(body_value) && !JS_IsNull(body_value)) request_data = js_string(ctx, body_value);
        if (!JS_IsUndefined(headers_value) && !JS_IsNull(headers_value)) {
            headers = page_headers_from_js(ctx, headers_value);
        }
        JS_FreeValue(ctx, method_value);
        JS_FreeValue(ctx, body_value);
        JS_FreeValue(ctx, headers_value);
    }
    Request *request = start_request(engine, REQUEST_FETCH, url, method, request_data, headers);
    free(method);
    free(request_data);
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
        if (JS_IsObject(argv[i])) {
            JSValue stack_value = JS_GetPropertyStr(ctx, argv[i], "stack");
            char *stack = js_string(ctx, stack_value);
            if (*stack) fprintf(stderr, "\n  stack: %s", stack);
            free(stack);
            JS_FreeValue(ctx, stack_value);
        }
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
        char *effective_url = NULL;
        curl_easy_getinfo(message->easy_handle, CURLINFO_EFFECTIVE_URL, &effective_url);
        free(request->effective_url);
        request->effective_url = effective_url ? d_strdup(effective_url) : d_strdup(request->url);
        curl_multi_remove_handle(engine->multi, message->easy_handle);
        CURLcode result = message->data.result;
        curl_easy_cleanup(request->easy);
        request->easy = NULL;
        if ((result != CURLE_OK || request->status == 0) && request->attempts < 2) {
            ++request->attempts;
            dbuf_free(&request->body);
            dbuf_init(&request->body);
            request->status = 0;
            if (attach_request_handle(engine, request)) continue;
        }
        request->completed = true;
        finish_request(engine, request);
    }
}

/* A module specifier resolves against the URL of the module that imported
   it, not against the page. A bundle at /assets/app.js importing
   './chunk.js' must reach /assets/chunk.js; resolving against the page URL
   instead sends the request to the site's HTML fallback, which then fails to
   compile with a bare syntax error at the first '<'. The base name is only
   usable when it is a real URL: top-level inline modules are evaluated under
   a placeholder name and still resolve against the document base. */
static char *module_normalize(JSContext *ctx, const char *base_name,
                              const char *name, void *opaque)
{
    Engine *engine = opaque;
    const char *base = (base_name && is_http_url(base_name)) ? base_name
                                                             : engine->base_url;
    char *url = resolve_url(base, name);
    char *out = js_malloc(ctx, strlen(url) + 1);
    if (out) strcpy(out, url);
    free(url);
    return out;
}

/* Servers answer an unknown path with an HTML error page or a single-page
   application fallback. Compiling that as JavaScript reports only
   "unexpected token '<'", which hides the real cause, so the condition is
   detected and reported by name instead. */
static bool looks_like_html(const char *data, size_t length)
{
    static const char *marks[] = {"<!doctype", "<html", "<?xml", "<head", "<body"};
    size_t i = 0;
    while (i < length && (unsigned char)data[i] <= ' ') ++i;
    if (i >= length || data[i] != '<') return false;
    for (size_t m = 0; m < sizeof(marks) / sizeof(*marks); ++m) {
        size_t n = strlen(marks[m]);
        if (length - i < n) continue;
        size_t k = 0;
        while (k < n) {
            char c = data[i + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != marks[m][k]) break;
            ++k;
        }
        if (k == n) return true;
    }
    return false;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name, void *opaque)
{
    Engine *engine = opaque;
    DBuffer body;
    long status = 0;
    if (!fetch_sync(engine, name, &body, &status, NULL)) {
        dbuf_free(&body);
        JS_ThrowReferenceError(ctx, "module not found: %s", name);
        return NULL;
    }
    if (status >= 400) {
        dbuf_free(&body);
        JS_ThrowReferenceError(ctx, "module request failed with HTTP %d: %s",
                               (int)status, name);
        return NULL;
    }
    if (looks_like_html(body.data, body.length)) {
        dbuf_free(&body);
        JS_ThrowReferenceError(ctx, "module request returned HTML, not JavaScript: %s",
                               name);
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
"__setWindow('self', window); globalThis.self = window;"
"__setWindow('innerWidth', 1280); __setWindow('innerHeight', 1024);"
"__setWindow('setTimeout', setTimeout); __setWindow('clearTimeout', clearTimeout); __setWindow('setInterval', setInterval); __setWindow('clearInterval', clearInterval);"
"globalThis.requestIdleCallback = callback => setTimeout(() => callback({didTimeout:false, timeRemaining:() => 50}), 1); globalThis.cancelIdleCallback = id => clearTimeout(id); __setWindow('requestIdleCallback', requestIdleCallback); __setWindow('cancelIdleCallback', cancelIdleCallback);"
"__setWindow('requestAnimationFrame', requestAnimationFrame); __setWindow('fetch', fetch); __setWindow('console', console);"
"if (typeof requestAnimationFrame !== 'function') globalThis.requestAnimationFrame = callback => setTimeout(callback, 16); __setWindow('requestAnimationFrame', requestAnimationFrame);"
"__setWindow('scrollTo', () => {}); __setWindow('scrollBy', () => {});"
"Object.defineProperty(document, 'addEventListener', {value:(type, callback) => __add('document', type, callback), configurable:true});"
"Object.defineProperty(document, 'removeEventListener', {value:() => {}, configurable:true});"
"Object.defineProperty(document, 'dispatchEvent', {value:event => __dispatch('document', event.type), configurable:true});"
"document.readyState = 'loading';"
"document.visibilityState = 'visible';"
"Object.defineProperty(document, 'cookie', {get() { return __distillCookieGet(); }, set(value) { __distillCookieSet(String(value)); }, configurable:true});"
"globalThis.localStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};"
"Object.defineProperty(localStorage, 'length', {get() { return Object.keys(this.__data).length; }});"
"globalThis.sessionStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};"
"Object.defineProperty(sessionStorage, 'length', {get() { return Object.keys(this.__data).length; }});"
"__setWindow('localStorage', localStorage); __setWindow('sessionStorage', sessionStorage);"
"globalThis.navigator = window.navigator || {userAgent: '" DISTILL_USER_AGENT "', language: 'en-US', platform: 'Linux'};"
"Object.defineProperty(navigator, 'userAgent', {value:'" DISTILL_USER_AGENT "', configurable:true}); if (!navigator.language) navigator.language = 'en-US'; __setWindow('navigator', navigator);"
"globalThis.screen = {width: 1280, height: 1024}; __setWindow('screen', screen);"
"globalThis.visualViewport = {width:1280, height:1024, offsetLeft:0, offsetTop:0, pageLeft:0, pageTop:0, scale:1, addEventListener() {}, removeEventListener() {}}; __setWindow('visualViewport', visualViewport);"
"globalThis.crypto = {getRandomValues(a) { for (let i=0;i<a.length;i++) a[i] = (i * 73 + 41) & 255; return a; }}; __setWindow('crypto', crypto);"
"globalThis.CSS = {escape(value) { return String(value).replace(/[^a-zA-Z0-9_-]/g, ch => '\\\\' + ch); }, supports() { return false; }}; __setWindow('CSS', CSS);"
"globalThis.AbortSignal = class { constructor() { this.aborted=false; this.reason=undefined; this.__listeners=[]; } addEventListener(type, callback) { if (type === 'abort' && typeof callback === 'function') this.__listeners.push(callback); } removeEventListener(type, callback) { if (type === 'abort') this.__listeners=this.__listeners.filter(item => item !== callback); } dispatchEvent(event) { for (const callback of this.__listeners.slice()) callback.call(this, event); return true; } throwIfAborted() { if (this.aborted) throw this.reason || new Error('The operation was aborted'); } };"
"globalThis.AbortController = class { constructor() { this.signal=new AbortSignal(); } abort(reason) { if (this.signal.aborted) return; this.signal.aborted=true; this.signal.reason=reason || new Error('The operation was aborted'); this.signal.dispatchEvent({type:'abort', target:this.signal}); } }; __setWindow('AbortSignal', AbortSignal); __setWindow('AbortController', AbortController);"
"globalThis.CustomEvent = class extends Event { constructor(type, init={}) { super(type, init); this.detail=init.detail; } }; __setWindow('CustomEvent', CustomEvent);"
"globalThis.Intl = { getCanonicalLocales(locales) { if (locales == null) return []; return (Array.isArray(locales) ? locales : [locales]).map(String); }, Locale: class { constructor(locale) { this.baseName=String(locale || 'en-US'); this.language=this.baseName.split('-')[0]; } toString() { return this.baseName; } maximize() { return this; } }, NumberFormat: class { constructor(locales, options) { this.locales=locales; this.options=options || {}; } format(value) { return String(value); } formatToParts(value) { return [{type:'integer', value:String(value)}]; } resolvedOptions() { return {locale:'en-US', numberingSystem:'latn'}; } }, DateTimeFormat: class { constructor(locales, options) { this.locales=locales; this.options=options || {}; } format(value) { return new Date(value == null ? Date.now() : value).toISOString(); } formatToParts(value) { return [{type:'literal', value:this.format(value)}]; } resolvedOptions() { return {locale:'en-US', calendar:'gregory', numberingSystem:'latn', timeZone:'UTC'}; } }, PluralRules: class { select(value) { return Number(value) === 1 ? 'one' : 'other'; } resolvedOptions() { return {locale:'en-US', type:'cardinal', pluralCategories:['one','other']}; } }, RelativeTimeFormat: class { format(value, unit) { return String(value) + ' ' + String(unit); } resolvedOptions() { return {locale:'en-US', numeric:'always', style:'long'}; } }, ListFormat: class { format(values) { return Array.from(values || []).join(', '); } }, Collator: class { compare(a, b) { return String(a).localeCompare(String(b)); } }, Segmenter: class { segment(value) { return Array.from(String(value), segment => ({segment})); } }, DisplayNames: class { of(value) { return String(value); } } }; __setWindow('Intl', Intl);"
"globalThis.matchMedia = q => ({matches: false, media: q, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}}); __setWindow('matchMedia', matchMedia);"
"globalThis.performance = {now: () => Date.now(), mark() {}, measure() {}, getEntries() { return []; }, getEntriesByName() { return []; }, getEntriesByType() { return []; }, clearMarks() {}, clearMeasures() {}}; __setWindow('performance', performance);"
"globalThis.PerformanceObserver = class { constructor(callback) { this.callback=callback; } observe() {} disconnect() {} takeRecords() { return []; } }; PerformanceObserver.supportedEntryTypes=[]; __setWindow('PerformanceObserver', PerformanceObserver);"
"globalThis.NodeFilter = {SHOW_ALL:4294967295, SHOW_ELEMENT:1, SHOW_TEXT:4, SHOW_COMMENT:128, FILTER_ACCEPT:1, FILTER_REJECT:2, FILTER_SKIP:3}; __setWindow('NodeFilter', NodeFilter);"
"globalThis.history = {pushState(s,t,u) { const suffix = String(u); location.search = suffix[0] === '?' ? suffix : ''; location.href = location.href.split('?')[0] + location.search; }, replaceState(s,t,u) { this.pushState(s,t,u); }}; __setWindow('history', history);"
"if (typeof Element !== 'undefined') Element.prototype.getBoundingClientRect = function() { return {x:0,y:0,top:0,left:0,width:800,height:20,right:800,bottom:20}; };"
"if (typeof Element !== 'undefined') { Object.defineProperty(Element.prototype, 'offsetHeight', {get() { return 20; }}); Object.defineProperty(Element.prototype, 'offsetWidth', {get() { return 800; }}); }"
"if (document.createRange) { const __createRange=document.createRange.bind(document); document.createRange=() => { const range=__createRange(); range.getBoundingClientRect=() => ({x:0,y:0,top:0,left:0,width:800,height:20,right:800,bottom:20}); range.getClientRects=() => [range.getBoundingClientRect()]; return range; }; }"
"globalThis.__resolveURL = function(value, baseValue) { const raw = String(value || ''); const fallback = (document.querySelector('base') && document.querySelector('base').getAttribute('href')) || location.href; const base = String(baseValue || fallback); const schemeAt = base.indexOf('://'); const baseOrigin = schemeAt < 0 ? '' : base.slice(0, schemeAt + 3) + base.slice(schemeAt + 3).split(/[/?#]/)[0]; if (/^[a-z]+:/i.test(raw)) return raw; if (raw.startsWith('//')) return (location.protocol || 'https:') + raw; if (raw.startsWith('/')) return baseOrigin + raw; const basePathEnd = base.search(/[?#]/); const basePath = basePathEnd < 0 ? base : base.slice(0, basePathEnd); const prefix = basePath.endsWith('/') ? basePath : basePath.slice(0, basePath.lastIndexOf('/') + 1); const pieces = (prefix + raw).split('/'); const clean = []; for (const piece of pieces) { if (!piece || piece === '.') continue; if (piece === '..') clean.pop(); else clean.push(piece); } return baseOrigin + '/' + clean.join('/'); };"
"globalThis.URLSearchParams = class { constructor(value='') { this.__pairs=[]; const source=String(value).replace(/^\\?/, ''); for (const part of source ? source.split('&') : []) { const at=part.indexOf('='); const key=decodeURIComponent((at < 0 ? part : part.slice(0, at)).replace(/\\+/g, ' ')); const val=decodeURIComponent((at < 0 ? '' : part.slice(at + 1)).replace(/\\+/g, ' ')); this.__pairs.push([key, val]); } } append(key, value) { this.__pairs.push([String(key), String(value)]); } set(key, value) { this.delete(key); this.append(key, value); } get(key) { const item=this.__pairs.find(pair => pair[0] === String(key)); return item ? item[1] : null; } getAll(key) { return this.__pairs.filter(pair => pair[0] === String(key)).map(pair => pair[1]); } has(key) { return this.__pairs.some(pair => pair[0] === String(key)); } delete(key) { this.__pairs=this.__pairs.filter(pair => pair[0] !== String(key)); } forEach(callback, thisArg) { this.__pairs.forEach(pair => callback.call(thisArg, pair[1], pair[0], this)); } entries() { return this.__pairs[Symbol.iterator](); } keys() { return this.__pairs.map(pair => pair[0])[Symbol.iterator](); } values() { return this.__pairs.map(pair => pair[1])[Symbol.iterator](); } toString() { return this.__pairs.map(pair => encodeURIComponent(pair[0]) + '=' + encodeURIComponent(pair[1])).join('&'); } [Symbol.iterator]() { return this.entries(); } };"
"globalThis.Headers = class { constructor(init) { this.__items=Object.create(null); if (init) { if (typeof init.forEach === 'function') init.forEach((v,k) => this.append(k,v)); else if (Array.isArray(init)) init.forEach(pair => this.append(pair[0], pair[1])); else Object.keys(init).forEach(k => this.append(k, init[k])); } } append(name,value) { const key=String(name).toLowerCase(); this.__items[key]=this.__items[key] ? this.__items[key]+', '+String(value) : String(value); } set(name,value) { this.__items[String(name).toLowerCase()]=String(value); } get(name) { const value=this.__items[String(name).toLowerCase()]; return value === undefined ? null : value; } has(name) { return Object.prototype.hasOwnProperty.call(this.__items, String(name).toLowerCase()); } delete(name) { delete this.__items[String(name).toLowerCase()]; } forEach(callback,thisArg) { Object.keys(this.__items).forEach(k => callback.call(thisArg,this.__items[k],k,this)); } entries() { return Object.keys(this.__items).map(k => [k,this.__items[k]]); } keys() { return Object.keys(this.__items); } values() { return Object.keys(this.__items).map(k => this.__items[k]); } [Symbol.iterator]() { return this.entries()[Symbol.iterator](); } }; __setWindow('Headers', Headers);"
"globalThis.URL = class { constructor(value, base) { const href=__resolveURL(value, base); const scheme=href.indexOf('://'); const authorityStart=scheme < 0 ? 0 : scheme + 3; const tail=href.slice(authorityStart); const authorityEnd=tail.search(/[/?#]/); const authority=authorityEnd < 0 ? tail : tail.slice(0, authorityEnd); const rest=authorityEnd < 0 ? '' : tail.slice(authorityEnd); const pathEnd=rest.search(/[?#]/); this.protocol=scheme < 0 ? '' : href.slice(0, scheme + 1); this.origin=scheme < 0 ? '' : href.slice(0, authorityStart) + authority; this.host=authority; this.hostname=authority.replace(/^.*@/, '').replace(/:\\d+$/, ''); this.port=(this.hostname === authority ? '' : authority.slice(authority.lastIndexOf(':') + 1)); this.pathname=pathEnd < 0 ? (rest || '/') : (rest.slice(0, pathEnd) || '/'); const query=rest.indexOf('?'); const hash=rest.indexOf('#'); this.search=query < 0 ? '' : rest.slice(query, hash < 0 ? rest.length : hash); this.hash=hash < 0 ? '' : rest.slice(hash); this.username=''; this.password=''; this.href=this.origin + this.pathname + this.search + this.hash; this.searchParams=new URLSearchParams(this.search); } toString() { return this.href; } toJSON() { return this.href; } static canParse(value, base) { try { new URL(value, base); return true; } catch (e) { return false; } } }; globalThis.URL.createObjectURL = () => 'blob:distill'; globalThis.URL.revokeObjectURL = () => {}; __setWindow('URL', URL); __setWindow('URLSearchParams', URLSearchParams);"
"globalThis.__anchorPart = function(node, part) { const href = __resolveURL(node.getAttribute('href') || ''); const scheme = href.indexOf('://'); if (scheme < 0) return part === 'href' ? href : ''; const authority = scheme + 3; const tail = href.slice(authority); const end = tail.search(/[/?#]/); const host = end < 0 ? tail : tail.slice(0, end); const rest = end < 0 ? '' : tail.slice(end); const pathEnd = rest.search(/[?#]/); const pathname = pathEnd < 0 ? (rest || '/') : (rest.slice(0, pathEnd) || '/'); const query = rest.indexOf('?'); const hash = rest.indexOf('#'); const search = query < 0 ? '' : rest.slice(query, hash < 0 ? rest.length : hash); const fragment = hash < 0 ? '' : rest.slice(hash); const portAt = host.lastIndexOf(':'); const hasPort = portAt > -1 && host.indexOf(']') < portAt; const hostname = hasPort ? host.slice(0, portAt) : host; const port = hasPort ? host.slice(portAt + 1) : ''; if (part === 'href') return href; if (part === 'protocol') return href.slice(0, scheme + 1); if (part === 'host') return host; if (part === 'hostname') return hostname; if (part === 'port') return port; if (part === 'pathname') return pathname; if (part === 'search') return search; if (part === 'hash') return fragment; return ''; };"
"try { Object.defineProperty(HTMLAnchorElement.prototype, 'href', {get() { return __anchorPart(this, 'href'); }, set(value) { this.setAttribute('href', value); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'protocol', {get() { return __anchorPart(this, 'protocol'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'host', {get() { return __anchorPart(this, 'host'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'hostname', {get() { return __anchorPart(this, 'hostname'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'port', {get() { return __anchorPart(this, 'port'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'pathname', {get() { return __anchorPart(this, 'pathname'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'search', {get() { return __anchorPart(this, 'search'); }, configurable:true}); Object.defineProperty(HTMLAnchorElement.prototype, 'hash', {get() { return __anchorPart(this, 'hash'); }, configurable:true}); } catch (e) {}"
"try { Object.defineProperty(HTMLImageElement.prototype, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}"
"try { Object.defineProperty(HTMLFormElement.prototype, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('a')) try { Object.defineProperty(node, 'href', {get() { return __resolveURL(this.getAttribute('href')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('img')) try { Object.defineProperty(node, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}"
"for (const node of document.querySelectorAll('form')) try { Object.defineProperty(node, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}"
"globalThis.getComputedStyle = node => ({display: node.id === 'genuinely-hidden' ? 'none' : ((node.getAttribute && node.getAttribute('hidden') !== null) ? 'none' : 'block'), visibility: 'visible', marginLeft: '0px', marginRight: '0px', getPropertyValue(name) { return this[name] || ''; }}); __setWindow('getComputedStyle', getComputedStyle);"
"globalThis.IntersectionObserver = class { constructor(callback) { this.callback = callback; } observe(target) { this.callback([{target, isIntersecting:true, intersectionRatio:1, intersectionRect:{top:0,left:0,width:1280,height:1024,right:1280,bottom:1024}}], this); } unobserve() {} disconnect() {} };"
"globalThis.ResizeObserver = class { constructor(callback) { this.callback=callback; } observe(target) { this.callback([{target, contentRect:{x:0,y:0,top:0,left:0,width:800,height:20,right:800,bottom:20}, borderBoxSize:[{inlineSize:800,blockSize:20},], contentBoxSize:[{inlineSize:800,blockSize:20}]}], this); } unobserve() {} disconnect() {} }; __setWindow('ResizeObserver', ResizeObserver);"
"globalThis.MutationObserver = class { constructor(callback) { this.callback = callback; } observe() { this.callback([{}], this); } disconnect() {} };"
"globalThis.XMLHttpRequest = class { constructor() { this.readyState=0; this.status=0; this.responseText=''; this.responseURL=''; this.onreadystatechange=null; this.onload=null; this.onerror=null; this._headers=Object.create(null); this._responseHeaders=Object.create(null); } open(method,url,async=true) { this._method=method || 'GET'; this._url=url; this._async=async; this.readyState=1; } setRequestHeader(name,value) { const key=String(name).toLowerCase(); this._headers[key]=this._headers[key] ? this._headers[key]+', '+String(value) : String(value); } getResponseHeader(name) { return this._responseHeaders[String(name).toLowerCase()] || null; } getAllResponseHeaders() { return Object.keys(this._responseHeaders).map(k=>k+': '+this._responseHeaders[k]).join('\\r\\n'); } send(body) { fetch(this._url,{method:this._method,body,headers:this._headers}).then(r => { this.status=r.status; this.responseURL=r.url || this._url; r.headers.forEach((v,k)=>{this._responseHeaders[k.toLowerCase()]=v;}); return r.text(); }).then(t => { this.responseText=t; this.readyState=4; if (this.onreadystatechange) this.onreadystatechange(); if (this.onload) this.onload(); }).catch(error => { if (this.onerror) this.onerror(error); }); } };"
"__setWindow('XMLHttpRequest', XMLHttpRequest); __setWindow('IntersectionObserver', IntersectionObserver); __setWindow('MutationObserver', MutationObserver);"
/* Audio is a no-op that remembers what it was handed. A page whose player is
   built in script would otherwise throw mid-settle and take the rest of the
   page down with it, so this is content fidelity first; the recorded URLs are
   reported as diagnostics, and never become media references -- the reader
   plays declarative sources only. See src/documents/MEDIA_PLAYBACK_PLAN.md. */
"globalThis.__distillScriptAudio = [];"
"globalThis.Audio = class { constructor(src) { this.src = src == null ? '' : String(src); if (this.src) __distillScriptAudio.push(this.src); this.currentTime=0; this.duration=NaN; this.paused=true; this.ended=false; this.volume=1; this.muted=false; this.loop=false; this.autoplay=false; this.preload='none'; this.readyState=0; this.networkState=0; this.crossOrigin=null; this.error=null; this.__listeners=Object.create(null); } play() { this.paused=false; return Promise.resolve(); } pause() { this.paused=true; } load() {} canPlayType() { return ''; } addEventListener(type, callback) { if (typeof callback !== 'function') return; (this.__listeners[type] = this.__listeners[type] || []).push(callback); } removeEventListener(type, callback) { const list=this.__listeners[type]; if (list) this.__listeners[type]=list.filter(item => item !== callback); } dispatchEvent(event) { const list=this.__listeners[event && event.type]; if (list) for (const callback of list.slice()) callback.call(this, event); return true; } }; __setWindow('Audio', Audio);"
/* The same courtesy for declarative media elements: linkedom parses <audio>
   and <video> but gives them no playback methods, and a script calling play()
   on one is ordinary. Patch the prototypes where they exist, the nodes the
   document already has, and anything created later. */
"globalThis.__distillMediaMethods = function(node) { if (!node || typeof node.play === 'function') return node; node.play = function() { return Promise.resolve(); }; node.pause = function() {}; node.load = function() {}; node.canPlayType = function() { return ''; }; return node; };"
"for (const name of ['HTMLMediaElement','HTMLAudioElement','HTMLVideoElement']) try { const type=globalThis[name]; if (type && type.prototype) __distillMediaMethods(type.prototype); } catch (e) {}"
"try { for (const node of document.querySelectorAll('audio,video')) __distillMediaMethods(node); } catch (e) {}"
"if (document.createElement) { const __createElement=document.createElement.bind(document); document.createElement=function(name, options) { const node=__createElement(name, options); const tag=String(name || '').toLowerCase(); if (tag === 'audio' || tag === 'video') __distillMediaMethods(node); return node; }; }"
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

static void process_async_scripts(Engine *engine)
{
    for (;;) {
        Request *next = NULL;
        for (size_t i = 0; i < engine->requests.length; ++i) {
            Request *request = engine->requests.items[i];
            if (request->kind == REQUEST_ASYNC_SCRIPT && !request->async_evaluated &&
                request->async_order == engine->async_next_order) {
                next = request;
                break;
            }
        }
        if (!next || !next->completed) return;
        next->async_evaluated = true;
        ++engine->async_next_order;
        if (next->body.length && next->body.data) {
            int flags = next->module_script ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
            evaluate(engine, next->body.data, next->body.length, next->url, flags);
        }
    }
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
    /* In a browser Window is also the JavaScript global object. Keeping a
       separate object makes globals installed by bundles invisible through
       window/self, which breaks common application bootstraps. */
    engine->window = JS_DupValue(engine->context, engine->global);
    const char *constructors[] = {
        "Element", "Node", "Event", "HTMLElement", "HTMLDocument",
        "HTMLAnchorElement", "HTMLImageElement", "HTMLFormElement", "HTMLIFrameElement"
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
    JS_SetPropertyStr(ctx, engine->document, "location", JS_GetPropertyStr(ctx, engine->global, "location"));
    JS_SetPropertyStr(ctx, engine->global, "fetch", JS_NewCFunction(ctx, fetch_callback, "fetch", 1));
    JS_SetPropertyStr(ctx, engine->global, "__distillCookieGet",
                      JS_NewCFunction(ctx, cookie_get_callback, "__distillCookieGet", 0));
    JS_SetPropertyStr(ctx, engine->global, "__distillCookieSet",
                      JS_NewCFunction(ctx, cookie_set_callback, "__distillCookieSet", 1));
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
        "Array.from(document.querySelectorAll('script')).map(s => ({node:s, src:s.getAttribute('src') || '', async:s.hasAttribute('async'), defer:s.hasAttribute('defer'), nomodule:s.hasAttribute('nomodule'), text:s.textContent || '', type:s.getAttribute('type') || ''}))";
    JSValue scripts = JS_Eval(engine->context, source, strlen(source), "distill-scripts.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(scripts)) { note_exception(engine, "script discovery"); return false; }
    JSValue deferred = JS_NewArray(engine->context);
    uint32_t deferred_count = 0;
    int64_t length = 0;
    JS_GetLength(engine->context, scripts, &length);
    for (int64_t i = 0; i < length; ++i) {
        JSValue spec = JS_GetPropertyUint32(engine->context, scripts, (uint32_t)i);
        JSValue node = JS_GetPropertyStr(engine->context, spec, "node");
        JS_SetPropertyStr(engine->context, node, "__distillProcessed", JS_NewBool(engine->context, true));
        JSValue src_value = JS_GetPropertyStr(engine->context, spec, "src");
        JSValue async_value = JS_GetPropertyStr(engine->context, spec, "async");
        JSValue defer_value = JS_GetPropertyStr(engine->context, spec, "defer");
        JSValue text_value = JS_GetPropertyStr(engine->context, spec, "text");
        JSValue type_value = JS_GetPropertyStr(engine->context, spec, "type");
        JSValue nomodule_value = JS_GetPropertyStr(engine->context, spec, "nomodule");
        char *src = js_string(engine->context, src_value);
        char *code = js_string(engine->context, text_value);
        char *type = js_string(engine->context, type_value);
        bool is_async = JS_ToBool(engine->context, async_value);
        bool is_defer = JS_ToBool(engine->context, defer_value);
        bool is_nomodule = JS_ToBool(engine->context, nomodule_value);
        JS_FreeValue(engine->context, node); JS_FreeValue(engine->context, src_value); JS_FreeValue(engine->context, async_value);
        JS_FreeValue(engine->context, defer_value); JS_FreeValue(engine->context, text_value);
        JS_FreeValue(engine->context, type_value); JS_FreeValue(engine->context, nomodule_value);
        JS_FreeValue(engine->context, spec);
        if (d_contains(type, "json")) { free(src); free(code); free(type); continue; }
        /* A page that ships a module bundle and a nomodule fallback expects a
           module-capable client to run exactly one of them. Running both
           initialises the same framework twice. */
        if (is_nomodule) { free(src); free(code); free(type); continue; }
        char *url = src[0] ? resolve_url(engine->base_url, src) : NULL;
        if (is_async && url) {
            ++engine->async_pending;
            Request *request = start_request(engine, REQUEST_ASYNC_SCRIPT, url, "GET", NULL, NULL);
            if (!request) {
                --engine->async_pending;
            } else {
                request->module_script = d_eq(type, "module") || d_eq(type, "text/javascript;module");
                request->async_order = engine->async_sequence++;
            }
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

    for (size_t pass = 0; pass < 64; ++pass) {
        const char *dynamic_source =
            "Array.from(document.querySelectorAll('script')).filter(s => !s.__distillProcessed).map(s => ({node:s, src:s.getAttribute('src') || '', text:s.textContent || '', type:s.getAttribute('type') || ''}))";
        JSValue dynamic = JS_Eval(engine->context, dynamic_source, strlen(dynamic_source),
                                  "distill-dynamic-scripts.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(dynamic)) {
            note_exception(engine, "dynamic script discovery");
            JS_FreeValue(engine->context, dynamic);
            return false;
        }
        int64_t dynamic_length = 0;
        JS_GetLength(engine->context, dynamic, &dynamic_length);
        if (!dynamic_length) {
            JS_FreeValue(engine->context, dynamic);
            break;
        }
        for (int64_t i = 0; i < dynamic_length; ++i) {
            JSValue spec = JS_GetPropertyUint32(engine->context, dynamic, (uint32_t)i);
            JSValue node = JS_GetPropertyStr(engine->context, spec, "node");
            JS_SetPropertyStr(engine->context, node, "__distillProcessed", JS_NewBool(engine->context, true));
            JSValue src_value = JS_GetPropertyStr(engine->context, spec, "src");
            JSValue text_value = JS_GetPropertyStr(engine->context, spec, "text");
            JSValue type_value = JS_GetPropertyStr(engine->context, spec, "type");
            char *src = js_string(engine->context, src_value);
            char *code = js_string(engine->context, text_value);
            char *type = js_string(engine->context, type_value);
            JS_FreeValue(engine->context, src_value);
            JS_FreeValue(engine->context, text_value); JS_FreeValue(engine->context, type_value);
            JS_FreeValue(engine->context, spec);
            char *url = src[0] ? resolve_url(engine->base_url, src) : NULL;
            if (url) {
                DBuffer body; long status = 0;
                if (fetch_sync(engine, url, &body, &status, NULL)) {
                    free(code); code = d_strndup(body.data, body.length); dbuf_free(&body);
                }
            }
            bool ok = true;
            if (!d_contains(type, "json") && *code) {
                int flags = d_eq(type, "module") || d_eq(type, "text/javascript;module")
                    ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
                ok = evaluate(engine, code, strlen(code), url ? url : "distill-dynamic.js", flags);
            }
            JSValue onload = JS_GetPropertyStr(engine->context, node, "onload");
            if (ok && JS_IsFunction(engine->context, onload)) {
                JSValue result = JS_Call(engine->context, onload, node, 0, NULL);
                if (JS_IsException(result)) note_exception(engine, url ? url : "dynamic script load");
                JS_FreeValue(engine->context, result);
            }
            JS_FreeValue(engine->context, onload);
            JS_FreeValue(engine->context, node);
            free(src); free(code); free(type); free(url);
        }
        JS_FreeValue(engine->context, dynamic);
    }
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

/* Recognising a bot challenge is not solving one. A challenge shell runs its
   own scripts, which otherwise consume the whole settle budget before the run
   can report that no content was ever available.

   Every marker here was checked against pages that do serve content, because
   the cost of a false positive is high: it would skip the scripts of a
   working page. Vendor script names are deliberately NOT used. Airbnb names
   DataDome and Product Hunt loads Cloudflare's challenge-platform precursor
   while both serve full pages; only strings unique to an interstitial
   qualify. The size guard is the second line of defence -- a challenge shell
   served challenge shell is a small document, so a large one is never
   classified from its markup alone however its scripts are named.

   The limit differs by caller. Before scripts run, the decision is
   destructive -- it skips the page's scripts -- so the served document must
   also be small. After settling, the decision only labels the exit status, and
   a challenge that renders itself from script has by then grown well past the
   served size, so the larger limit applies. */
#define DISTILL_CHALLENGE_SERVED_BYTES 131072
#define DISTILL_CHALLENGE_SETTLED_BYTES 1048576

static const char *challenge_kind(const char *data, size_t length, size_t limit)
{
    static const struct { const char *mark; const char *name; } marks[] = {
        {"cf-browser-verification", "cloudflare"},
        {"cf_chl_opt", "cloudflare"},
        {"just a moment...", "cloudflare"},
        {"checking your browser before accessing", "cloudflare"},
        {"prove your humanity", "humanity-check"},
        {"pardon the interruption", "bot-mitigation"},
        {"px-captcha", "perimeterx"},
        {"_px_captcha", "perimeterx"},
        {"vercel security checkpoint", "vercel-security-checkpoint"},
        {"enable javascript to continue", "vercel-security-checkpoint"},
        {"captcha-delivery.com", "datadome"},
        {"awswafcookiedomainlist", "aws-waf"},
        {"window.gokuprops", "aws-waf"},
    };
    if (!data || !length || length > limit) return NULL;
    char *lower = d_strndup(data, length);
    for (char *p = lower; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    const char *found = NULL;
    for (size_t i = 0; i < sizeof(marks) / sizeof(*marks) && !found; ++i) {
        if (strstr(lower, marks[i].mark)) found = marks[i].name;
    }
    free(lower);
    return found;
}

static void settle(Engine *engine)
{
    for (;;) {
        process_network(engine);
        process_async_scripts(engine);
        execute_jobs(engine);
        run_timers(engine);
        execute_jobs(engine);
        dispatch_load_if_ready(engine);
        if (engine->progress && engine->cap_ms) {
            /* The settle budget is the one bound this phase cannot exceed, so
               it is what the band is measured against. Settling early -- which
               is the common case -- simply ends the phase ahead of the bar. */
            uint64_t elapsed = clock_ms() - engine->started;
            if (elapsed > engine->cap_ms) elapsed = engine->cap_ms;
            uint64_t span = PROGRESS_SCRIPTS_END - PROGRESS_FETCH_END;
            report_progress(engine, PROGRESS_FETCH_END + (int)(span * elapsed / engine->cap_ms),
                            "scripts");
        }
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
static char *run_extraction(Engine *engine, const char *serialized, bool include_readability)
{
    (void)engine; (void)serialized; (void)include_readability;
    return d_strdup("{\"schema\":1,\"readability\":null}");
}
#endif

#ifdef DISTILL_B_PRUNED
extern int distill_litehtml_probe(const char *html);
#endif

/* A page can be useful before its scripts run and empty after a framework
   crashes. Keep the two extraction results separate until there is enough
   evidence to choose one. These scores are deliberately content-oriented:
   navigation links and a document title help, but do not outweigh readable
   blocks. */
typedef struct {
    long score;
    size_t blocks;
    size_t characters;
    size_t links;
    size_t media;
    bool useful;
    bool error_like;
} ExtractionCandidate;

static int64_t candidate_array_length(Engine *engine, JSValueConst value)
{
    return json_array_length_for_context(engine->context, value);
}

static char *candidate_string(Engine *engine, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    char *result = JS_IsUndefined(value) || JS_IsNull(value)
                       ? d_strdup("") : js_string(engine->context, value);
    JS_FreeValue(engine->context, value);
    return result;
}

static bool candidate_error_text(const char *text)
{
    char *lower = d_lower(text);
    bool result = d_contains(lower, "application error") ||
                  d_contains(lower, "something went wrong") ||
                  d_contains(lower, "internal server error") ||
                  d_contains(lower, "checking your browser") ||
                  d_contains(lower, "enable javascript") ||
                  d_contains(lower, "javascript is required") ||
                  d_contains(lower, "page could not be loaded");
    free(lower);
    return result;
}

static ExtractionCandidate score_extraction(Engine *engine, const char *json)
{
    ExtractionCandidate score = {0};
    JSValue parsed = JS_ParseJSON(engine->context, json ? json : "", json ? strlen(json) : 0,
                                  "distill-candidate.json");
    if (JS_IsException(parsed)) {
        JS_FreeValue(engine->context, JS_GetException(engine->context));
        return score;
    }
    JSValue blocks = JS_GetPropertyStr(engine->context, parsed, "blocks");
    int64_t block_count = candidate_array_length(engine, blocks);
    bool any_error = false;
    for (int64_t i = 0; i < block_count; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        char *text = candidate_string(engine, block, "text");
        if (*text) {
            ++score.blocks;
            size_t length = strlen(text);
            if (candidate_error_text(text)) any_error = true;
            else {
                score.characters += length;
                score.score += (long)length + 32;
            }
        }
        free(text);
        JS_FreeValue(engine->context, block);
    }
    JSValue links = JS_GetPropertyStr(engine->context, parsed, "links");
    JSValue media = JS_GetPropertyStr(engine->context, parsed, "media");
    score.links = (size_t)candidate_array_length(engine, links);
    score.media = (size_t)candidate_array_length(engine, media);
    score.score += (long)score.links * 8L + (long)score.media * 16L;

    JSValue main_content = JS_GetPropertyStr(engine->context, parsed, "main_content");
    JSValue main_blocks = JS_GetPropertyStr(engine->context, main_content, "blocks");
    int64_t main_count = candidate_array_length(engine, main_blocks);
    for (int64_t i = 0; i < main_count; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, main_blocks, (uint32_t)i);
        char *text = candidate_string(engine, block, "text");
        if (*text && !candidate_error_text(text)) score.score += 24;
        free(text);
        JS_FreeValue(engine->context, block);
    }

    JSValue readability = JS_GetPropertyStr(engine->context, parsed, "readability");
    if (!JS_IsNull(readability) && !JS_IsUndefined(readability)) {
        char *text = candidate_string(engine, readability, "textContent");
        if (*text && !candidate_error_text(text)) {
            score.characters += strlen(text);
            score.score += (long)strlen(text) / 2L;
        } else if (*text) {
            any_error = true;
        }
        free(text);
    }
    score.error_like = any_error && score.characters < 240;
    score.useful = score.characters >= 24 || score.blocks >= 2 ||
                   score.links >= 2 || score.media >= 1;
    JS_FreeValue(engine->context, readability);
    JS_FreeValue(engine->context, main_blocks);
    JS_FreeValue(engine->context, main_content);
    JS_FreeValue(engine->context, media);
    JS_FreeValue(engine->context, links);
    JS_FreeValue(engine->context, blocks);
    JS_FreeValue(engine->context, parsed);
    return score;
}

static char *annotate_extraction(Engine *engine, const char *json, const char *source,
                                 const char *reason, const ExtractionCandidate *initial,
                                 const ExtractionCandidate *settled)
{
    JSValue parsed = JS_ParseJSON(engine->context, json ? json : "", json ? strlen(json) : 0,
                                  "distill-selected-extraction.json");
    if (JS_IsException(parsed)) {
        JS_FreeValue(engine->context, JS_GetException(engine->context));
        return d_strdup(json ? json : "");
    }
    JSValue diagnostics = JS_NewObject(engine->context);
    JS_SetPropertyStr(engine->context, diagnostics, "selected_source", js_str(engine->context, source));
    JS_SetPropertyStr(engine->context, diagnostics, "fallback", JS_NewBool(engine->context, d_eq(source, "initial-html")));
    JS_SetPropertyStr(engine->context, diagnostics, "reason", js_str(engine->context, reason));
    JS_SetPropertyStr(engine->context, diagnostics, "initial_score", JS_NewInt64(engine->context, initial->score));
    JS_SetPropertyStr(engine->context, diagnostics, "settled_score", JS_NewInt64(engine->context, settled->score));
    JS_SetPropertyStr(engine->context, diagnostics, "script_errors", JS_NewInt64(engine->context, (int64_t)engine->script_errors));
    JS_SetPropertyStr(engine->context, diagnostics, "cap_hit", JS_NewBool(engine->context, engine->cap_hit));
    /* How much audio the page built in script rather than in markup. Not a
       media reference -- a count, so the decision to chase JS players stays a
       measurement rather than a guess. */
    JSValue script_audio = JS_GetPropertyStr(engine->context, engine->global, "__distillScriptAudio");
    int64_t script_audio_count = JS_IsArray(script_audio) ? json_array_length_for_context(engine->context, script_audio) : 0;
    JS_FreeValue(engine->context, script_audio);
    JS_SetPropertyStr(engine->context, diagnostics, "script_audio", JS_NewInt64(engine->context, script_audio_count));
    JS_SetPropertyStr(engine->context, parsed, "diagnostics", diagnostics);
    JSValue serialized = JS_JSONStringify(engine->context, parsed, JS_UNDEFINED, JS_UNDEFINED);
    char *result = JS_IsException(serialized) ? d_strdup(json ? json : "") : js_string(engine->context, serialized);
    if (JS_IsException(serialized)) JS_FreeValue(engine->context, JS_GetException(engine->context));
    JS_FreeValue(engine->context, serialized);
    JS_FreeValue(engine->context, parsed);
    return result;
}

static char *select_extraction(Engine *engine, const char *initial_json,
                               const char *settled_json, bool scripts_failed)
{
    ExtractionCandidate initial = score_extraction(engine, initial_json);
    ExtractionCandidate settled = score_extraction(engine, settled_json);
    bool choose_initial = scripts_failed || (initial.useful &&
                          (!settled.useful || settled.error_like ||
                           initial.score > settled.score + settled.score / 4L));
    const char *source = choose_initial ? "initial-html" : "settled-dom";
    const char *reason = choose_initial
        ? (scripts_failed ? "script discovery failed; initial HTML preserved"
                          : !settled.useful ? "settled DOM has no useful content"
                           : settled.error_like ? "settled DOM is an error or placeholder"
                                                 : "initial HTML has substantially more content")
        : (!initial_json ? "settled DOM accepted without fallback extraction"
                         : initial.useful ? "settled DOM is coherent or improves the page"
                                          : "initial HTML has no useful content");
    const char *selected = choose_initial ? initial_json : settled_json;
    return annotate_extraction(engine, selected, source, reason, &initial, &settled);
}

#ifdef DISTILL_RENDER
extern int distill_litehtml_render(const char *html, const char *output_path,
                                   int width, int height, const char *font_dir);
/* Laid-out height of the reader page at a given width; see render.h. */
extern int distill_litehtml_content_height(const char *html, int width,
                                           const char *font_dir);
#define DISTILL_MAX_RENDER_HEIGHT 20000
#endif

#ifdef DISTILL_READER
/* The format these bytes are in, or "" for none we recognise; see image.h. */
extern const char *d_image_format_name(const unsigned char *bytes, size_t length);
typedef struct { int width; int height; uint32_t *pixels; } DImage;
extern bool d_image_load_file(const char *path, DImage *out);
extern void d_image_free(DImage *image);
extern const char *d_image_last_error(void);
#endif

/* The reader page: the template every front end renders, the cards in it and
   the JSON helpers they read through. None of it lays anything out, so it
   belongs to every build that extracts, not only to the one that paints. */
#ifdef DISTILL_READER
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

static bool json_property_bool(Engine *engine, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    bool result = JS_ToBool(engine->context, value);
    JS_FreeValue(engine->context, value);
    return result;
}

static double json_property_number(Engine *engine, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(engine->context, object, name);
    double result = 0;
    if (JS_ToFloat64(engine->context, &result, value) < 0) {
        JS_FreeValue(engine->context, JS_GetException(engine->context));
        result = 0;
    }
    JS_FreeValue(engine->context, value);
    return result;
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

static bool json_block_is_main(Engine *engine, JSValueConst block)
{
    JSValue flag = JS_GetPropertyStr(engine->context, block, "main");
    bool promoted = JS_ToBool(engine->context, flag);
    JS_FreeValue(engine->context, flag);
    return promoted;
}

/* skip_main leaves out the blocks the reader has already shown at the top of
   the page. The extraction list stays complete on purpose, so the decision not
   to repeat them belongs here rather than in the engine. */
static void append_json_blocks(Engine *engine, JSValueConst blocks, DBuffer *output,
                               bool skip_main)
{
    int64_t length = json_array_length(engine, blocks);
    for (int64_t i = 0; i < length; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        if (skip_main && json_block_is_main(engine, block)) {
            JS_FreeValue(engine->context, block);
            continue;
        }
        char *type = json_property_string(engine, block, "type");
        char *text = json_property_string(engine, block, "text");
        char *href = json_property_string(engine, block, "href");
        if (d_eq(type, "h1")) {
            /* The reader title is already displayed above the block stream. */
        } else if (d_eq(type, "h2") || d_eq(type, "h3")) {
            dbuf_printf(output, "<%s>", type);
            /* A headline that was a link stays one. On a front page this is
               the whole of the information: the words say what happened, the
               link is the only way to go and read it. */
            if (*href) {
                dbuf_puts(output, "<a href=\"");
                append_html_escaped(output, href);
                dbuf_puts(output, "\">");
            }
            append_html_escaped(output, text);
            if (*href) dbuf_puts(output, "</a>");
            dbuf_printf(output, "</%s>", type);
        } else if (d_eq(type, "pre")) {
            dbuf_puts(output, "<pre><code>");
            append_html_escaped(output, text);
            dbuf_puts(output, "</code></pre>");
        } else if (d_eq(type, "li")) {
            dbuf_puts(output, "<p class=\"reader-list-item\">• ");
            if (*href) {
                dbuf_puts(output, "<a href=\"");
                append_html_escaped(output, href);
                dbuf_puts(output, "\">");
            }
            append_html_escaped(output, text);
            if (*href) dbuf_puts(output, "</a>");
            dbuf_puts(output, "</p>");
        } else if (*text) {
            dbuf_puts(output, "<p>");
            if (*href) {
                dbuf_puts(output, "<a href=\"");
                append_html_escaped(output, href);
                dbuf_puts(output, "\">");
            }
            append_html_escaped(output, text);
            if (*href) dbuf_puts(output, "</a>");
            dbuf_puts(output, "</p>");
        }
        free(type);
        free(text);
        free(href);
        JS_FreeValue(engine->context, block);
    }
}

/* A front page is a block stream whose links are the content. Treating its
   h2/h3 elements like article subheads makes every item pay article-sized
   margins, so identify the dense linked-heading shape before writing the
   template. */
static bool json_is_dense_link_index(Engine *engine, JSValueConst blocks)
{
    int64_t block_count = json_array_length(engine, blocks);
    int64_t linked_count = 0;
    int64_t heading_count = 0;
    int64_t linked_heading_count = 0;
    for (int64_t i = 0; i < block_count; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        char *type = json_property_string(engine, block, "type");
        char *href = json_property_string(engine, block, "href");
        bool heading = d_eq(type, "h2") || d_eq(type, "h3");
        if (*href) ++linked_count;
        if (heading) {
            ++heading_count;
            if (*href) ++linked_heading_count;
        }
        free(type);
        free(href);
        JS_FreeValue(engine->context, block);
    }
    return heading_count >= 3 && linked_heading_count * 2 >= heading_count &&
           linked_count * 2 >= block_count;
}

/* How much of the page is left once the promoted item is taken out. Nothing
   left means no divider: a heading announcing an empty remainder is worse than
   no heading at all. */
static int64_t json_blocks_outside_main(Engine *engine, JSValueConst blocks)
{
    int64_t length = json_array_length(engine, blocks);
    int64_t outside = 0;
    for (int64_t i = 0; i < length; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        if (!json_block_is_main(engine, block)) ++outside;
        JS_FreeValue(engine->context, block);
    }
    return outside;
}

/* The promoted item's own headline. It is preferred to the record's headline
   because a publisher writes the section into that -- "DN Direkt - Sheeran:
   ..." - while the item's <h1> is the words a reader came for. */
static char *json_main_heading(Engine *engine, JSValueConst blocks)
{
    int64_t length = json_array_length(engine, blocks);
    for (int64_t i = 0; i < length; ++i) {
        JSValue block = JS_GetPropertyUint32(engine->context, blocks, (uint32_t)i);
        char *type = json_property_string(engine, block, "type");
        bool heading = d_eq(type, "h1");
        free(type);
        if (heading) {
            char *text = json_property_string(engine, block, "text");
            JS_FreeValue(engine->context, block);
            if (*text) return text;
            free(text);
            continue;
        }
        JS_FreeValue(engine->context, block);
    }
    return d_strdup("");
}

static int count_literal(const char *text, const char *literal)
{
    if (!text || !literal || !*literal) return 0;
    int count = 0;
    size_t length = strlen(literal);
    for (const char *cursor = text; (cursor = strstr(cursor, literal)); cursor += length)
        ++count;
    return count;
}

static bool html_is_dense_link_index(const char *html)
{
    int headings = count_literal(html, "<h2") + count_literal(html, "<h3");
    int linked_headings = count_literal(html, "<h2><a") + count_literal(html, "<h3><a");
    int links = count_literal(html, "<a ") + count_literal(html, "<a>");
    return headings >= 3 && linked_headings * 2 >= headings && links * 2 >= headings;
}

/* The GTK shell reads these tiny internal links from the laid-out link boxes
   to jump to the same document position the reader paints. They carry no text
   and are only one pixel high, so they do not add another visible control to
   the page. */
static void append_section_marker(DBuffer *output, const char *id)
{
    dbuf_puts(output, "<a class=\"section-anchor\" aria-hidden=\"true\" href=\"distill-section:");
    append_html_escaped(output, id);
    dbuf_puts(output, "\"></a>");
}

static void append_json_table(Engine *engine, JSValueConst tables, DBuffer *output)
{
    int64_t table_count = json_array_length(engine, tables);
    for (int64_t table_index = 0; table_index < table_count; ++table_index) {
        JSValue table = JS_GetPropertyUint32(engine->context, tables, (uint32_t)table_index);
        char *caption = json_property_string(engine, table, "caption");
        JSValue rows = JS_GetPropertyStr(engine->context, table, "rows");
        int64_t row_count = json_array_length(engine, rows);
        if (table_index == 0) append_section_marker(output, "tables");
        dbuf_puts(output, table_index == 0
                         ? "<section id=\"tables\" class=\"table-card\">"
                         : "<section class=\"table-card\">");
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
    append_section_marker(output, "results");
    dbuf_puts(output, "<section id=\"results\" class=\"result-list\"><h2>Results</h2>");
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

static void append_form_metadata(DBuffer *output, bool checked, bool disabled, bool required)
{
    if (checked) append_json_attr(output, "data-distill-checked", "1");
    if (disabled) append_json_attr(output, "data-distill-disabled", "1");
    if (required) append_json_attr(output, "data-distill-required", "1");
}

static void append_json_select_options(Engine *engine, JSValueConst options, DBuffer *output)
{
    int64_t count = json_array_length(engine, options);
    for (int64_t i = 0; i < count; ++i) {
        JSValue option = JS_GetPropertyUint32(engine->context, options, (uint32_t)i);
        char *value = json_property_string(engine, option, "value");
        char *label = json_property_string(engine, option, "label");
        bool selected = json_property_bool(engine, option, "selected");
        bool disabled = json_property_bool(engine, option, "disabled");
        dbuf_puts(output, "<option");
        append_json_attr(output, "value", value);
        if (selected) append_json_attr(output, "data-distill-selected", "1");
        if (disabled) append_json_attr(output, "data-distill-disabled", "1");
        dbuf_puts(output, ">");
        append_html_escaped(output, label);
        dbuf_puts(output, "</option>");
        free(value);
        free(label);
        JS_FreeValue(engine->context, option);
    }
}

static void append_json_forms(Engine *engine, JSValueConst forms, DBuffer *output)
{
    int64_t form_count = json_array_length(engine, forms);
    for (int64_t form_index = 0; form_index < form_count; ++form_index) {
        JSValue form = JS_GetPropertyUint32(engine->context, forms, (uint32_t)form_index);
        char *action = json_property_string(engine, form, "action");
        char *method = json_property_string(engine, form, "method");
        JSValue fields = JS_GetPropertyStr(engine->context, form, "fields");
        if (form_index == 0) append_section_marker(output, "forms");
        dbuf_puts(output, form_index == 0
                         ? "<section id=\"forms\" class=\"form-card\"><h2>Form</h2><form"
                         : "<section class=\"form-card\"><h2>Form</h2><form");
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
            char *caption = json_property_string(engine, field, "caption");
            char *placeholder = json_property_string(engine, field, "placeholder");
            bool checked = json_property_bool(engine, field, "checked");
            bool disabled = json_property_bool(engine, field, "disabled");
            bool required = json_property_bool(engine, field, "required");
            JSValue options = JS_GetPropertyStr(engine->context, field, "options");
            char control_id[64];
            snprintf(control_id, sizeof(control_id), "distill-form-%lld-%lld",
                     (long long)form_index, (long long)field_index);
            if (d_eq(type, "hidden")) {
                dbuf_puts(output, "<input type=\"hidden\"");
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                dbuf_puts(output, ">");
            } else if (d_eq(type, "submit") || d_eq(type, "reset") || d_eq(type, "button")) {
                dbuf_puts(output, "<button");
                append_json_attr(output, "class", d_eq(type, "submit") ? "form-submit" : "form-button");
                append_json_attr(output, "type", d_eq(type, "submit") ? "submit" : "button");
                append_json_attr(output, "data-distill-type", type);
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                append_form_metadata(output, checked, disabled, required);
                dbuf_puts(output, ">");
                append_html_escaped(output, *caption ? caption : (*value ? value : "Submit"));
                dbuf_puts(output, "</button>");
            } else if (d_eq(type, "checkbox") || d_eq(type, "radio")) {
                dbuf_puts(output, "<label class=\"form-choice\"><input class=\"form-choice-input\"");
                append_json_attr(output, "id", control_id);
                append_json_attr(output, "type", type);
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                append_form_metadata(output, checked, disabled, required);
                dbuf_puts(output, "><span>");
                append_html_escaped(output, *label ? label : name);
                dbuf_puts(output, "</span></label>");
            } else if (d_eq(type, "textarea")) {
                dbuf_puts(output, "<label class=\"form-field-label\"");
                append_json_attr(output, "for", control_id);
                dbuf_puts(output, ">");
                append_html_escaped(output, *label ? label : name);
                dbuf_puts(output, "</label><textarea class=\"form-field-textarea\"");
                append_json_attr(output, "id", control_id);
                append_json_attr(output, "data-distill-type", "textarea");
                append_json_attr(output, "name", name);
                append_json_attr(output, "placeholder", placeholder);
                append_form_metadata(output, checked, disabled, required);
                dbuf_puts(output, ">");
                append_html_escaped(output, value);
                dbuf_puts(output, "</textarea>");
            } else if (d_eq(type, "select")) {
                dbuf_puts(output, "<label class=\"form-field-label\"");
                append_json_attr(output, "for", control_id);
                dbuf_puts(output, ">");
                append_html_escaped(output, *label ? label : name);
                dbuf_puts(output, "</label><select class=\"form-field-select\"");
                append_json_attr(output, "id", control_id);
                append_json_attr(output, "data-distill-type", "select");
                append_json_attr(output, "name", name);
                append_form_metadata(output, checked, disabled, required);
                dbuf_puts(output, ">");
                append_json_select_options(engine, options, output);
                dbuf_puts(output, "</select>");
            } else {
                dbuf_puts(output, "<label class=\"form-field-label\"");
                append_json_attr(output, "for", control_id);
                dbuf_puts(output, ">");
                append_html_escaped(output, *label ? label : name);
                dbuf_puts(output, "</label><input class=\"form-field-input\"");
                append_json_attr(output, "id", control_id);
                append_json_attr(output, "type", *type ? type : "text");
                append_json_attr(output, "name", name);
                append_json_attr(output, "value", value);
                append_json_attr(output, "placeholder", placeholder);
                append_form_metadata(output, checked, disabled, required);
                dbuf_puts(output, ">");
            }
            free(label); free(name); free(type); free(value); free(caption); free(placeholder);
            JS_FreeValue(engine->context, options);
            JS_FreeValue(engine->context, field);
        }
        dbuf_puts(output, "</form></section>");
        free(action); free(method);
        JS_FreeValue(engine->context, fields);
        JS_FreeValue(engine->context, form);
    }
}

/* A URL as something to read rather than something to parse. litehtml
   implements neither overflow-wrap nor word-break, so a CDN URL is one
   unbreakable token that simply runs off the right edge of the column and
   keeps going. Shortening it is not only a layout fix: the host and the file
   name are the part that says where an image came from, and forty characters
   of cache key in the middle say nothing. The whole URL stays in the link. */
#define DISTILL_DISPLAY_URL_BUDGET 58

static void append_display_url(DBuffer *output, const char *url)
{
    const char *text = url ? url : "";
    if (!strncmp(text, "https://", 8)) text += 8;
    else if (!strncmp(text, "http://", 7)) text += 7;
    size_t length = strlen(text);
    while (length && text[length - 1] == '/') --length;
    if (length <= DISTILL_DISPLAY_URL_BUDGET) {
        char *shown = d_strndup(text, length);
        append_html_escaped(output, shown);
        free(shown);
        return;
    }
    /* Keep the host, and as much of the tail as the budget allows: the file
       name is what identifies one image among a page of them. */
    size_t host = strcspn(text, "/");
    if (host > DISTILL_DISPLAY_URL_BUDGET / 2) host = DISTILL_DISPLAY_URL_BUDGET / 2;
    size_t tail = DISTILL_DISPLAY_URL_BUDGET - host - 1;
    if (tail > length) tail = length;
    char *head = d_strndup(text, host);
    append_html_escaped(output, head);
    free(head);
    dbuf_puts(output, "\u2026");
    append_html_escaped(output, text + length - tail);
}

/* Elapsed/total in the form a reader expects to see, not in seconds. */
static void append_duration(DBuffer *output, double seconds)
{
    if (!(seconds > 0)) return;
    long total = (long)(seconds + 0.5);
    long hours = total / 3600, minutes = (total % 3600) / 60, rest = total % 60;
    char text[32];
    if (hours) snprintf(text, sizeof text, "%ld:%02ld:%02ld", hours, minutes, rest);
    else snprintf(text, sizeof text, "%ld:%02ld", minutes, rest);
    dbuf_puts(output, text);
}

static void append_media_action(DBuffer *output, const char *scheme, const char *url,
                                const char *label, bool primary)
{
    dbuf_printf(output, "<a class=\"button%s\" href=\"", primary ? "" : " button-secondary");
    dbuf_puts(output, scheme);
    append_html_escaped(output, url);
    dbuf_puts(output, "\">");
    dbuf_puts(output, label);
    dbuf_puts(output, "</a>");
}

/* Audio is the one medium the reader plays itself, and it says plainly when it
   cannot: an Opus or AAC file is a download, not a Play button that fails. */
/* A page offering files for download can name dozens of them, and a full card
   each would bury the prose that explains what they are. A reference found as
   a link carries no duration or declared type to show anyway, so it gets one
   row: what it is called, and what can be done with it. */
static void append_json_audio_list(Engine *engine, JSValueConst audio, DBuffer *output,
                                   int64_t count)
{
    bool opened = false;
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, audio, (uint32_t)i);
        char *source = json_property_string(engine, item, "source");
        char *url = json_property_string(engine, item, "url");
        char *title = json_property_string(engine, item, "title");
        bool playable = json_property_bool(engine, item, "playable");
        if (d_eq(source, "link") && *url) {
            if (!opened) {
                append_section_marker(output, "audio");
                dbuf_puts(output, "<section id=\"audio\" class=\"audio-card\">"
                                  "<p class=\"eyebrow\">Audio</p><h2>Audio files on this page</h2>");
                opened = true;
            }
            dbuf_puts(output, "<p class=\"audio-row\"><span class=\"audio-name\">");
            append_html_escaped(output, *title ? title : "Audio");
            dbuf_puts(output, "</span>");
            if (playable) append_media_action(output, "distill-play:", url, "Play", true);
            append_media_action(output, "distill-download:", url, "Download", false);
            dbuf_puts(output, "</p>");
        }
        free(source); free(url); free(title);
        JS_FreeValue(engine->context, item);
    }
    if (opened) dbuf_puts(output, "</section>");
}

static void append_json_audio(Engine *engine, JSValueConst audio, DBuffer *output)
{
    int64_t count = json_array_length(engine, audio);
    append_json_audio_list(engine, audio, output, count);
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, audio, (uint32_t)i);
        char *found_by = json_property_string(engine, item, "source");
        bool from_link = d_eq(found_by, "link");
        free(found_by);
        if (from_link) { JS_FreeValue(engine->context, item); continue; }
        char *title = json_property_string(engine, item, "title");
        char *url = json_property_string(engine, item, "url");
        char *mime = json_property_string(engine, item, "mime");
        double duration = json_property_number(engine, item, "duration");
        bool playable = json_property_bool(engine, item, "playable");
        if (*url) {
            dbuf_puts(output, "<section class=\"audio-card\"><p class=\"eyebrow\">Audio</p><h2>");
            append_html_escaped(output, *title ? title : "Audio");
            dbuf_puts(output, "</h2>");
            if (duration > 0 || *mime) {
                dbuf_puts(output, "<p class=\"media-meta\">");
                append_duration(output, duration);
                if (duration > 0 && *mime) dbuf_puts(output, " \xc2\xb7 ");
                if (*mime) append_html_escaped(output, mime);
                dbuf_puts(output, "</p>");
            }
            if (playable) {
                append_media_action(output, "distill-play:", url, "Play", true);
                append_media_action(output, "distill-download:", url, "Download", false);
            } else {
                dbuf_puts(output, "<p>This reader decodes MP3, WAV and FLAC. This file is something else, so it is offered as a download.</p>");
                append_media_action(output, "distill-download:", url, "Download", true);
                append_media_action(output, "distill-open:", url, "Open externally", false);
            }
            dbuf_puts(output, "<p class=\"media-url\">");
            append_display_url(output, url);
            dbuf_puts(output, "</p></section>");
        }
        free(title); free(url); free(mime);
        JS_FreeValue(engine->context, item);
    }
}

/* The action follows what the reference is. A direct file can be downloaded
   and never played here; an embed is somebody else's player, so there is
   nothing to download and the hand-off is the only honest offer. */
static void append_json_videos(Engine *engine, JSValueConst videos, DBuffer *output)
{
    int64_t count = json_array_length(engine, videos);
    for (int64_t i = 0; i < count; ++i) {
        JSValue video = JS_GetPropertyUint32(engine->context, videos, (uint32_t)i);
        char *title = json_property_string(engine, video, "title");
        char *url = json_property_string(engine, video, "url");
        char *poster = json_property_string(engine, video, "poster");
        char *mime = json_property_string(engine, video, "mime");
        double duration = json_property_number(engine, video, "duration");
        bool direct = json_property_bool(engine, video, "direct");
        if (i == 0) append_section_marker(output, "videos");
        dbuf_puts(output, i == 0
                         ? "<section id=\"videos\" class=\"video-card\"><p class=\"eyebrow\">Video reference</p><h2>"
                         : "<section class=\"video-card\"><p class=\"eyebrow\">Video reference</p><h2>");
        append_html_escaped(output, *title ? title : "External video");
        dbuf_puts(output, "</h2>");
        if (duration > 0 || (*mime && direct)) {
            dbuf_puts(output, "<p class=\"media-meta\">");
            append_duration(output, duration);
            if (duration > 0 && *mime && direct) dbuf_puts(output, " \xc2\xb7 ");
            if (*mime && direct) append_html_escaped(output, mime);
            dbuf_puts(output, "</p>");
        }
        if (!*url) {
            dbuf_puts(output, "<p>The page names a video here but gives no address for it.</p>");
        } else if (direct) {
            dbuf_puts(output, "<p>This reader keeps playback outside the page. The file can be saved and played later.</p>");
            append_media_action(output, "distill-download:", url, "Download", true);
            append_media_action(output, "distill-open:", url, "Open externally", false);
        } else {
            dbuf_puts(output, "<p>This is an embedded player, not a file, so there is nothing here to download.</p>");
            append_media_action(output, "distill-open:", url, "Open externally", true);
        }
        if (*url) {
            dbuf_puts(output, "<p class=\"media-url\">");
            append_display_url(output, url);
            dbuf_puts(output, "</p>");
        }
        if (*poster) {
            dbuf_puts(output, "<p class=\"media-url\">");
            append_display_url(output, poster);
            dbuf_puts(output, "</p>");
        }
        dbuf_puts(output, "</section>");
        free(title); free(url); free(poster); free(mime);
        JS_FreeValue(engine->context, video);
    }
}

static void append_json_media(Engine *engine, JSValueConst media, DBuffer *output)
{
    int64_t count = json_array_length(engine, media);
    if (!count) return;
    append_section_marker(output, "images");
    dbuf_puts(output, "<section id=\"images\" class=\"media-gallery\"><h2>Images</h2>");
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, media, (uint32_t)i);
        char *url = json_property_string(engine, item, "url");
        char *alt = json_property_string(engine, item, "alt");
        dbuf_puts(output, "<p class=\"media-item\">");
        /* Keep the media activation target as one ordinary inline link. The
           desktop shell still recognizes distill-media: and fetches lazily,
           but the reader does not repeat the alt text in a placeholder and a
           caption. */
        bool openable = is_http_url(url);
        if (openable) {
            dbuf_puts(output, "<a class=\"media-open\" href=\"distill-media:");
            append_html_escaped(output, url);
            dbuf_puts(output, "\">");
        }
        append_html_escaped(output, *alt ? alt : "Decorative image");
        if (openable) dbuf_puts(output, "</a>");
        dbuf_puts(output, "</p>");
        free(url); free(alt);
        JS_FreeValue(engine->context, item);
    }
    dbuf_puts(output, "</section>");
}

static void append_json_links(Engine *engine, JSValueConst links, DBuffer *output)
{
    int64_t count = json_array_length(engine, links);
    if (!count) return;
    append_section_marker(output, "links");
    dbuf_puts(output, "<section id=\"links\" class=\"link-list\"><h2>Links</h2>");
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, links, (uint32_t)i);
        char *text = json_property_string(engine, item, "text");
        char *url = json_property_string(engine, item, "url");
        if (*text || *url) {
            dbuf_puts(output, "<p><a");
            if (*url) append_json_attr(output, "href", url);
            dbuf_puts(output, ">");
            append_html_escaped(output, *text ? text : url);
            dbuf_puts(output, "</a></p>");
        }
        free(text); free(url);
        JS_FreeValue(engine->context, item);
    }
    dbuf_puts(output, "</section>");
}

static void append_json_navigation(Engine *engine, JSValueConst navigation,
                                   DBuffer *output)
{
    int64_t count = json_array_length(engine, navigation);
    if (!count) return;
    append_section_marker(output, "navigation");
    dbuf_puts(output, "<section id=\"navigation\" class=\"link-list\"><h2>Navigation</h2>");
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(engine->context, navigation, (uint32_t)i);
        char *text = json_property_string(engine, item, "text");
        char *url = json_property_string(engine, item, "url");
        if (*text && *url) {
            dbuf_puts(output, "<p><a");
            append_json_attr(output, "href", url);
            dbuf_puts(output, ">");
            append_html_escaped(output, text);
            dbuf_puts(output, "</a></p>");
        }
        free(text); free(url);
        JS_FreeValue(engine->context, item);
    }
    dbuf_puts(output, "</section>");
}

/* The template is laid out at whatever width the surface happens to be -- the
   desktop shell re-lays it out as its window changes -- so nothing here is
   computed for one width. The column is a maximum and the side padding the
   gutter beside it; a surface too narrow for both narrows the column and
   keeps the gutter, and a wider one centres the pair. Zoom scales them along
   with the type and the fields; every other length in the template is
   already relative to the body size. */
typedef struct {
    int font_size;
    int column;
    int side_padding;
    int top_padding;
    int bottom_padding;
    int field_height;
    int textarea_height;
    int choice_size;
} ReaderMetrics;

static int scale_px(int base, int zoom)
{
    long value = ((long)base * zoom + 50) / 100;
    if (value < 1) value = 1;
    return (int)value;
}

static ReaderMetrics reader_metrics(int zoom)
{
    if (zoom < READER_ZOOM_MIN) zoom = READER_ZOOM_MIN;
    if (zoom > READER_ZOOM_MAX) zoom = READER_ZOOM_MAX;
    return (ReaderMetrics){
        .font_size = scale_px(18, zoom),
        .column = scale_px(612, zoom),
        .side_padding = scale_px(28, zoom),
        .top_padding = scale_px(68, zoom),
        .bottom_padding = scale_px(96, zoom),
        .field_height = scale_px(28, zoom),
        .textarea_height = scale_px(104, zoom),
        .choice_size = scale_px(18, zoom)
    };
}

static char *reader_template(Engine *engine, const char *json, const char *theme,
                             bool native_forms, int zoom)
{
    ReaderMetrics metrics = reader_metrics(zoom);

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
    JSValue audio = JS_GetPropertyStr(engine->context, parsed, "audio");
    JSValue media = JS_GetPropertyStr(engine->context, parsed, "media");
    JSValue links = JS_GetPropertyStr(engine->context, parsed, "links");
    JSValue embedded_data = JS_GetPropertyStr(engine->context, parsed, "embedded_data");
    JSValue embedded_blocks = JS_GetPropertyStr(engine->context, embedded_data, "blocks");
    JSValue embedded_navigation = JS_GetPropertyStr(engine->context, embedded_data, "navigation");
    JSValue main_content = JS_GetPropertyStr(engine->context, parsed, "main_content");
    JSValue main_blocks = JS_GetPropertyStr(engine->context, main_content, "blocks");
    char *shape_kind = json_property_string(engine, shape, "kind");
    char *title = json_property_string(engine, parsed, "title");
    char *content = d_strdup("");
    char *text_dir = d_strdup("");
    char *text_lang = d_strdup("");
    if (!JS_IsNull(readability) && !JS_IsUndefined(readability)) {
        char *readable_title = json_property_string(engine, readability, "title");
        if (*readable_title) { free(title); title = readable_title; }
        else free(readable_title);
        content = json_property_string(engine, readability, "content");
        free(text_dir); text_dir = json_property_string(engine, readability, "dir");
        free(text_lang); text_lang = json_property_string(engine, readability, "lang");
    }
    /* The reader renders its own template, so the source document's
       directionality has to be carried across deliberately or a
       right-to-left article is laid out flush left. */
    bool rtl = d_ieq(text_dir, "rtl");
    /* The page located the item the URL asked for, so that item leads and the
       stream it came from follows. This wins over Readability, which scores the
       densest container and on a liveblog permalink returns the whole stream
       with the requested post somewhere inside it. */
    bool main_found = false;
    bool embedded_mapped = false;
    if (JS_IsObject(embedded_data)) {
        JSValue matched = JS_GetPropertyStr(engine->context, embedded_data, "matched");
        embedded_mapped = JS_ToBool(engine->context, matched);
        JS_FreeValue(engine->context, matched);
    }
    if (JS_IsObject(main_content)) {
        JSValue found = JS_GetPropertyStr(engine->context, main_content, "found");
        main_found = JS_ToBool(engine->context, found);
        JS_FreeValue(engine->context, found);
    }
    char *main_heading = d_strdup("");
    if (main_found) {
        free(main_heading);
        main_heading = json_main_heading(engine, main_blocks);
        if (!*main_heading) { free(main_heading); main_heading = json_property_string(engine, main_content, "title"); }
    }
    bool link_index = json_is_dense_link_index(engine, blocks) ||
                      html_is_dense_link_index(content);

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
    dbuf_puts(&output, "<!doctype html><html");
    if (*text_lang) {
        dbuf_puts(&output, " lang=\"");
        append_html_escaped(&output, text_lang);
        dbuf_puts(&output, "\"");
    }
    if (*text_dir) {
        dbuf_puts(&output, " dir=\"");
        append_html_escaped(&output, text_dir);
        dbuf_puts(&output, "\"");
    }
    dbuf_puts(&output,
        "><head><meta charset=\"utf-8\"><title>");
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
        "}html,body{margin:0;padding:0;background:var(--page-background);color:var(--page-text);}");
    /* The measure lives on <body>, which is the one box guaranteed to be a
       block here: litehtml's master stylesheet has no rule for <main>, which
       lays out as an inline box, and giving that box the measure instead
       changes how the margins inside the page collapse. So body is capped at
       the measure plus its gutters and centred in whatever the surface is,
       and everything inside it lays out exactly as it did at a fixed width.
       <html> carries the same background, so the surface stays one colour
       either side of it. */
    dbuf_printf(&output,
        "body{font-family:\"Source Serif 4\",serif;font-size:%dpx;line-height:1.58;"
        "max-width:%dpx;margin:0 auto;"
        "padding-top:%dpx;padding-right:%dpx;padding-bottom:%dpx;padding-left:%dpx;}"
        "main{max-width:%dpx;margin:0 auto;background:var(--main-background);}",
        metrics.font_size, metrics.column,
        metrics.top_padding, metrics.side_padding,
        metrics.bottom_padding, metrics.side_padding, metrics.column);
    dbuf_puts(&output,
        "h1,h2,h3,.eyebrow,button,input,.button,th{font-family:\"Source Sans 3\",sans-serif;}"
        "h1{font-size:2.35em;line-height:1.08;letter-spacing:-.025em;margin:0 0 1.1em;color:var(--heading-text);}"
        "h2{font-size:1.45em;line-height:1.2;margin:1.7em 0 .55em;color:var(--heading-text);}"
        "h3{font-size:1.15em;line-height:1.25;margin:1.35em 0 .45em;color:var(--heading-text);}"
        ".link-index h2,.link-index h3{font-size:1em;line-height:1.35;margin:0 0 .55em;font-weight:600;}"
        ".link-index p,.link-index ul,.link-index ol{line-height:1.35;margin:0 0 .55em;}"
        "p,ul,ol,pre,blockquote{margin:0 0 1.05em;}"
        "a{color:var(--link);}pre,code{font-family:\"Source Code Pro\",monospace;}pre{padding:1em;overflow:hidden;white-space:pre-wrap;overflow-wrap:anywhere;background:var(--pre-background);border-radius:4px;font-size:.86em;line-height:1.45;}"
        "blockquote{margin-left:1.2em;padding-left:1em;border-left:3px solid var(--border);color:var(--muted);}"
        "img,video,iframe{max-width:100%;height:auto;}"
        "table{width:100%;border-collapse:collapse;margin:0 0 1.5em;}th,td{text-align:left;padding:.55em .6em;border-bottom:1px solid var(--border);}"
        ".table-card,.form-card,.video-card,.audio-card,.media-gallery,.result-list{margin:2em 0;padding:1.2em;background:var(--card-background);border:1px solid var(--border);border-radius:5px;}"
        ".result-card{padding:.7em 0;border-bottom:1px solid var(--border);}.result-card h3{margin:0 0 .3em;}"
        ".form-card form{margin:0;background:transparent;text-align:left;}"
        ".form-field-label{display:block;margin:.7em 0 0;color:var(--page-text);text-align:left;}");
    /* A native control is placed on the geometry litehtml reports for the
       field it replaces, so a field that does not grow with the type leaves
       the reader with page-sized text in thumbnail-sized boxes. */
    dbuf_printf(&output,
        ".form-field-input{display:block;width:100%%;height:%dpx;box-sizing:border-box;margin:3px 0 0;padding:.55em;border:1px solid var(--border);border-radius:3px;background:var(--main-background);color:var(--page-text);text-align:left;}"
        ".form-field-textarea{display:block;width:100%%;height:%dpx;box-sizing:border-box;margin:3px 0 0;padding:.55em;border:1px solid var(--border);border-radius:3px;background:var(--main-background);color:var(--page-text);font:inherit;text-align:left;resize:none;}"
        ".form-field-select{display:block;width:100%%;height:%dpx;box-sizing:border-box;margin:3px 0 0;padding:0 .35em;border:1px solid var(--border);border-radius:3px;background:var(--main-background);color:var(--page-text);font:inherit;text-align:left;}"
        ".form-choice{display:block;margin:.55em 0 0;color:var(--page-text);text-align:left;}"
        ".form-choice-input{display:inline-block;width:%dpx;height:%dpx;box-sizing:border-box;margin:0 .45em 0 0;padding:0;vertical-align:middle;}",
        metrics.field_height, metrics.textarea_height, metrics.field_height,
        metrics.choice_size, metrics.choice_size);
    dbuf_puts(&output,
        ".form-choice span{vertical-align:middle;}"
        ".form-card form>button{margin:.7em .45em 0 0;text-align:left;}"
        "button,.button{display:inline-block;padding:.55em .85em;border:0;border-radius:3px;background:var(--link);color:white;text-decoration:none;font:inherit;}"
        ".video-card h2,.audio-card h2,.table-card h2,.form-card h2,.media-gallery h2,.result-list h2{margin-top:0;}"
        ".media-meta{color:var(--muted);margin:0 0 0.8em 0;}"
        ".audio-row{margin:0 0 .5em 0;}"
        ".audio-name{display:inline-block;min-width:17em;}"
        ".button-secondary{background:var(--card-background);border:1px solid var(--border);color:var(--page-text);}"
        ".video-card .button,.audio-card .button{margin:0 .5em .5em 0;}"
        ".eyebrow,.media-url{font-size:.78em;color:var(--muted);}"
        ".section-anchor{display:block;width:1px;height:1px;overflow:hidden;}"
        ".media-item{margin:0 0 .75em;}"
        ".reader-list-item{padding-left:1em;}"
        /* The promoted item keeps article spacing even when the page around it
           is a dense index, because it is being read, not scanned. */
        ".main-content{margin:0 0 1em;}"
        ".link-index .main-content p{line-height:1.62;margin:0 0 1.05em;}"
        ".link-index .main-content h2{font-size:1.45em;line-height:1.2;margin:1.7em 0 .55em;font-weight:bold;}"
        ".link-index .main-content h3{font-size:1.15em;line-height:1.25;margin:1.35em 0 .45em;font-weight:bold;}"
        ".main-rest{border-top:1px solid var(--border);padding-top:1.1em;margin-top:2.4em;margin-bottom:1.4em;}");
    if (native_forms)
        dbuf_puts(&output,
                  ".form-card form input,.form-card form textarea,"
                  ".form-card form select,.form-card form button{color:var(--card-background);"
                  "background:var(--card-background);background-image:none;"
                  "border-color:var(--card-background);text-shadow:none;}");
    /* litehtml exposes a form control's content box. Native GTK controls are
       positioned from that geometry, so remove the source control's padding
       and border only while it is hidden behind its GTK replacement. Its
       reported origin is then the outer edge that GTK must occupy. Explicit
       border-box dimensions still reserve the following field's space. */
    if (native_forms)
        dbuf_puts(&output,
                  ".form-card form input,.form-card form textarea,.form-card form select{padding:0;border-width:0;}");
    /* Mirrored rules rather than logical properties: litehtml lays out the
       reader template, and physical sides are what it reliably supports. */
    if (rtl) {
        dbuf_puts(&output,
            "body{text-align:right;}"
            "th,td{text-align:right;}"
            "blockquote{margin-left:0;margin-right:1.2em;padding-left:0;padding-right:1em;"
            "border-left:0;border-right:3px solid var(--border);}"
            ".reader-list-item{padding-left:0;padding-right:1em;}"
            "pre{text-align:left;}");
    }
    dbuf_puts(&output,
        "</style></head><body><main id=\"top\"");
    if (link_index) dbuf_puts(&output, " class=\"link-index\"");
    dbuf_puts(&output, "><h1>");
    const char *heading = *main_heading ? main_heading : (*title ? title : "Untitled page");
    append_html_escaped(&output, heading);
    dbuf_puts(&output, "</h1>");
    if (main_found) {
        dbuf_puts(&output, "<section class=\"main-content\">");
        append_json_blocks(engine, main_blocks, &output, false);
        dbuf_puts(&output, "</section>");
        if (json_blocks_outside_main(engine, blocks) > 0) {
            dbuf_puts(&output, "<p class=\"eyebrow main-rest\">Rest of the page</p>");
            append_json_blocks(engine, blocks, &output, true);
        }
        /* None of these is a text block, and all of them are ordinarily
           inside main rather than outside it. Leaving them to the "rest of
           the page" branch meant a page whose <main> held the whole document
           -- which is most pages that mark one up at all -- reached the
           reader with its table, its results, its gallery and its video
           actions silently dropped. A table page rendered as a heading and
           nothing else. */
        append_json_table(engine, tables, &output);
        append_json_results(engine, results, &output);
        append_json_videos(engine, videos, &output);
        append_json_media(engine, media, &output);
        /* Forms are separate interactive content, not text blocks. Keep them
           available even when the reader selected the whole page as main. */
        append_json_forms(engine, forms, &output);
    } else if (d_eq(shape_kind, "article") && *content) {
        dbuf_puts(&output, content);
        if (embedded_mapped && json_array_length(engine, embedded_blocks) > 0) {
            dbuf_puts(&output, "<section class=\"embedded-content\">");
            append_json_blocks(engine, embedded_blocks, &output, false);
            dbuf_puts(&output, "</section>");
        }
    } else if (d_eq(shape_kind, "docs")) {
        append_json_blocks(engine, blocks, &output, main_found);
    } else if (d_eq(shape_kind, "table")) {
        append_json_table(engine, tables, &output);
    } else if (d_eq(shape_kind, "search-results")) {
        append_json_results(engine, results, &output);
    } else if (d_eq(shape_kind, "form")) {
        append_json_forms(engine, forms, &output);
    } else if (d_eq(shape_kind, "video")) {
        append_json_blocks(engine, blocks, &output, main_found);
        append_json_videos(engine, videos, &output);
        append_json_links(engine, links, &output);
    } else if (d_eq(shape_kind, "image-heavy")) {
        append_json_media(engine, media, &output);
        append_json_links(engine, links, &output);
    } else {
        append_json_blocks(engine, blocks, &output, main_found);
        append_json_table(engine, tables, &output);
        append_json_results(engine, results, &output);
        append_json_forms(engine, forms, &output);
        append_json_videos(engine, videos, &output);
        append_json_media(engine, media, &output);
    }
    /* Audio is the one medium the reader plays itself, and no shape decision
       should be able to swallow it: a podcast page is an article, a station
       page is a link index, and the player is the point of both. Rendered once
       here rather than per branch, after whatever the shape chose. */
    append_json_audio(engine, audio, &output);
    if (embedded_mapped) append_json_navigation(engine, embedded_navigation, &output);
    dbuf_puts(&output, "</main></body></html>");
    free(shape_kind);
    free(title);
    free(main_heading);
    free(content);
    free(text_dir);
    free(text_lang);
    JS_FreeValue(engine->context, shape);
    JS_FreeValue(engine->context, blocks);
    JS_FreeValue(engine->context, tables);
    JS_FreeValue(engine->context, results);
    JS_FreeValue(engine->context, forms);
    JS_FreeValue(engine->context, videos);
    JS_FreeValue(engine->context, audio);
    JS_FreeValue(engine->context, media);
    JS_FreeValue(engine->context, links);
    JS_FreeValue(engine->context, embedded_blocks);
    JS_FreeValue(engine->context, embedded_navigation);
    JS_FreeValue(engine->context, embedded_data);
    JS_FreeValue(engine->context, main_blocks);
    JS_FreeValue(engine->context, main_content);
    JS_FreeValue(engine->context, readability);
    JS_FreeValue(engine->context, parsed);
    return dbuf_take(&output);
}

#endif /* DISTILL_READER */

#ifdef DISTILL_RENDER
/* local_path is a file the engine has already downloaded, or empty. When it
   is there the viewer shows the image; when it is not it shows what it knows
   about the image instead, which is the honest state before a fetch has
   happened or after one has failed. */
static char *viewer_template(Engine *engine, const char *json, int media_index,
                             const char *local_path)
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
        /* Deliberately plain block layout. An earlier version centred the
           image with flexbox inside a calc() height, and litehtml drew a
           fraction of it.

           The stage is full bleed and everything else is inset with margins,
           for a reason worth writing down: the renderer fits an image to the
           surface width, so a stage inset by N would lose N pixels off the
           right of every wide image. Horizontal padding is avoided because in
           this litehtml build a padding rule shifted sibling blocks as well
           as its own, which moved the stage and clipped the image again. */
        "html,body{margin:0;background:#17191b;color:#f5f1e8;}"
        "body{font-family:sans-serif;padding:26px 0;}"
        ".top{font-size:.82em;color:#aaa79f;margin:0 30px 20px;}"
        ".stage{background:#24272a;margin:0 0 18px;}"
        ".stage img{display:block;}"
        ".placeholder{padding:60px 20px;text-align:center;background:#3a3f42;color:#d4d0c7;font-size:1.2em;}"
        ".caption{margin:0 30px;}.caption strong{font-size:1.2em;}"
        ".url{color:#aaa79f;font-size:.8em;margin:4px 30px 16px;}"
        ".nav{color:#78b9cf;margin:0 30px;}a{color:#78b9cf;text-decoration:none;}.nav a{margin-right:16px;}"
        "</style></head><body><div class=\"top\">Distill media viewer · ");
    dbuf_printf(&output, "%d of %lld · %s", media_index + 1, (long long)count,
                d_eq(kind, "decorative") ? "decorative reference" : "content reference");
    dbuf_puts(&output, "</p><div class=\"stage\">");
    DImage decoded = {0};
    bool shown = local_path && *local_path && d_image_load_file(local_path, &decoded);
    if (shown) {
        /* No width or height: the renderer fits the image to the surface and
           an explicit size would stop it being drawn at all. */
        d_image_free(&decoded);
        dbuf_puts(&output, "<img src=\"");
        append_html_escaped(&output, local_path);
        dbuf_puts(&output, "\" alt=\"");
        append_html_escaped(&output, alt);
        dbuf_puts(&output, "\">");
    } else {
        dbuf_puts(&output, "<div class=\"placeholder\">");
        if (local_path && *local_path) append_html_escaped(&output, d_image_last_error());
        else append_html_escaped(&output, *alt ? alt : "Image reference");
        dbuf_puts(&output, "</div>");
    }
    dbuf_puts(&output, "</div><div class=\"caption\"><strong>");
    append_html_escaped(&output, *alt ? alt : "Image reference");
    dbuf_puts(&output, "</strong><div class=\"url\">");
    append_display_url(&output, url);
    dbuf_puts(&output, "</div><div class=\"nav\">");
    if (media_index > 0) dbuf_puts(&output, "<a href=\"#previous\">Previous</a> · ");
    else dbuf_puts(&output, "Previous · ");
    if (media_index + 1 < count) dbuf_puts(&output, "<a href=\"#next\">Next</a>");
    else dbuf_puts(&output, "Next");
    dbuf_puts(&output, "</div></body></html>");
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

static bool write_file_bytes(const char *path, const char *data, size_t length);

#ifdef DISTILL_READER
/* The reader page laid out in character cells, as the terminal would show
   it, written as plain text. Rows 0 means the whole page. */
static bool write_cells(const char *template, int columns, int rows, const char *path)
{
    ReaderLayout *layout = reader_layout(template, &(ReaderLayoutOptions){.columns = columns});
    if (!layout) return false;
    char *text = reader_layout_dump(layout, 0, rows);
    bool ok = write_file_bytes(path, text, strlen(text));
    free(text);
    reader_layout_free(layout);
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
    /* Cookies live in the share, not in any one easy handle, so closing the
       request handles does not reliably write them out. Flush explicitly,
       while the share is still alive, or a login is lost at exit. */
    if (engine->cookie_jar_persistent && engine->cookie_jar_path && engine->share) {
        CURL *flush = curl_easy_init();
        if (flush) {
            curl_easy_setopt(flush, CURLOPT_SHARE, engine->share);
            /* Read before writing: a run that set no cookies must not
               truncate a session file that already holds some. */
            curl_easy_setopt(flush, CURLOPT_COOKIEFILE, engine->cookie_jar_path);
            curl_easy_setopt(flush, CURLOPT_COOKIEJAR, engine->cookie_jar_path);
            curl_easy_setopt(flush, CURLOPT_COOKIELIST, "FLUSH");
            curl_easy_cleanup(flush);
        }
    }
    if (engine->multi) curl_multi_cleanup(engine->multi);
    if (engine->share) curl_share_cleanup(engine->share);
    /* A per-run jar is scratch and is removed. A session file belongs to the
       user and is theirs to delete. */
    if (engine->cookie_jar_path && !engine->cookie_jar_persistent) unlink(engine->cookie_jar_path);
    free(engine->cookie_jar_path);
    free(engine->script_cookies);
    free(engine->source_url); free(engine->base_url);
    *engine = (Engine){0};
}

/* A session file holds live credentials: anything able to read it can act as
   the user on every site in it. It is created private and refused if it is
   not, rather than silently writing cookies into a file others can read. */
static bool prepare_session_file(Engine *engine, const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0) {
        if (!S_ISREG(info.st_mode)) {
            fprintf(stderr, "session file %s is not a regular file\n", path);
            return false;
        }
        if (info.st_mode & (S_IRWXG | S_IRWXO)) {
            fprintf(stderr,
                    "refusing session file %s: mode %04o is accessible to other users; "
                    "run chmod 600 on it\n",
                    path, (unsigned)(info.st_mode & 07777));
            return false;
        }
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            fprintf(stderr, "unable to create session file %s: %s\n", path, strerror(errno));
            return false;
        }
        close(fd);
    }
    engine->cookie_jar_path = d_strdup(path);
    engine->cookie_jar_persistent = true;
    return true;
}

static bool init_network(Engine *engine, const char *session_file)
{
    if (session_file && *session_file) {
        if (!prepare_session_file(engine, session_file)) return false;
    } else {
        char cookie_template[] = "/tmp/distill-cookies-XXXXXX";
        int cookie_fd = mkstemp(cookie_template);
        if (cookie_fd < 0) return false;
        close(cookie_fd);
        engine->cookie_jar_path = d_strdup(cookie_template);
        engine->cookie_jar_persistent = false;
    }
    engine->share = curl_share_init();
    if (!engine->share ||
        curl_share_setopt(engine->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE) != CURLSHE_OK) {
        if (engine->share) curl_share_cleanup(engine->share);
        engine->share = NULL;
        if (!engine->cookie_jar_persistent) unlink(engine->cookie_jar_path);
        free(engine->cookie_jar_path);
        engine->cookie_jar_path = NULL;
        return false;
    }
    engine->multi = curl_multi_init();
    if (!engine->multi) {
        curl_share_cleanup(engine->share);
        engine->share = NULL;
        if (!engine->cookie_jar_persistent) unlink(engine->cookie_jar_path);
        free(engine->cookie_jar_path);
        engine->cookie_jar_path = NULL;
        return false;
    }
    return true;
}

static bool set_document_base(Engine *engine)
{
    const char *source =
        "(function(){var b=document.querySelector('base');if(b)return b.getAttribute('href') || '';"
        "if(location.protocol === 'file:'){var c=document.querySelector('link[rel=canonical]');"
        "return c ? (c.getAttribute('href') || '') : '';}return '';})()";
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

/* The formats the audio core actually decodes, recognised from the bytes
   rather than from a served content type the page controls. Keeping this in
   step with media_is_playable in the extractor is the point: a card offers
   Play only for these, and the fetch refuses anything else. */
static const char *audio_format_name(const unsigned char *data, size_t length)
{
    if (!data || length < 12) return "";
    if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) return "WAV";
    if (memcmp(data, "fLaC", 4) == 0) return "FLAC";
    if (memcmp(data, "ID3", 3) == 0) return "MP3";
    /* A bare MPEG audio frame: sync bits set, and neither a reserved layer
       nor a reserved bitrate index. */
    if (data[0] == 0xff && (data[1] & 0xe0) == 0xe0 &&
        (data[1] & 0x06) != 0x00 && (data[2] & 0xf0) != 0xf0) return "MP3";
    return "";
}

/* Not every URL is a document. A link to an .mp3 is an ordinary link on an
   ordinary page, and following it handed 600 KB of audio to the HTML parser,
   which produced an empty document and a reader page with no title and
   nothing on it.
 *
 * The bytes decide, as everywhere else here: a served content type is page
 * controlled. What comes back is a small document describing the resource,
 * so the rest of the pipeline treats it as any other page and the reader
 * renders the same audio or video card it would render for an embedded one.
 */
typedef struct { const char *kind; const char *mime; } MediaResource;

static MediaResource media_resource_kind(const unsigned char *data, size_t length)
{
    if (!data || length < 12) return (MediaResource){NULL, NULL};
    const char *audio = audio_format_name(data, length);
    if (*audio) {
        if (d_eq(audio, "WAV")) return (MediaResource){"audio", "audio/wav"};
        if (d_eq(audio, "FLAC")) return (MediaResource){"audio", "audio/flac"};
        return (MediaResource){"audio", "audio/mpeg"};
    }
    /* Ogg carries either, and the codec name is inside the first page; the
       reader offers a download for both, so the distinction can wait. */
    if (memcmp(data, "OggS", 4) == 0) return (MediaResource){"audio", "audio/ogg"};
    if (memcmp(data + 4, "ftyp", 4) == 0) {
        const unsigned char *brand = data + 8;
        if (memcmp(brand, "M4A", 3) == 0) return (MediaResource){"audio", "audio/mp4"};
        return (MediaResource){"video", "video/mp4"};
    }
    if (memcmp(data, "\x1a\x45\xdf\xa3", 4) == 0) return (MediaResource){"video", "video/webm"};
    if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return (MediaResource){"image", "image/png"};
    if (memcmp(data, "\xff\xd8\xff", 3) == 0) return (MediaResource){"image", "image/jpeg"};
    if (memcmp(data, "GIF8", 4) == 0) return (MediaResource){"image", "image/gif"};
    if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
        return (MediaResource){"image", "image/webp"};
    if (memcmp(data, "%PDF-", 5) == 0) return (MediaResource){"document", "application/pdf"};
    return (MediaResource){NULL, NULL};
}

/* The reader's own escaper lives in the render-only half of this file, and
   this document has to be built in every build. */
static void media_escaped(DBuffer *out, const char *text)
{
    for (const char *at = text ? text : ""; *at; ++at) {
        switch (*at) {
            case '&': dbuf_puts(out, "&amp;"); break;
            case '<': dbuf_puts(out, "&lt;"); break;
            case '>': dbuf_puts(out, "&gt;"); break;
            case '"': dbuf_puts(out, "&quot;"); break;
            default: dbuf_printf(out, "%c", *at); break;
        }
    }
}

/* The last path segment, which is the only name a bare file has. */
static char *media_resource_name(const char *url)
{
    const char *tail = url ? strrchr(url, '/') : NULL;
    char *name = d_strdup(tail && tail[1] ? tail + 1 : (url ? url : "file"));
    char *cut = strpbrk(name, "?#");
    if (cut) *cut = '\0';
    if (!*name) { free(name); name = d_strdup("file"); }
    return name;
}

/* A document standing in for the resource. Deliberately built from the same
   markup a page would use, so extraction, shape classification and the reader
   template all take their ordinary paths and there is no second way for a
   media card to be produced. */
static char *media_resource_document(const MediaResource *resource, const char *url,
                                     size_t bytes)
{
    char *name = media_resource_name(url);
    DBuffer out; dbuf_init(&out);
    dbuf_puts(&out, "<!doctype html><html><head><meta charset=\"utf-8\"><title>");
    media_escaped(&out, name);
    dbuf_puts(&out, "</title></head><body><main><h1>");
    media_escaped(&out, name);
    dbuf_puts(&out, "</h1>");
    if (d_eq(resource->kind, "audio") || d_eq(resource->kind, "video")) {
        bool audio = d_eq(resource->kind, "audio");
        dbuf_puts(&out, audio ? "<audio title=\"" : "<video title=\"");
        media_escaped(&out, name);
        dbuf_puts(&out, "\"><source src=\"");
        media_escaped(&out, url ? url : "");
        dbuf_puts(&out, "\" type=\"");
        media_escaped(&out, resource->mime ? resource->mime : "");
        dbuf_puts(&out, "\"></");
        dbuf_puts(&out, audio ? "audio>" : "video>");
    } else if (d_eq(resource->kind, "image")) {
        dbuf_puts(&out, "<img src=\"");
        media_escaped(&out, url ? url : "");
        dbuf_puts(&out, "\" alt=\"");
        media_escaped(&out, name);
        dbuf_puts(&out, "\">");
    } else {
        /* Nothing this reader renders, but still something it can hand over
           rather than pretend to have opened. */
        dbuf_puts(&out, "<p>This is a ");
        media_escaped(&out, resource->mime ? resource->mime : "binary");
        dbuf_printf(&out, " file of %zu bytes. The reader does not display it, "
                          "but it can be saved.</p><p><a href=\"", bytes);
        media_escaped(&out, url ? url : "");
        dbuf_puts(&out, "\">");
        media_escaped(&out, name);
        dbuf_puts(&out, "</a></p>");
    }
    dbuf_puts(&out, "</main></body></html>");
    free(name);
    return dbuf_take(&out);
}

static int run_page(const char *input, uint64_t cap_ms, uint64_t memory_mb,
                    uint64_t max_scripts, bool dump_dom, bool extract,
                    const char *render_path, const char *viewer_path, int media_index,
                    const char *save_media_path, const char *fetch_media_path,
                    const char *fetch_kind,
                    int render_width, int render_height,
                    const char *font_dir, const char *template_path,
                    const char *markdown_path,
                    const char *cells_path, int cell_columns, int cell_rows,
                    const char *theme, bool native_forms, int zoom,
                    const char *request_method,
                    const char *request_data, const char *session_file,
                    bool progress, bool https_optional)
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
    engine.progress = progress;
    engine.memory_limit = memory_mb * 1024u * 1024u;
    engine.max_scripts = max_scripts;
    engine.source_url = absolute_input(input);
    engine.base_url = d_strdup(engine.source_url);
    engine.started = clock_ms();
    if (!init_network(&engine, session_file)) {
        fprintf(stderr, "unable to initialize network state\n");
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        free(engine.source_url); free(engine.base_url);
        return 2;
    }

    DBuffer html;
    long status = 0;
    char *effective_url = NULL;
    CURLcode transport = CURLE_OK;
    report_progress(&engine, PROGRESS_FETCH_START, "fetch");
    bool loaded = fetch_sync_request(&engine, engine.source_url, &html, &status,
                                     &effective_url, request_method, request_data,
                                     &transport);
    /* The shell guesses https:// for a bare hostname typed in the address bar,
       the way a browser does, and says so by passing --https-optional. A host
       with no TLS service at all never opens the connection, so there is no
       response to read and no redirect to follow: the only thing wrong is the
       scheme nobody chose deliberately. Try the other one, once.

       Only when the connection never opened. A certificate this run distrusts
       means a server that does have TLS, and stepping down to plaintext there
       would be taking an attacker's advice. */
    if (!loaded && https_optional && transport_never_opened(transport) &&
        d_starts(engine.source_url, "https://")) {
        char *plain = d_cat2("http://", engine.source_url + strlen("https://"));
        dbuf_free(&html);
        free(effective_url);
        effective_url = NULL;
        free(engine.source_url);
        engine.source_url = plain;
        free(engine.base_url);
        engine.base_url = d_strdup(engine.source_url);
        loaded = fetch_sync_request(&engine, engine.source_url, &html, &status,
                                    &effective_url, request_method, request_data, NULL);
    }
    if (!loaded) {
        fprintf(stderr, "unable to load %s (status %ld)\n", engine.source_url, status);
        dbuf_free(&html);
        engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        free(effective_url);
        return 2;
    }
    /* Keep the served representation alive for the whole run. Page scripts
       are allowed to replace the body or leave an error shell behind, and a
       settled DOM is not a reversible snapshot of what the server sent. */
    char *initial_html = d_strndup(html.data, html.length);
    if (effective_url && *effective_url) { free(engine.source_url); engine.source_url = effective_url; }
    else free(effective_url);
    free(engine.base_url); engine.base_url = d_strdup(engine.source_url);
    report_location(&engine);
#ifdef DISTILL_READER
    /* One request and nothing else: no runtime, no scripts, no extraction.
       The shell has no network of its own -- only the engine links libcurl --
       so this is how an image a reader activated reaches the viewer, and
       re-running a whole page to fetch one file it already named would cost
       seconds for no new information.

       The bytes decide whether this is an image. A served content type is
       page controlled, and a file that says image/png while holding something
       else must not reach a decoder as if it were a PNG. */
    if (fetch_media_path) {
        /* What the bytes are allowed to be depends on what the caller will do
           with them. The viewer decodes an image, so it keeps the strict check
           that has always been here. The audio core decodes a sound file, so
           it gets the same treatment against audio magic. A download decodes
           nothing at all -- it is a copy of the source file, unmodified -- so
           it takes whatever the server sent. */
        const char *format = d_eq(fetch_kind, "audio")
                             ? audio_format_name((const unsigned char *)html.data, html.length)
                             : d_eq(fetch_kind, "any")
                             ? "file"
                             : d_image_format_name((const unsigned char *)html.data, html.length);
        /* An error page is not the file that was asked for, whatever its
           bytes happen to be: a download of a 404 must fail, not save the
           server's apology under the name of a song. */
        if (status >= 400) {
            fprintf(stderr, "the server answered %ld for %s\n", status, engine.source_url);
            dbuf_free(&html);
            free(initial_html);
            engine_cleanup(&engine);
#ifdef DISTILL_RENDER
            free(owned_font_dir);
#endif
            return 2;
        }
        bool ok = *format && write_file_bytes(fetch_media_path, html.data, html.length);
        if (!ok && !*format)
            fprintf(stderr, "not %s: %s\n",
                    d_eq(fetch_kind, "audio") ? "audio this reader can decode" : "an image",
                    engine.source_url);
        else if (!ok)
            fprintf(stderr, "unable to write %s\n", fetch_media_path);
        else
            fprintf(stderr, "fetched %s, %zu bytes\n", format, html.length);
        dbuf_free(&html);
        free(initial_html);
        engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return ok ? 0 : 2;
    }
#endif
    /* A resource that is not a document must not be parsed as one. Stand a
       small document in for it before anything else looks at the bytes, so
       extraction, shape classification and the reader template all run their
       ordinary course and produce the same card an embedded reference gets. */
    MediaResource resource = media_resource_kind((const unsigned char *)html.data, html.length);
    if (resource.kind) {
        fprintf(stderr, "%s resource (%s), %zu bytes: presenting it rather than parsing it\n",
                resource.kind, resource.mime, html.length);
        char *document = media_resource_document(&resource, engine.source_url, html.length);
        dbuf_free(&html);
        dbuf_init(&html);
        dbuf_puts(&html, document);
        free(initial_html);
        initial_html = d_strdup(document);
        free(document);
    }
    engine.challenge = challenge_kind(html.data, html.length,
                                      DISTILL_CHALLENGE_SERVED_BYTES);
    if (engine.challenge) {
        fprintf(stderr, "challenge page detected (%s) for %s; skipping script execution\n",
                engine.challenge, engine.source_url);
    }

    engine.runtime = JS_NewRuntime();
    if (!engine.runtime) {
        dbuf_free(&html); free(initial_html); engine_cleanup(&engine);
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
        dbuf_free(&html); free(initial_html); engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    JS_SetContextOpaque(engine.context, &engine);
    JS_SetModuleLoaderFunc(engine.runtime, module_normalize, module_loader, &engine);
    engine.global = JS_GetGlobalObject(engine.context);
    if (!engine.multi || !load_linkedom(&engine, &html) || !install_host(&engine)) {
        dbuf_free(&html); free(initial_html); engine_cleanup(&engine);
#ifdef DISTILL_RENDER
        free(owned_font_dir);
#endif
        return 2;
    }
    dbuf_free(&html);
    set_document_base(&engine);
    bool needs_extraction = viewer_path || render_path || template_path || markdown_path ||
                            cells_path || extract;
    char *initial_json = NULL;
    report_progress(&engine, PROGRESS_FETCH_END, "scripts");
    bool scripts_failed = false;
    if (!engine.challenge) {
        if (!discover_scripts(&engine)) {
            /* Script discovery can be interrupted by a page whose DOM or
               inline data is too costly for the compatibility layer to walk.
               The fetched response and its extraction are still valid; keep
               them and continue with the untouched document instead of
               turning a script failure into a transport failure. */
            fprintf(stderr, "script discovery failed; continuing with initial document\n");
            scripts_failed = true;
        } else {
            settle(&engine);
        }
    }
    /* The settle cap stops page activity; it must not interrupt our own
       final DOM snapshot or extraction pass. */
    JS_SetInterruptHandler(engine.runtime, NULL, NULL);
    report_progress(&engine, PROGRESS_SCRIPTS_END, "extract");
    char *serialized = serialize_document(&engine);
    /* Some challenges write their message from script, so the served HTML
       carries no marker and only the settled DOM does. Detecting it here is
       too late to save the settle budget, but the run is still classified
       correctly rather than reported as an empty extraction. */
    if (!engine.challenge) {
        engine.challenge = challenge_kind(serialized, strlen(serialized),
                                          DISTILL_CHALLENGE_SETTLED_BYTES);
        if (engine.challenge) {
            fprintf(stderr, "challenge page detected (%s) for %s after script execution\n",
                    engine.challenge, engine.source_url);
        }
    }
    int result = 0;
    char *selected_json = NULL;
    if (needs_extraction) {
        char *settled_json = NULL;
        if (scripts_failed || engine.cap_hit) {
            initial_json = run_extraction(&engine, initial_html, false);
            ExtractionCandidate initial_candidate = score_extraction(&engine, initial_json);
            if (scripts_failed || initial_candidate.useful)
                settled_json = d_strdup("");
            else
                settled_json = run_extraction(&engine, serialized, true);
        } else {
            settled_json = run_extraction(&engine, serialized, true);
            ExtractionCandidate settled_candidate = score_extraction(&engine, settled_json);
            if (!settled_candidate.useful || settled_candidate.error_like)
                initial_json = run_extraction(&engine, initial_html, true);
        }
        selected_json = select_extraction(&engine, initial_json, settled_json, scripts_failed);
        free(settled_json);
        free(initial_json);
        initial_json = NULL;
    }
    report_progress(&engine, PROGRESS_EXTRACT_END, "render");
    if (viewer_path) {
#ifdef DISTILL_RENDER
        char *json = selected_json;
        selected_json = NULL;
        bool saved = !save_media_path || save_media_file(&engine, json, media_index, save_media_path);
        char *template = viewer_template(&engine, json, media_index,
                                         saved ? save_media_path : NULL);
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
        char *json = selected_json;
        selected_json = NULL;
        char *template = reader_template(&engine, json, theme, native_forms, zoom);
        bool template_ok = !template_path || write_file_bytes(template_path, template, strlen(template));
        /* Height 0 means "as tall as the content". Measure here rather than
           inside the renderer so the caller is told what it actually got: the
           desktop shell sizes its scroll area from this. */
        int used_height = render_height;
        if (used_height <= 0 && *template) {
            used_height = distill_litehtml_content_height(template, render_width, font_dir);
            if (used_height < 1) used_height = 1;
            if (used_height > DISTILL_MAX_RENDER_HEIGHT) used_height = DISTILL_MAX_RENDER_HEIGHT;
        }
        if (!template_ok || !*template || !distill_litehtml_render(template, render_path,
                                                                     render_width, used_height, font_dir)) {
            fprintf(stderr, "unable to render %s\n", render_path);
            result = 3;
        } else {
            fprintf(stderr, "rendered %s (%dx%d)\n", render_path, render_width, used_height);
        }
        free(template);
        free(json);
#else
        (void)render_width; (void)render_height; (void)font_dir;
        fprintf(stderr, "this build has no litehtml renderer\n");
        result = 2;
#endif
    } else if (markdown_path) {
#ifdef DISTILL_READER
        /* The export is a conversion of the reader page, not a second
           rendering of the extraction: what it writes is what the reader
           shows, and a change to the template carries into it untold. */
        char *json = selected_json;
        selected_json = NULL;
        char *template = reader_template(&engine, json, theme, native_forms, zoom);
        char *markdown = *template ? reader_markdown(template, engine.source_url)
                                   : d_strdup("");
        if (!*markdown || !write_file_bytes(markdown_path, markdown, strlen(markdown))) {
            fprintf(stderr, "unable to write markdown %s\n", markdown_path);
            result = 3;
        }
        free(markdown);
        free(template);
        free(json);
#else
        (void)theme; (void)zoom;
        fprintf(stderr, "this build has no reader page\n");
        result = 2;
#endif
    } else if (cells_path) {
#ifdef DISTILL_READER
        char *json = selected_json;
        selected_json = NULL;
        char *template = reader_template(&engine, json, theme, native_forms, zoom);
        if (!*template || !write_cells(template, cell_columns, cell_rows, cells_path)) {
            fprintf(stderr, "unable to write cells %s\n", cells_path);
            result = 3;
        }
        free(template);
        free(json);
#else
        (void)cell_columns; (void)cell_rows;
        fprintf(stderr, "this build has no reader page\n");
        result = 2;
#endif
    } else if (template_path) {
#ifdef DISTILL_READER
        char *json = selected_json;
        selected_json = NULL;
        char *template = reader_template(&engine, json, theme, native_forms, zoom);
        if (!*template || !write_file_bytes(template_path, template, strlen(template))) {
            fprintf(stderr, "unable to write reader template %s\n", template_path);
            result = 3;
        }
        free(template);
        free(json);
#else
        (void)theme; (void)zoom;
        fprintf(stderr, "this build has no reader page\n");
        result = 2;
#endif
    } else if (extract) {
        char *json = selected_json;
        selected_json = NULL;
        puts(json);
        free(json);
    } else if (dump_dom) {
        puts(serialized);
#ifdef DISTILL_B_PRUNED
        fprintf(stderr, "arm-b-pruned litehtml=%s\n", distill_litehtml_probe(serialized) ? "ok" : "failed");
#endif
    }
    /* The output above is still produced so the challenge shell can be
       inspected; the status distinguishes it from a content run. */
    if (result == 0 && engine.challenge) result = 4;
    report_progress(&engine, 100, "done");
    fprintf(stderr, "%s scripts=%llu cap_hit=%s\n", viewer_path ? "phase2-viewer" : (render_path ? "phase2" : (extract ? "phase1" : "phase0")),
            (unsigned long long)engine.scripts_run, engine.cap_hit ? "true" : "false");
    free(selected_json);
    free(initial_json);
    free(initial_html);
    free(serialized);
    engine_cleanup(&engine);
#ifdef DISTILL_RENDER
    free(owned_font_dir);
#endif
    return result;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --url URL [--dump-dom|--extract|--render FILE|--template FILE|--markdown FILE|--dump-cells FILE|--viewer FILE|--fetch-media FILE] [--media-index N] [--save-media FILE] [--width N] [--height N|0] [--font-dir DIR] [--theme warm|night|paper] [--zoom PERCENT] [--native-forms] [--progress] [--https-optional] [--request-method GET|POST] [--request-data DATA|--request-data-file FILE|-] [--session-file FILE] [--script-cap-ms N] [--memory-limit-mb N] [--max-scripts N] [--version]\n", program);
    fprintf(stderr, "  --dump-cells FILE     lay the reader page out in character cells, as the terminal\n"
        "                        shows it, and write it as plain text. --columns N (default\n"
        "                        80) is the terminal width; --rows N (default 0, the whole\n"
        "                        page) how much of it to write. --cells-from PAGE converts a\n"
        "                        reader page already on disk, like --markdown-from.\n");
    fprintf(stderr, "  --height 0            size the render to the content instead of cropping it.\n"
        "  --fetch-media FILE    fetch one image and write its bytes, with no scripts and no\n"
        "                        extraction. The format is taken from the bytes, not the\n"
        "                        served content type, and a non-image is refused.\n");
    fprintf(stderr, "  --zoom PERCENT        reader type size, 50 to 300, default 100. The column and the\n"
        "                        gutters scale with it; the surface width caps both.\n");
    fprintf(stderr, "  --progress            report \"progress PERCENT PHASE\" on stdout as the run moves\n"
        "                        through fetch, scripts, extract and render. The phases are\n"
        "                        what is measured; the percentage is their weighting. Not\n"
        "                        available with --dump-dom or --extract, whose output is\n"
        "                        stdout itself.\n");
    fprintf(stderr, "  --session-file FILE   keep cookies across runs, so a login survives the next navigation.\n");
    fprintf(stderr, "                        Created mode 600 and refused if others can read it.\n");
    fprintf(stderr, "  --request-data-file F read the POST body from a file, or from stdin when F is \"-\".\n");
    fprintf(stderr, "                        Prefer this for credentials: --request-data puts them in argv,\n");
    fprintf(stderr, "                        which is visible in the process table and in shell history.\n");
}

/* A request body carrying a password must not reach argv: the command line is
   visible in the process table and lands in shell history. It is read from a
   file, or from stdin when the path is "-". */
static char *read_request_body(const char *path)
{
    if (d_eq(path, "-")) {
        DBuffer body; dbuf_init(&body);
        char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), stdin)) > 0) dbuf_append(&body, chunk, got);
        dbuf_append(&body, "", 1);
        return dbuf_take(&body);
    }
    size_t length = 0;
    char *data = d_read_file(path, &length);
    if (!data) {
        fprintf(stderr, "unable to read request body from %s: %s\n", path, strerror(errno));
        return NULL;
    }
    /* A trailing newline from an editor or a heredoc is not part of the body. */
    while (length && (data[length - 1] == '\n' || data[length - 1] == '\r')) data[--length] = '\0';
    return data;
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    const char *render_path = NULL;
    const char *viewer_path = NULL;
    const char *template_path = NULL;
    const char *markdown_path = NULL;
    const char *markdown_from = NULL;
    const char *cells_path = NULL;
    const char *cells_from = NULL;
    int cell_columns = 80, cell_rows = 0;
    const char *save_media_path = NULL;
    const char *fetch_media_path = NULL;
    const char *fetch_kind = "image";
    const char *font_dir = NULL;
    const char *theme = "warm";
    const char *request_method = NULL;
    const char *request_data = NULL;
    const char *request_data_file = NULL;
    const char *session_file = NULL;
    char *owned_request_data = NULL;
    uint64_t cap_ms = 5000, memory_mb = 128, max_scripts = 1000;
    int render_width = 760, render_height = 1100;
    int zoom = 100;
    int media_index = 0;
    bool dump_dom = false, extract = false, native_forms = false, progress = false;
    bool https_optional = false;
    for (int i = 1; i < argc; ++i) {
        if (d_eq(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (d_eq(argv[i], "--dump-dom")) dump_dom = true;
        else if (d_eq(argv[i], "--extract")) extract = true;
        else if (d_eq(argv[i], "--render") && i + 1 < argc) render_path = argv[++i];
        else if (d_eq(argv[i], "--template") && i + 1 < argc) template_path = argv[++i];
        else if (d_eq(argv[i], "--markdown") && i + 1 < argc) markdown_path = argv[++i];
        else if (d_eq(argv[i], "--markdown-from") && i + 1 < argc) markdown_from = argv[++i];
        else if (d_eq(argv[i], "--dump-cells") && i + 1 < argc) cells_path = argv[++i];
        else if (d_eq(argv[i], "--cells-from") && i + 1 < argc) cells_from = argv[++i];
        else if (d_eq(argv[i], "--columns") && i + 1 < argc) cell_columns = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--rows") && i + 1 < argc) cell_rows = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--viewer") && i + 1 < argc) viewer_path = argv[++i];
        else if (d_eq(argv[i], "--media-index") && i + 1 < argc) media_index = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--save-media") && i + 1 < argc) save_media_path = argv[++i];
        else if (d_eq(argv[i], "--fetch-media") && i + 1 < argc) fetch_media_path = argv[++i];
        else if (d_eq(argv[i], "--fetch-kind") && i + 1 < argc) fetch_kind = argv[++i];
        else if (d_eq(argv[i], "--width") && i + 1 < argc) render_width = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--height") && i + 1 < argc) render_height = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--font-dir") && i + 1 < argc) font_dir = argv[++i];
        else if (d_eq(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if (d_eq(argv[i], "--zoom") && i + 1 < argc) zoom = (int)strtol(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--native-forms")) native_forms = true;
        else if (d_eq(argv[i], "--progress")) progress = true;
        else if (d_eq(argv[i], "--https-optional")) https_optional = true;
        else if (d_eq(argv[i], "--request-method") && i + 1 < argc) request_method = argv[++i];
        else if (d_eq(argv[i], "--request-data") && i + 1 < argc) request_data = argv[++i];
        else if (d_eq(argv[i], "--request-data-file") && i + 1 < argc) request_data_file = argv[++i];
        else if (d_eq(argv[i], "--session-file") && i + 1 < argc) session_file = argv[++i];
        else if (d_eq(argv[i], "--script-cap-ms") && i + 1 < argc) cap_ms = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--memory-limit-mb") && i + 1 < argc) memory_mb = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--max-scripts") && i + 1 < argc) max_scripts = strtoull(argv[++i], NULL, 10);
        else if (d_eq(argv[i], "--help") || d_eq(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (d_eq(argv[i], "--version")) { puts("distill " DISTILL_VERSION); return 0; }
    }
    if (request_data && request_data_file) {
        fprintf(stderr, "use --request-data or --request-data-file, not both\n");
        return 2;
    }
    /* Converting a reader page already on disk. No URL, no fetch, no scripts:
       the exported document is the page the reader is showing, not a second
       reading of the same address, which may since have changed or may not be
       reachable again without the session that produced it. */
    if (markdown_from) {
#ifdef DISTILL_READER
        if (!markdown_path) { usage(argv[0]); return 2; }
        size_t length = 0;
        char *template = d_read_file(markdown_from, &length);
        if (!template) {
            fprintf(stderr, "unable to read reader page %s\n", markdown_from);
            return 2;
        }
        char *markdown = reader_markdown(template, url);
        bool ok = *markdown && write_file_bytes(markdown_path, markdown, strlen(markdown));
        if (!ok) fprintf(stderr, "unable to write markdown %s\n", markdown_path);
        free(markdown);
        free(template);
        return ok ? 0 : 3;
#else
        fprintf(stderr, "this build has no reader page\n");
        return 2;
#endif
    }
    if (cells_from) {
#ifdef DISTILL_READER
        if (!cells_path || cell_columns < 8 || cell_rows < 0) { usage(argv[0]); return 2; }
        size_t length = 0;
        char *template = d_read_file(cells_from, &length);
        if (!template) {
            fprintf(stderr, "unable to read reader page %s\n", cells_from);
            return 2;
        }
        bool ok = write_cells(template, cell_columns, cell_rows, cells_path);
        if (!ok) fprintf(stderr, "unable to write cells %s\n", cells_path);
        free(template);
        return ok ? 0 : 3;
#else
        fprintf(stderr, "this build has no reader page\n");
        return 2;
#endif
    }
    if ((cells_path && (cell_columns < 8 || cell_rows < 0)) ||
        (cells_path && (dump_dom || extract || render_path || template_path || viewer_path ||
                        markdown_path || fetch_media_path))) {
        usage(argv[0]); return 2;
    }
    if (!url || !*url || (dump_dom && (extract || render_path || template_path || viewer_path)) ||
        (extract && (render_path || template_path || viewer_path)) ||
        (template_path && viewer_path) || (render_path && viewer_path) ||
        (save_media_path && !viewer_path) || media_index < 0 ||
        (fetch_media_path && (dump_dom || extract || render_path || template_path || viewer_path)) ||
        render_width <= 0 || render_height < 0 ||
        zoom < READER_ZOOM_MIN || zoom > READER_ZOOM_MAX ||
        (progress && (dump_dom || extract)) ||
        ((request_data || request_data_file) && (!request_method || !d_ieq(request_method, "post"))) ||
        (request_method && !d_ieq(request_method, "get") && !d_ieq(request_method, "post"))) {
        usage(argv[0]); return 2;
    }
    if (request_data_file) {
        owned_request_data = read_request_body(request_data_file);
        if (!owned_request_data) return 2;
        request_data = owned_request_data;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = run_page(url, cap_ms, memory_mb, max_scripts, dump_dom, extract,
                          render_path, viewer_path, media_index, save_media_path,
                          fetch_media_path,
                          fetch_kind,
                          render_width, render_height, font_dir, template_path,
                          markdown_path, cells_path, cell_columns, cell_rows, theme,
                          native_forms, zoom,
                          request_method, request_data, session_file, progress,
                          https_optional);
    curl_global_cleanup();
    /* The body may hold a password; do not leave it in freed-but-readable
       heap longer than necessary. */
    if (owned_request_data) {
        memset(owned_request_data, 0, strlen(owned_request_data));
        free(owned_request_data);
    }
    return result;
}
