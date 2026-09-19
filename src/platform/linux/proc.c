/* dproc on Linux: posix_spawn, pipes, poll, waitpid and sigaction. */

#define _GNU_SOURCE

#include "dproc.h"

#include "cutil.h"
#include "dfs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct DProc {
    pid_t pid;
    int output;
    int errors;
    bool ended;
    DProcStatus status;
};

static void set_error(char **error, const char *what, int code)
{
    if (!error) return;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", what, strerror(code));
    *error = d_strdup(buffer);
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

DProc *d_proc_spawn(const char *const *argv, DProcStream output, DProcStream errors,
                    char **error)
{
    if (error) *error = NULL;
    if (output == D_PROC_WITH_OUTPUT) output = D_PROC_DISCARD;
    if (errors == D_PROC_WITH_OUTPUT && output != D_PROC_PIPE) errors = D_PROC_DISCARD;
    int out[2] = {-1, -1}, err[2] = {-1, -1};
    if ((output == D_PROC_PIPE && pipe2(out, O_CLOEXEC) != 0) ||
        (errors == D_PROC_PIPE && pipe2(err, O_CLOEXEC) != 0)) {
        int code = errno;
        for (int i = 0; i < 2; ++i) {
            if (out[i] >= 0) close(out[i]);
            if (err[i] >= 0) close(err[i]);
        }
        set_error(error, "no pipe to the child", code);
        return NULL;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (output == D_PROC_PIPE)
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (errors == D_PROC_PIPE)
        posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    else if (errors == D_PROC_WITH_OUTPUT)
        posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (out[1] >= 0) close(out[1]);
    if (err[1] >= 0) close(err[1]);
    if (spawned != 0) {
        if (out[0] >= 0) close(out[0]);
        if (err[0] >= 0) close(err[0]);
        set_error(error, argv[0], spawned);
        return NULL;
    }
    DProc *proc = d_xcalloc(1, sizeof(*proc));
    proc->pid = pid;
    proc->output = out[0];
    proc->errors = err[0];
    if (proc->output >= 0) set_nonblocking(proc->output);
    if (proc->errors >= 0) set_nonblocking(proc->errors);
    return proc;
}

int d_proc_output(const DProc *proc)
{
    return proc ? proc->output : -1;
}

int d_proc_errors(const DProc *proc)
{
    return proc ? proc->errors : -1;
}

void d_proc_close_output(DProc *proc)
{
    if (proc && proc->output >= 0) { close(proc->output); proc->output = -1; }
}

void d_proc_close_errors(DProc *proc)
{
    if (proc && proc->errors >= 0) { close(proc->errors); proc->errors = -1; }
}

static DProcStatus status_from(int raw)
{
    return WIFEXITED(raw) ? (DProcStatus){.exited = true, .code = WEXITSTATUS(raw)}
                          : (DProcStatus){.exited = false, .code = -1};
}

bool d_proc_poll_exit(DProc *proc, DProcStatus *status)
{
    if (!proc) return false;
    if (!proc->ended) {
        int raw = 0;
        pid_t reaped;
        while ((reaped = waitpid(proc->pid, &raw, WNOHANG)) < 0 && errno == EINTR) {}
        if (reaped == 0) return false;
        proc->ended = true;
        proc->status = reaped == proc->pid ? status_from(raw)
                                           : (DProcStatus){.exited = false, .code = -1};
    }
    if (status) *status = proc->status;
    return true;
}

DProcStatus d_proc_wait(DProc *proc)
{
    if (!proc->ended) {
        int raw = 0;
        pid_t reaped;
        while ((reaped = waitpid(proc->pid, &raw, 0)) < 0 && errno == EINTR) {}
        proc->ended = true;
        proc->status = reaped == proc->pid ? status_from(raw)
                                           : (DProcStatus){.exited = false, .code = -1};
    }
    return proc->status;
}

void d_proc_terminate(DProc *proc)
{
    if (!proc || proc->ended) return;
    kill(proc->pid, SIGTERM);
    d_proc_wait(proc);
}

void d_proc_free(DProc *proc)
{
    if (!proc) return;
    d_proc_close_output(proc);
    d_proc_close_errors(proc);
    free(proc);
}

DProc *d_proc_open_with_system(const char *uri)
{
    /* xdg-open needs a desktop session to open anything in. */
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (!(display && *display) && !(wayland && *wayland)) return NULL;
    const char *argv[] = {"xdg-open", uri, NULL};
    return d_proc_spawn(argv, D_PROC_DISCARD, D_PROC_DISCARD, NULL);
}

long d_handle_read(int handle, void *buffer, size_t size)
{
    for (;;) {
        ssize_t got = read(handle, buffer, size);
        if (got >= 0) return (long)got;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return D_READ_EMPTY;
        return D_READ_FAILED;
    }
}

int d_poll(DPollItem *items, size_t count, int timeout_ms)
{
    struct pollfd fds[16];
    if (count > sizeof(fds) / sizeof(fds[0])) return -1;
    for (size_t i = 0; i < count; ++i) {
        fds[i] = (struct pollfd){items[i].handle, POLLIN, 0};
        items[i].ready = false;
    }
    int ready = poll(fds, (nfds_t)count, timeout_ms);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready == 0) return 0;
    for (size_t i = 0; i < count; ++i)
        items[i].ready = fds[i].revents != 0;
    return ready;
}

static void (*fatal_restore)(void);

static void on_fatal_signal(int signal_number)
{
    if (fatal_restore) fatal_restore();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

size_t d_proc_on_fatal_signal(void (*restore)(void))
{
    fatal_restore = restore;
    static const int signals[] = {SIGTERM, SIGHUP, SIGINT, SIGQUIT,
                                  SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
    size_t installed = 0;
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        struct sigaction action = {0};
        action.sa_handler = on_fatal_signal;
        sigemptyset(&action.sa_mask);
        if (sigaction(signals[i], &action, NULL) == 0) ++installed;
    }
    signal(SIGPIPE, SIG_IGN);
    return installed;
}

size_t d_proc_stack_limit(void)
{
    struct rlimit stack;
    if (getrlimit(RLIMIT_STACK, &stack) != 0 || stack.rlim_cur == RLIM_INFINITY) return 0;
    return stack.rlim_cur > SIZE_MAX ? SIZE_MAX : (size_t)stack.rlim_cur;
}

char *d_proc_executable_directory(void)
{
    char path[D_FS_PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return NULL;
    path[length] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    *slash = '\0';
    return d_strdup(path);
}

bool d_proc_set_environment(const char *name, const char *value)
{
    return setenv(name, value, 1) == 0;
}

long d_proc_id(void)
{
    return (long)getpid();
}

long d_proc_peak_rss_kib(void)
{
    /* VmHWM is this address space's high-water mark. getrusage() is only the
       fallback: its ru_maxrss survives execve, so a process started from a
       large parent reports the parent's peak -- every engine spawned from
       Python read exactly Python's 30 MiB. */
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        long kib = -1;
        while (fgets(line, sizeof(line), status))
            if (sscanf(line, "VmHWM: %ld kB", &kib) == 1) break;
        fclose(status);
        if (kib >= 0) return kib;
    }
    struct rusage usage = {0};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    return (long)usage.ru_maxrss;
}
