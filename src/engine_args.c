#include "engine_args.h"

#include "cutil.h"

#include <errno.h>

typedef struct {
    const char **items;
    size_t length, capacity;
} Tokens;

static void push_token(Tokens *tokens, const char *token)
{
    d_vec_reserve((void **)&tokens->items, &tokens->capacity, tokens->length,
                  sizeof(*tokens->items), tokens->length + 1);
    tokens->items[tokens->length++] = token;
}

static char *keep(DEngineArgs *args, char *text)
{
    d_vec_reserve((void **)&args->owned, &args->owned_capacity, args->owned_count,
                  sizeof(*args->owned), args->owned_count + 1);
    args->owned[args->owned_count++] = text;
    return text;
}

static char *read_all_stdin(void)
{
    DBuffer body;
    dbuf_init(&body);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), stdin)) > 0) dbuf_append(&body, chunk, got);
    dbuf_append(&body, "", 1);
    return dbuf_take(&body);
}

static char *message(const char *format, const char *detail)
{
    size_t length = strlen(format) + strlen(detail) + 1;
    char *text = d_xmalloc(length);
    snprintf(text, length, format, detail);
    return text;
}

/* A request file, as tokens: "--name" and, where there is one, its value. */
static bool read_request_file(DEngineArgs *args, const char *path, bool *stdin_used,
                              Tokens *tokens, char **error)
{
    char *text;
    if (d_eq(path, "-")) {
        if (*stdin_used) { *error = d_strdup("stdin can hold only one of the request and its body"); return false; }
        *stdin_used = true;
        text = read_all_stdin();
    } else {
        size_t length = 0;
        text = d_read_file(path, &length);
        if (!text) { *error = message("unable to read request file %s", path); return false; }
    }
    keep(args, text);
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' ')) line[--length] = '\0';
        while (*line == ' ' || *line == '\t') ++line;
        if (*line && *line != '#') {
            char *space = strchr(line, ' ');
            if (space) *space = '\0';
            if (d_eq(line, "request")) { *error = d_strdup("a request file cannot name another"); return false; }
            push_token(tokens, keep(args, d_cat2("--", line)));
            if (space) {
                char *value = space + 1;
                while (*value == ' ') ++value;
                push_token(tokens, value);
            }
        }
        line = end ? end + 1 : NULL;
    }
    return true;
}

/* A request body carrying a password must not reach argv: the command line is
   visible in the process table and lands in shell history. It is read from a
   file, or from stdin when the path is "-". */
static char *read_request_body(const char *path, bool *stdin_used, char **error)
{
    if (d_eq(path, "-")) {
        if (*stdin_used) { *error = d_strdup("stdin can hold only one of the request and its body"); return NULL; }
        *stdin_used = true;
        return read_all_stdin();
    }
    size_t length = 0;
    char *data = d_read_file(path, &length);
    if (!data) {
        size_t size = strlen(path) + 160;
        *error = d_xmalloc(size);
        snprintf(*error, size, "unable to read request body from %s: %s", path, strerror(errno));
        return NULL;
    }
    /* A trailing newline from an editor or a heredoc is not part of the body. */
    while (length && (data[length - 1] == '\n' || data[length - 1] == '\r')) data[--length] = '\0';
    return data;
}

int d_engine_args_parse(DEngineArgs *args, int argc, const char *const *argv, char **error)
{
    *args = (DEngineArgs){0};
    *error = NULL;
    DEngineRequest *r = &args->request;
    d_engine_request_init(r);
    bool stdin_used = false;
    Tokens tokens = {0};
    for (int i = 1; i < argc; ++i) {
        if (d_eq(argv[i], "--request") && i + 1 < argc) {
            if (!read_request_file(args, argv[++i], &stdin_used, &tokens, error)) {
                free(tokens.items);
                return 2;
            }
        } else {
            push_token(&tokens, argv[i]);
        }
    }
    const char *request_data_file = NULL;
    const char *request_data = NULL;
    const char **t = tokens.items;
    size_t n = tokens.length;
    for (size_t i = 0; i < n; ++i) {
        bool more = i + 1 < n;
        if (d_eq(t[i], "--url") && more) r->url = t[++i];
        else if (d_eq(t[i], "--dump-dom")) r->dump_dom = true;
        else if (d_eq(t[i], "--extract")) r->extract = true;
        else if (d_eq(t[i], "--render") && more) r->render_path = t[++i];
        else if (d_eq(t[i], "--template") && more) r->template_path = t[++i];
        else if (d_eq(t[i], "--markdown") && more) r->markdown_path = t[++i];
        else if (d_eq(t[i], "--markdown-from") && more) args->markdown_from = t[++i];
        else if (d_eq(t[i], "--dump-cells") && more) r->cells_path = t[++i];
        else if (d_eq(t[i], "--cells-from") && more) args->cells_from = t[++i];
        else if (d_eq(t[i], "--columns") && more) r->cell_columns = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--rows") && more) r->cell_rows = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--viewer") && more) r->viewer_path = t[++i];
        else if (d_eq(t[i], "--media-index") && more) r->media_index = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--save-media") && more) r->save_media_path = t[++i];
        else if (d_eq(t[i], "--fetch-media") && more) r->fetch_media_path = t[++i];
        else if (d_eq(t[i], "--fetch-kind") && more) r->fetch_kind = t[++i];
        else if (d_eq(t[i], "--width") && more) r->render_width = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--height") && more) r->render_height = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--font-dir") && more) r->font_dir = t[++i];
        else if (d_eq(t[i], "--theme") && more) r->theme = t[++i];
        else if (d_eq(t[i], "--zoom") && more) r->zoom = (int)strtol(t[++i], NULL, 10);
        else if (d_eq(t[i], "--native-forms")) r->native_forms = true;
        else if (d_eq(t[i], "--progress")) r->progress = true;
        else if (d_eq(t[i], "--https-optional")) r->https_optional = true;
        else if (d_eq(t[i], "--request-method") && more) r->request_method = t[++i];
        else if (d_eq(t[i], "--request-data") && more) request_data = t[++i];
        else if (d_eq(t[i], "--request-data-file") && more) request_data_file = t[++i];
        else if (d_eq(t[i], "--session-file") && more) r->session_file = t[++i];
        else if (d_eq(t[i], "--script-cap-ms") && more) r->cap_ms = strtoull(t[++i], NULL, 10);
        else if (d_eq(t[i], "--memory-limit-mb") && more) r->memory_mb = strtoull(t[++i], NULL, 10);
        else if (d_eq(t[i], "--max-scripts") && more) r->max_scripts = strtoull(t[++i], NULL, 10);
        else if (d_eq(t[i], "--max-fetches") && more) r->max_fetches = strtoull(t[++i], NULL, 10);
        else if (d_eq(t[i], "--js-stack-kib") && more) r->js_stack_bytes = (size_t)strtoull(t[++i], NULL, 10) * 1024u;
        else if (d_eq(t[i], "--help") || d_eq(t[i], "-h")) args->help = true;
        else if (d_eq(t[i], "--version")) args->version = true;
    }
    free(tokens.items);
    if (args->help || args->version) return 0;
    if (request_data && request_data_file) {
        *error = d_strdup("use --request-data or --request-data-file, not both");
        return 2;
    }
    if (args->markdown_from) return r->markdown_path ? 0 : 2;
    if (args->cells_from) return r->cells_path && r->cell_columns >= 8 && r->cell_rows >= 0 ? 0 : 2;
    if ((r->cells_path && (r->cell_columns < 8 || r->cell_rows < 0)) ||
        (r->cells_path && (r->dump_dom || r->extract || r->render_path || r->template_path ||
                           r->viewer_path || r->markdown_path || r->fetch_media_path)))
        return 2;
    if (!r->url || !*r->url || (r->dump_dom && (r->extract || r->render_path || r->template_path || r->viewer_path)) ||
        (r->extract && (r->render_path || r->template_path || r->viewer_path)) ||
        (r->template_path && r->viewer_path) || (r->render_path && r->viewer_path) ||
        (r->save_media_path && !r->viewer_path) || r->media_index < 0 ||
        (r->fetch_media_path && (r->dump_dom || r->extract || r->render_path || r->template_path || r->viewer_path)) ||
        r->render_width <= 0 || r->render_height < 0 ||
        r->zoom < D_ENGINE_ZOOM_MIN || r->zoom > D_ENGINE_ZOOM_MAX ||
        (r->progress && (r->dump_dom || r->extract)) ||
        ((request_data || request_data_file) && (!r->request_method || !d_ieq(r->request_method, "post"))) ||
        (r->request_method && !d_ieq(r->request_method, "get") && !d_ieq(r->request_method, "post")))
        return 2;
    if (request_data_file) {
        args->request_data = read_request_body(request_data_file, &stdin_used, error);
        if (!args->request_data) return 2;
        r->request_data = args->request_data;
    } else if (request_data) {
        r->request_data = request_data;
    }
    return 0;
}

void d_engine_args_free(DEngineArgs *args)
{
    /* The body may hold a password; do not leave it in freed-but-readable
       heap longer than necessary. */
    if (args->request_data) {
        memset(args->request_data, 0, strlen(args->request_data));
        free(args->request_data);
    }
    for (size_t i = 0; i < args->owned_count; ++i) {
        memset(args->owned[i], 0, strlen(args->owned[i]));
        free(args->owned[i]);
    }
    free(args->owned);
    *args = (DEngineArgs){0};
}

void d_engine_args_usage(FILE *out, const char *program)
{
    fprintf(out, "usage: %s --url URL [--dump-dom|--extract|--render FILE|--template FILE|--markdown FILE|--dump-cells FILE|--viewer FILE|--fetch-media FILE] [--media-index N] [--save-media FILE] [--width N] [--height N|0] [--font-dir DIR] [--theme warm|night|paper] [--zoom PERCENT] [--native-forms] [--progress] [--https-optional] [--request-method GET|POST] [--request-data DATA|--request-data-file FILE|-] [--session-file FILE] [--script-cap-ms N] [--memory-limit-mb N] [--max-scripts N] [--max-fetches N] [--js-stack-kib N] [--request FILE|-] [--version]\n", program);
    fprintf(out, "  --request FILE        read the options from FILE, or stdin when FILE is \"-\": one per\n"
        "                        line, the name without its dashes and then its value, so a\n"
        "                        request never has to go on the command line.\n");
    fprintf(out, "  --dump-cells FILE     lay the reader page out in character cells, as the terminal\n"
        "                        shows it, and write it as plain text. --columns N (default\n"
        "                        80) is the terminal width; --rows N (default 0, the whole\n"
        "                        page) how much of it to write. --cells-from PAGE converts a\n"
        "                        reader page already on disk, like --markdown-from.\n");
    fprintf(out, "  --height 0            size the render to the content instead of cropping it.\n"
        "  --fetch-media FILE    fetch one image and write its bytes, with no scripts and no\n"
        "                        extraction. The format is taken from the bytes, not the\n"
        "                        served content type, and a non-image is refused.\n");
    fprintf(out, "  --zoom PERCENT        reader type size, 50 to 300, default 100. The column and the\n"
        "                        gutters scale with it; the surface width caps both.\n");
    fprintf(out, "  --progress            report \"progress PERCENT PHASE\" on stdout as the run moves\n"
        "                        through fetch, scripts, extract and render. The phases are\n"
        "                        what is measured; the percentage is their weighting. Not\n"
        "                        available with --dump-dom or --extract, whose output is\n"
        "                        stdout itself.\n");
    fprintf(out, "  --max-fetches N       transfers at once, default 8, 0 for no limit; more wait.\n"
        "  --js-stack-kib N      how deep JavaScript may recurse, in KiB of native stack;\n"
        "                        default: the stack limit less 1 MiB.\n");
    fprintf(out, "  --session-file FILE   keep cookies across runs, so a login survives the next navigation.\n");
    fprintf(out, "                        Created mode 600 and refused if others can read it.\n");
    fprintf(out, "  --request-data-file F read the POST body from a file, or from stdin when F is \"-\".\n");
    fprintf(out, "                        Prefer this for credentials: --request-data puts them in argv,\n");
    fprintf(out, "                        which is visible in the process table and in shell history.\n");
    fprintf(out, "Exit status: 0 content, 2 failure, 3 render failure, 4 a bot challenge,\n"
        "5 TLS refused because the system clock is not trusted.\n");
}
