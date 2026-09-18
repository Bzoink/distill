#define _GNU_SOURCE

#include "media_transfer.h"

#include "cutil.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct DMediaTransfer {
    pid_t pid;
    int fd;
    char *url;
    char *partial;       /* where the engine writes */
    char *destination;   /* where it belongs once complete; NULL keeps partial */
    DBuffer output;      /* what the engine said, for the failure message */
    DTransferState state;
    char *error;
};

static char *temporary_file(void)
{
    const char *directory = getenv("TMPDIR");
    char *path = d_cat2(directory && *directory ? directory : "/tmp", "/distill-media-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    /* mkstemp already makes it 0600; a file that may hold what the reader
       is listening to says so explicitly. */
    fchmod(fd, S_IRUSR | S_IWUSR);
    close(fd);
    return path;
}

DMediaTransfer *d_media_transfer_start(const char *engine, const char *url,
                                       const char *destination, const char *kind,
                                       char **error)
{
    if (error) *error = NULL;
    char *partial = destination ? d_cat2(destination, ".part") : temporary_file();
    if (!partial) {
        if (error) *error = d_strdup("unable to create a temporary file");
        return NULL;
    }
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        if (error) *error = d_strdup(strerror(errno));
        if (!destination) unlink(partial);
        free(partial);
        return NULL;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    char *argv[] = {(char *)engine, "--url", (char *)url, "--fetch-media", partial,
                    "--fetch-kind", (char *)(kind ? kind : "any"), NULL};
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, engine, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    if (spawned != 0) {
        if (error) *error = d_cat2("unable to start the engine: ", strerror(spawned));
        close(pipe_fds[0]);
        if (!destination) unlink(partial);
        free(partial);
        return NULL;
    }
    fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL) | O_NONBLOCK);
    DMediaTransfer *transfer = d_xcalloc(1, sizeof(*transfer));
    transfer->pid = pid;
    transfer->fd = pipe_fds[0];
    transfer->url = d_strdup(url);
    transfer->partial = partial;
    transfer->destination = destination ? d_strdup(destination) : NULL;
    transfer->state = D_TRANSFER_RUNNING;
    dbuf_init(&transfer->output);
    return transfer;
}

int d_media_transfer_fd(const DMediaTransfer *transfer)
{
    return transfer ? transfer->fd : -1;
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
    unlink(transfer->partial);
}

DTransferState d_media_transfer_poll(DMediaTransfer *transfer)
{
    if (!transfer || transfer->state != D_TRANSFER_RUNNING) return transfer ? transfer->state : D_TRANSFER_FAILED;
    char buffer[4096];
    for (;;) {
        ssize_t got = read(transfer->fd, buffer, sizeof(buffer));
        if (got > 0) {
            /* A bounded record of what it said: the tail is what matters. */
            if (transfer->output.length < 64 * 1024) dbuf_append(&transfer->output, buffer, (size_t)got);
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return D_TRANSFER_RUNNING;   /* EAGAIN: still going */
        break;                                    /* EOF: the engine has exited */
    }
    close(transfer->fd);
    transfer->fd = -1;
    int status = 0;
    while (waitpid(transfer->pid, &status, 0) < 0 && errno == EINTR) {}
    transfer->pid = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char *said = last_line(&transfer->output);
        fail(transfer, said ? said : d_strdup(WIFSIGNALED(status) ? "the engine was stopped"
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
    if (transfer->fd >= 0) close(transfer->fd);
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
    if (transfer->pid > 0) {
        kill(transfer->pid, SIGTERM);
        while (waitpid(transfer->pid, NULL, 0) < 0 && errno == EINTR) {}
        transfer->pid = 0;
    }
    /* A cancelled transfer leaves nothing behind. */
    if (transfer->state == D_TRANSFER_RUNNING) unlink(transfer->partial);
    d_media_transfer_free(transfer);
}
