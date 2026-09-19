#ifndef DISTILL_PLATFORM_DTHREAD_H
#define DISTILL_PLATFORM_DTHREAD_H

/* Threads with a stack of a stated size. C11's threads cannot say how big a
   thread's stack is, and the engine must know: QuickJS's recursion limit is
   only a safeguard if it sits inside the stack it runs on. */

#include <stddef.h>

typedef struct DThread DThread;

/* Run `run(opaque)` on a new thread with at least `stack_bytes` of stack.
   NULL with `*error` set (caller frees) when it cannot start. */
DThread *d_thread_start(void (*run)(void *opaque), void *opaque, size_t stack_bytes,
                        char **error);

/* Wait for the thread to finish, and free it. */
void d_thread_join(DThread *thread);

#endif
