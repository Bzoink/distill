/* A transfer's lifecycle against a stand-in engine: a script that behaves
   the way the engine does for --fetch-media -- writes the file and exits 0,
   or complains and exits 2 -- or hangs, so cancellation can be seen. */

#define _POSIX_C_SOURCE 200809L

#include "media_transfer.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[] = "/tmp/distill-transfer-XXXXXX";
static char engine[256];

static void write_engine(void)
{
    snprintf(engine, sizeof(engine), "%s/engine.sh", directory);
    FILE *file = fopen(engine, "w");
    assert(file);
    /* argv: --request FILE, whose lines are "url URL", "fetch-media PATH"
       and "fetch-kind KIND" (engine_args.h). */
    fputs("#!/bin/sh\n"
          "[ \"$1\" = --request ] || exit 2\n"
          "while read -r name value; do\n"
          "  case \"$name\" in\n"
          "    url) url=$value;; fetch-media) out=$value;; fetch-kind) kind=$value;;\n"
          "  esac\n"
          "done < \"$2\"\n"
          "case \"$url\" in\n"
          "  *ok*) printf 'bytes' > \"$out\"; echo \"fetched $kind\" >&2; exit 0;;\n"
          "  *fail*) printf 'partial' > \"$out\"; echo 'not audio this reader can decode: x' >&2; exit 2;;\n"
          "  *hang*) printf 'partial' > \"$out\"; exec sleep 30;;\n"
          "esac\n", file);
    fclose(file);
    chmod(engine, 0700);
}

static DTransferState run_to_end(DMediaTransfer *transfer)
{
    DTransferState state;
    while ((state = d_media_transfer_poll(transfer)) == D_TRANSFER_RUNNING) {
        struct pollfd fd = {d_media_transfer_fd(transfer), POLLIN, 0};
        assert(poll(&fd, 1, 5000) == 1);
    }
    return state;
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

int main(void)
{
    assert(mkdtemp(directory));
    write_engine();
    char destination[256], partial[300];
    snprintf(destination, sizeof(destination), "%s/song.mp3", directory);
    snprintf(partial, sizeof(partial), "%s.part", destination);
    char *error = NULL;

    /* A download lands at its destination, and only once finished. */
    DMediaTransfer *transfer = d_media_transfer_start(engine, "https://x/ok", destination, "any", &error);
    assert(transfer && !error);
    assert(run_to_end(transfer) == D_TRANSFER_DONE);
    assert(!strcmp(d_media_transfer_path(transfer), destination));
    assert(exists(destination) && !exists(partial));
    d_media_transfer_free(transfer);
    unlink(destination);

    /* Playback goes to a private temporary file the caller owns. */
    transfer = d_media_transfer_start(engine, "https://x/ok", NULL, "audio", &error);
    assert(transfer);
    assert(run_to_end(transfer) == D_TRANSFER_DONE);
    const char *temporary = d_media_transfer_path(transfer);
    struct stat info;
    assert(stat(temporary, &info) == 0 && (info.st_mode & 0777) == 0600);
    char *kept = strdup(temporary);
    d_media_transfer_free(transfer);
    assert(exists(kept));
    unlink(kept);
    free(kept);

    /* A failure says why, in the engine's words, and leaves nothing. */
    transfer = d_media_transfer_start(engine, "https://x/fail", destination, "audio", &error);
    assert(transfer);
    assert(run_to_end(transfer) == D_TRANSFER_FAILED);
    assert(strstr(d_media_transfer_error(transfer), "not audio this reader can decode"));
    assert(!exists(destination) && !exists(partial));
    d_media_transfer_free(transfer);

    /* Cancelling stops the engine and leaves nothing. */
    transfer = d_media_transfer_start(engine, "https://x/hang", destination, "any", &error);
    assert(transfer);
    for (int i = 0; i < 50 && !exists(partial); ++i) nanosleep(&(struct timespec){0, 20000000}, NULL);
    assert(exists(partial));
    d_media_transfer_cancel(transfer);
    assert(!exists(destination) && !exists(partial));

    /* An engine that is not there is an error, not a hang. */
    transfer = d_media_transfer_start("/nonexistent/distill", "https://x/ok", destination, "any", &error);
    assert(!transfer && error);
    free(error);

    unlink(engine);
    rmdir(directory);
    puts("media transfer: ok");
    return 0;
}
