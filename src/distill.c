/* The distill command: the engine, once, as a process.

   Options in, one artifact out, and the exit status the engine returns.
   What the engine reports goes where the shells have always read it:
   "progress PERCENT PHASE" and "location URL" lines and --dump-dom or
   --extract output on stdout, diagnosis on stderr. */

#include "cutil.h"
#include "distill_version.h"
#include "engine.h"
#include "engine_args.h"
#include "measure.h"

static void on_progress(void *opaque, int percent, const char *phase)
{
    (void)opaque;
    printf("progress %d %s\n", percent, phase);
    fflush(stdout);
}

static void on_location(void *opaque, const char *url)
{
    (void)opaque;
    printf("location %s\n", url);
    fflush(stdout);
}

static void on_output(void *opaque, const char *text)
{
    (void)opaque;
    puts(text);
}

static void on_diagnostic(void *opaque, const char *line)
{
    (void)opaque;
    fprintf(stderr, "%s\n", line);
}

int main(int argc, char **argv)
{
    d_measure_begin(argv);
    DEngineArgs args;
    char *error = NULL;
    int status = d_engine_args_parse(&args, argc, (const char *const *)argv, &error);
    if (status != 0) {
        if (error) fprintf(stderr, "%s\n", error);
        else d_engine_args_usage(stderr, argv[0]);
        free(error);
        d_engine_args_free(&args);
        return status;
    }
    if (args.help) { d_engine_args_usage(stderr, argv[0]); d_engine_args_free(&args); return 0; }
    if (args.version) { puts("distill " DISTILL_VERSION); d_engine_args_free(&args); return 0; }
    const DEngineCallbacks callbacks = {
        .progress = on_progress,
        .location = on_location,
        .output = on_output,
        .diagnostic = on_diagnostic,
    };
    if (args.markdown_from)
        status = d_engine_markdown_from(args.markdown_from, args.request.url,
                                        args.request.markdown_path, &callbacks);
    else if (args.cells_from)
        status = d_engine_cells_from(args.cells_from, args.request.cell_columns,
                                     args.request.cell_rows, args.request.cells_path, &callbacks);
    else
        status = d_engine_run(&args.request, &callbacks);
    d_engine_args_free(&args);
    return status;
}
