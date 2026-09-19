#ifndef DISTILL_ENGINE_H
#define DISTILL_ENGINE_H

/* The engine as a library: one navigation in, one artifact out.

   `build/distill` is a thin command around d_engine_run, and the shells run
   that command once per navigation, which keeps crash isolation and gives
   all memory back when it exits. The same entry point also runs inside
   another process, on a thread of its own (engine_host.h), for a platform
   that cannot start processes and for testing the engine without spawning.

   One engine runs at a time in a process: the engine keeps a little state
   of its own (run statistics, a stylesheet cache) between the calls inside a
   run, and each run starts it afresh. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *url;

    /* What to produce. At most one of the artifacts, as the command line
       allows; see engine_args.h for the combinations. */
    bool dump_dom;                  /* the settled DOM, to `output` */
    bool extract;                   /* the extraction JSON, to `output` */
    const char *template_path;      /* the reader page */
    const char *render_path;        /* a PNG of the reader page */
    const char *viewer_path;        /* a PNG of the media viewer */
    const char *markdown_path;      /* the reader page as Markdown */
    const char *cells_path;         /* the reader page laid out in cells */
    const char *fetch_media_path;   /* one file, no scripts */
    const char *fetch_kind;         /* "image", "audio" or "any" */
    const char *save_media_path;    /* with viewer_path: the original bytes */
    int media_index;

    /* How the reader page looks. */
    const char *theme;
    int zoom;
    bool native_forms;
    int render_width, render_height;
    int cell_columns, cell_rows;
    const char *font_dir;

    /* The request itself. `request_data` may hold a password. */
    const char *request_method;
    const char *request_data;
    const char *session_file;
    bool https_optional;
    bool progress;                  /* report progress and location */

    /* Limits. Every one is explicit, and exceeding one is a refusal that
       names it. */
    uint64_t cap_ms;                /* settle budget */
    uint64_t memory_mb;             /* JavaScript heap */
    uint64_t max_scripts;
    uint64_t max_fetches;           /* transfers at once; 0 is no limit */
    size_t js_stack_bytes;          /* 0: from the stack this runs on */
} DEngineRequest;

/* Everything the engine says, other than the files it writes. Any callback
   may be NULL. Each is called on the thread that called d_engine_run. */
typedef struct {
    void *opaque;
    void (*progress)(void *opaque, int percent, const char *phase);
    void (*location)(void *opaque, const char *url);
    /* The --dump-dom or --extract result, whole. */
    void (*output)(void *opaque, const char *text);
    /* One line of diagnosis, without its newline. */
    void (*diagnostic)(void *opaque, const char *line);
} DEngineCallbacks;

/* The engine's exit statuses, which the command passes on unchanged. */
enum {
    D_ENGINE_OK = 0,
    D_ENGINE_FAILED = 2,
    D_ENGINE_RENDER_FAILED = 3,
    D_ENGINE_CHALLENGE = 4,        /* a recognised bot challenge */
    D_ENGINE_UNTRUSTED_TIME = 5,   /* TLS refused: the system clock is not trusted */
};

/* Reader zoom, as a percentage of the design size. */
#define D_ENGINE_ZOOM_MIN 50
#define D_ENGINE_ZOOM_MAX 300

/* The defaults the command line starts from. */
void d_engine_request_init(DEngineRequest *request);

int d_engine_run(const DEngineRequest *request, const DEngineCallbacks *callbacks);

/* Converting a reader page already on disk: no fetch, no scripts. */
int d_engine_markdown_from(const char *template_path, const char *url,
                           const char *markdown_path, const DEngineCallbacks *callbacks);
int d_engine_cells_from(const char *template_path, int columns, int rows,
                        const char *cells_path, const DEngineCallbacks *callbacks);

#endif
