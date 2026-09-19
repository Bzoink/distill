/* dthread on Linux: pthreads with pthread_attr_setstacksize. */

#define _POSIX_C_SOURCE 200809L

#include "dthread.h"

#include "cutil.h"

#include <limits.h>
#include <pthread.h>

struct DThread {
    pthread_t handle;
    void (*run)(void *opaque);
    void *opaque;
};

static void *trampoline(void *argument)
{
    DThread *thread = argument;
    thread->run(thread->opaque);
    return NULL;
}

DThread *d_thread_start(void (*run)(void *opaque), void *opaque, size_t stack_bytes,
                        char **error)
{
    if (error) *error = NULL;
    DThread *thread = d_xcalloc(1, sizeof(*thread));
    thread->run = run;
    thread->opaque = opaque;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stack_bytes < PTHREAD_STACK_MIN) stack_bytes = PTHREAD_STACK_MIN;
    int code = pthread_attr_setstacksize(&attributes, stack_bytes);
    if (code == 0) code = pthread_create(&thread->handle, &attributes, trampoline, thread);
    pthread_attr_destroy(&attributes);
    if (code != 0) {
        if (error) *error = d_cat2("unable to start a thread: ", strerror(code));
        free(thread);
        return NULL;
    }
    return thread;
}

void d_thread_join(DThread *thread)
{
    if (!thread) return;
    pthread_join(thread->handle, NULL);
    free(thread);
}
