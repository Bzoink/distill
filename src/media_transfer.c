#include "media_transfer.h"

#include "cutil.h"
#include "dfs.h"
#include "dproc.h"
#include "engine_request.h"

struct DMediaTransfer {
    DProc *engine;
    char *url;
    char *request;       /* the engine's request file */
    char *partial;       /* where the engine writes */
    char *destination;   /* where it belongs once complete; NULL keeps partial */
    DBuffer output;      /* what the engine said, for the failure message */
    DTransferState state;
    char *error;
};

DMediaTransfer *d_media_transfer_start(const char *engine, const char *url,
                                       const char *destination, const char *kind,
                                       char **error)
{
    if (error) *error = NULL;
    /* A temporary file that may hold what the reader is listening to is
       private to them. */
    char *partial = destination ? d_cat2(destination, ".part")
                                : d_fs_private_file(NULL, "distill-media");
    if (!partial) {
        if (error) *error = d_strdup("unable to create a temporary file");
        return NULL;
    }
    /* Signed media URLs run long; the request goes in a file, not argv. */
    DRequestFile request_file;
    d_request_file_init(&request_file);
    d_request_file_add(&request_file, "url", url);
    d_request_file_add(&request_file, "fetch-media", partial);
    d_request_file_add(&request_file, "fetch-kind", kind ? kind : "any");
    char *request = d_request_file_write(&request_file);
    char *why = NULL;
    const char *argv[] = {engine, "--request", request, NULL};
    DProc *child = request ? d_proc_spawn(argv, D_PROC_PIPE, D_PROC_WITH_OUTPUT, &why) : NULL;
    if (!child) {
        if (error) *error = d_cat2("unable to start the engine: ",
                                   why ? why : request ? "unknown error" : "no request file");
        free(why);
        if (request) { d_fs_remove(request); free(request); }
        if (!destination) d_fs_remove(partial);
        free(partial);
        return NULL;
    }
    DMediaTransfer *transfer = d_xcalloc(1, sizeof(*transfer));
    transfer->engine = child;
    transfer->request = request;
    transfer->url = d_strdup(url);
    transfer->partial = partial;
    transfer->destination = destination ? d_strdup(destination) : NULL;
    transfer->state = D_TRANSFER_RUNNING;
    dbuf_init(&transfer->output);
    return transfer;
}

int d_media_transfer_fd(const DMediaTransfer *transfer)
{
    return transfer ? d_proc_output(transfer->engine) : -1;
}

/* The engine's last line of complaint, which is the one worth showing. */
static char *last_line(const DBuffer *output)
{
    if (!output->length) return NULL;
    size_t end = output->length;
    while (end && (output->data[end - 1] == '\n' || output->data[end - 1] == '\r')) --end;
    size_t start = end;
    while (start && output->data[start - 1] != '\n') --start;
    return end > start ? d_strndup(output->data + start, end - start) : NULL;
}

static void fail(DMediaTransfer *transfer, char *message)
{
    transfer->state = D_TRANSFER_FAILED;
    free(transfer->error);
    transfer->error = message;
    d_fs_remove(transfer->partial);
}

DTransferState d_media_transfer_poll(DMediaTransfer *transfer)
{
    if (!transfer || transfer->state != D_TRANSFER_RUNNING) return transfer ? transfer->state : D_TRANSFER_FAILED;
    char buffer[4096];
    for (;;) {
        long got = d_handle_read(d_proc_output(transfer->engine), buffer, sizeof(buffer));
        if (got > 0) {
            /* A bounded record of what it said: the tail is what matters. */
            if (transfer->output.length < 64 * 1024) dbuf_append(&transfer->output, buffer, (size_t)got);
            continue;
        }
        if (got == D_READ_EMPTY) return D_TRANSFER_RUNNING;
        break;                                    /* end of output: the engine has exited */
    }
    d_proc_close_output(transfer->engine);
    DProcStatus status = d_proc_wait(transfer->engine);
    d_proc_free(transfer->engine);
    transfer->engine = NULL;
    if (!status.exited || status.code != 0) {
        char *said = last_line(&transfer->output);
        fail(transfer, said ? said : d_strdup(!status.exited ? "the engine was stopped"
                                                             : "the engine failed"));
        return transfer->state;
    }
    if (transfer->destination) {
        /* Renamed only now. A partial file that looks like a finished one is
           worse than no file at all. */
        if (rename(transfer->partial, transfer->destination) != 0) {
            fail(transfer, d_cat3("could not be saved as ", transfer->destination, ""));
            return transfer->state;
        }
    }
    transfer->state = D_TRANSFER_DONE;
    return transfer->state;
}

const char *d_media_transfer_url(const DMediaTransfer *transfer)
{
    return transfer ? transfer->url : "";
}

const char *d_media_transfer_path(const DMediaTransfer *transfer)
{
    if (!transfer) return "";
    return transfer->destination ? transfer->destination : transfer->partial;
}

const char *d_media_transfer_error(const DMediaTransfer *transfer)
{
    return transfer && transfer->error ? transfer->error : "";
}

void d_media_transfer_free(DMediaTransfer *transfer)
{
    if (!transfer) return;
    if (transfer->engine) {
        d_proc_terminate(transfer->engine);
        d_proc_free(transfer->engine);
    }
    if (transfer->request) { d_fs_remove(transfer->request); free(transfer->request); }
    free(transfer->url);
    free(transfer->partial);
    free(transfer->destination);
    free(transfer->error);
    dbuf_free(&transfer->output);
    free(transfer);
}

void d_media_transfer_cancel(DMediaTransfer *transfer)
{
    if (!transfer) return;
    if (transfer->engine) {
        d_proc_terminate(transfer->engine);
        d_proc_free(transfer->engine);
        transfer->engine = NULL;
    }
    /* A cancelled transfer leaves nothing behind. */
    if (transfer->state == D_TRANSFER_RUNNING) d_fs_remove(transfer->partial);
    d_media_transfer_free(transfer);
}
