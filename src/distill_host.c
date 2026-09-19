/* distill-host: many navigations, in one process, each on a thread of its own.

   The in-process mode of the engine (engine_host.h), driven the way the
   corpus drives the command. Each line on stdin names a request file and two
   files for what the run says:

       REQUEST<TAB>OUTPUT<TAB>ERRORS

   OUTPUT gets what the command would print on stdout -- progress and
   location lines, the --dump-dom or --extract result -- and ERRORS what it
   would print on stderr. The reply on stdout is one line, "status N", with
   the status the command would have exited with.

   Running a whole corpus through one process is the point: a run that
   leaves state behind for the next shows up as the next run's failure. */

#include "cutil.h"
#include "engine_args.h"
#include "engine_host.h"

typedef struct {
    FILE *output;
    FILE *errors;
} Sinks;

static void on_progress(void *opaque, int percent, const char *phase)
{
    fprintf(((Sinks *)opaque)->output, "progress %d %s\n", percent, phase);
}

static void on_location(void *opaque, const char *url)
{
    fprintf(((Sinks *)opaque)->output, "location %s\n", url);
}

static void on_output(void *opaque, const char *text)
{
    fprintf(((Sinks *)opaque)->output, "%s\n", text);
}

static void on_diagnostic(void *opaque, const char *line)
{
    fprintf(((Sinks *)opaque)->errors, "%s\n", line);
}

static int run_one(const char *request_path, FILE *output, FILE *errors)
{
    const char *argv[] = {"distill", "--request", request_path, NULL};
    DEngineArgs args;
    char *error = NULL;
    int status = d_engine_args_parse(&args, 3, argv, &error);
    if (status != 0) {
        if (error) fprintf(errors, "%s\n", error);
        else d_engine_args_usage(errors, "distill");
        free(error);
        d_engine_args_free(&args);
        return status;
    }
    Sinks sinks = {output, errors};
    const DEngineCallbacks callbacks = {
        .opaque = &sinks,
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
        status = d_engine_run_on_thread(&args.request, &callbacks, D_ENGINE_THREAD_STACK);
    d_engine_args_free(&args);
    return status;
}

int main(void)
{
    char line[3 * 1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!*line) continue;
        char *output_path = strchr(line, '\t');
        char *errors_path = output_path ? strchr(output_path + 1, '\t') : NULL;
        if (!errors_path) {
            printf("status 2\n");
            fflush(stdout);
            continue;
        }
        *output_path++ = '\0';
        *errors_path++ = '\0';
        FILE *output = fopen(output_path, "w");
        FILE *errors = fopen(errors_path, "w");
        int status = output && errors ? run_one(line, output, errors) : 2;
        if (output) fclose(output);
        if (errors) fclose(errors);
        printf("status %d\n", status);
        fflush(stdout);
    }
    return 0;
}
