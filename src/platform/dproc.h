#ifndef DISTILL_PLATFORM_DPROC_H
#define DISTILL_PLATFORM_DPROC_H

/* Processes, for the core: running the engine and helpers as children,
   hearing from them, waiting on several things at once, and the facts about
   this process a budget needs.

   A handle is a small non-negative integer naming something that can be
   read and waited on -- a child's output, a terminal. -1 is no handle. */

#include <stddef.h>
#include <stdint.h>

typedef struct DProc DProc;

/* Where a child's output goes. Its input is always empty. */
typedef enum {
    D_PROC_DISCARD,      /* nowhere */
    D_PROC_PIPE,         /* to a handle this process reads */
    D_PROC_WITH_OUTPUT,  /* standard error only: into standard output's pipe */
} DProcStream;

/* Start argv[0] with arguments argv (NULL-terminated). A program named
   without a '/' is looked for on the search path. The child inherits the
   environment. NULL with `*error` set (caller frees) when it cannot start. */
DProc *d_proc_spawn(const char *const *argv, DProcStream output, DProcStream errors,
                    char **error);

/* The read handles of a child's piped streams, or -1. Reads never block. */
int d_proc_output(const DProc *proc);
int d_proc_errors(const DProc *proc);

/* Stop reading a stream. The handle is closed and the child's accessor
   returns -1 from now on. */
void d_proc_close_output(DProc *proc);
void d_proc_close_errors(DProc *proc);

typedef struct {
    bool exited;    /* ended by returning or exit(); false when it was killed */
    int code;       /* the exit status, when exited */
} DProcStatus;

/* True once the child has ended, filling `status`; false while it runs.
   Never blocks. */
bool d_proc_poll_exit(DProc *proc, DProcStatus *status);

/* Wait for the child to end. */
DProcStatus d_proc_wait(DProc *proc);

/* Ask the child to stop and wait until it has. */
void d_proc_terminate(DProc *proc);

/* Free an ended child, closing any handle still open. */
void d_proc_free(DProc *proc);

/* Read what is there without waiting: the byte count, 0 at the end of the
   stream, D_READ_EMPTY when nothing is there yet, D_READ_FAILED on error. */
#define D_READ_EMPTY (-1L)
#define D_READ_FAILED (-2L)
long d_handle_read(int handle, void *buffer, size_t size);

/* Hand a validated URI to whatever this system opens such things with -- a
   browser, a mail client. The caller reaps the returned child. NULL when
   there is no such handler here, as on a console or over SSH, and the
   caller shows the address instead. */
DProc *d_proc_open_with_system(const char *uri);

/* Wait until one of the handles is readable (or has ended), or `timeout_ms`
   passes; -1 waits without limit. Sets `ready` on each; returns how many are
   ready, 0 on a timeout or an interruption (a signal), -1 on an error. */
typedef struct {
    int handle;
    bool ready;
} DPollItem;
int d_poll(DPollItem *items, size_t count, int timeout_ms);

/* Run `restore` when this process is told to stop or crashes, then die of
   the same signal as before. `restore` runs in a signal handler: it may only
   do what is safe there (d_fs_remove, d_term_emergency_restore). Installs
   what the platform has -- Muroxi has no SIGSEGV -- and returns how many were
   installed; a platform with none returns 0 and the program carries on.
   Writing to a pipe whose reader has gone then fails with an error rather
   than stopping the process. */
size_t d_proc_on_fatal_signal(void (*restore)(void));

/* This process's main-thread stack limit in bytes, or 0 when unlimited or
   unknown. */
size_t d_proc_stack_limit(void);

/* The directory holding this program's executable, or NULL. */
char *d_proc_executable_directory(void);

/* Set an environment variable for this process and the children it starts
   from now on. */
bool d_proc_set_environment(const char *name, const char *value);

/* This process's identifier, for telling runs apart in a shared log. */
long d_proc_id(void);

/* The most resident memory this process has used, in KiB; -1 if unknown.
   It must be this program's own peak: on Linux a process's ru_maxrss
   survives execve, so the parent's size would be reported instead. */
long d_proc_peak_rss_kib(void);

#endif
