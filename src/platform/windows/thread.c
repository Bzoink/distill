#include "dthread.h"
#include "win.h"

#include <errno.h>
#include <limits.h>
#include <process.h>

struct DThread {
    HANDLE handle;
    void (*run)(void *);
    void *opaque;
};

static unsigned __stdcall trampoline(void *opaque)
{
    DThread *thread = opaque;
    thread->run(thread->opaque);
    return 0;
}

DThread *d_thread_start(void (*run)(void *), void *opaque, size_t stack_bytes, char **error)
{
    if (error) *error = NULL;
    if (stack_bytes > UINT_MAX) {
        if (error) *error = d_strdup("thread stack is too large");
        return NULL;
    }
    DThread *thread = d_xcalloc(1, sizeof(*thread));
    thread->run = run;
    thread->opaque = opaque;
    thread->handle = (HANDLE)_beginthreadex(NULL, (unsigned)stack_bytes, trampoline,
                                           thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (thread->handle) return thread;
    if (error) *error = d_cat2("unable to start a thread: ", strerror(errno));
    free(thread);
    return NULL;
}

void d_thread_join(DThread *thread)
{
    if (!thread) return;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    free(thread);
}
