#include "engine_host.h"

#include "dthread.h"

#include <stdio.h>
#include <stdlib.h>

/* The part of the thread's stack QuickJS may not count on: the frames above
   the point it measures from, and our own C between its checks. The same
   margin the engine keeps from a process's stack (engine.c). */
#define HOST_JS_STACK_MARGIN (1024u * 1024u)

typedef struct {
    DEngineRequest request;
    const DEngineCallbacks *callbacks;
    int status;
} HostRun;

static void run(void *opaque)
{
    HostRun *host = opaque;
    host->status = d_engine_run(&host->request, host->callbacks);
}

int d_engine_run_on_thread(const DEngineRequest *request, const DEngineCallbacks *callbacks,
                           size_t stack_bytes)
{
    HostRun host = {.request = *request, .callbacks = callbacks, .status = D_ENGINE_FAILED};
    if (!host.request.js_stack_bytes)
        host.request.js_stack_bytes = stack_bytes > 2 * HOST_JS_STACK_MARGIN
                                      ? stack_bytes - HOST_JS_STACK_MARGIN : stack_bytes / 2;
    char *error = NULL;
    DThread *thread = d_thread_start(run, &host, stack_bytes, &error);
    if (!thread) {
        if (callbacks && callbacks->diagnostic)
            callbacks->diagnostic(callbacks->opaque, error ? error : "unable to start a thread");
        free(error);
        return D_ENGINE_FAILED;
    }
    d_thread_join(thread);
    return host.status;
}
