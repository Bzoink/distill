#ifndef DISTILL_MEDIA_TRANSFER_H
#define DISTILL_MEDIA_TRANSFER_H

/* Fetching a media file for a front end, without GLib.

   The shells open no socket of their own: the engine owns the network and
   URL validation, so a transfer is the engine run once with --fetch-media.
   What a front end needs around that is the same everywhere -- start it
   without blocking, hear when it ends, cancel it, and never leave a partial
   file where a finished one belongs -- so it lives here, once.

   Completion is a file descriptor. The engine's output goes down a pipe
   whose far end closes when the engine exits, so a front end waits on it in
   whatever loop it already has: poll() in the terminal, a GLib source on the
   desktop. */

#include <stdbool.h>

typedef struct DMediaTransfer DMediaTransfer;

typedef enum {
    D_TRANSFER_RUNNING,
    D_TRANSFER_DONE,
    D_TRANSFER_FAILED,
} DTransferState;

/* `kind` is what the engine checks the bytes are: "audio" for the decoder,
   "image" for the viewer, "any" for a download, which is a copy of the source
   and not the engine's business to judge. With `destination` NULL the file
   goes to a private temporary path (0600) the caller removes when done with
   it; otherwise it is written beside the destination and renamed into place
   only once complete. NULL with `*error` set when the engine cannot start. */
DMediaTransfer *d_media_transfer_start(const char *engine, const char *url,
                                       const char *destination, const char *kind,
                                       char **error);

/* Readable when the engine has output or has exited. */
int d_media_transfer_fd(const DMediaTransfer *transfer);

/* Non-blocking: drains what the engine said and, once it has exited, reaps
   it and settles the file. RUNNING until then. */
DTransferState d_media_transfer_poll(DMediaTransfer *transfer);

const char *d_media_transfer_url(const DMediaTransfer *transfer);
/* Where the finished file is: the destination, or the temporary path. */
const char *d_media_transfer_path(const DMediaTransfer *transfer);
/* Why it failed, in the engine's words where it gave any. */
const char *d_media_transfer_error(const DMediaTransfer *transfer);

/* Stop a running transfer and leave nothing behind. Frees it. */
void d_media_transfer_cancel(DMediaTransfer *transfer);
/* Free a finished transfer. The file it produced is the caller's. */
void d_media_transfer_free(DMediaTransfer *transfer);

#endif
