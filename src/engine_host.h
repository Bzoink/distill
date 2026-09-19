#ifndef DISTILL_ENGINE_HOST_H
#define DISTILL_ENGINE_HOST_H

/* Running the engine inside another process.

   The shells run the engine as a process of its own by default: a crash or a
   leak in a page's scripts then costs one navigation, and all its memory
   comes back when it exits. This is the other way, for a platform that
   cannot start processes and for testing the engine without spawning one.

   The run gets a thread of its own, never the caller's stack: on Muroxi a
   shell may rely on 64 KiB of stack only, and a page's scripts can use
   hundreds. QuickJS's recursion limit is sized from that thread's stack, so
   a deep page is refused with a RangeError inside it. What contains the run
   otherwise is what contains a process: the JS heap limit and the settle
   budget. One run at a time (engine.h). */

#include "engine.h"

/* The largest stack Muroxi gives a thread (MX_PTHREAD_STACK_MAX). */
#define D_ENGINE_THREAD_STACK (8u * 1024u * 1024u - 4096u)

/* Run `request` on a new thread with `stack_bytes` of stack and wait for it.
   The callbacks are called on that thread. */
int d_engine_run_on_thread(const DEngineRequest *request, const DEngineCallbacks *callbacks,
                           size_t stack_bytes);

#endif
