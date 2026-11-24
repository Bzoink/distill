#include "measure.h"

#include "dfs.h"
#include "dproc.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stack probe. Every word of the region below the caller's frame is
   written with a pattern before the program does any work; at the end the
   lowest word that no longer holds it is the deepest the stack went. Writing
   below the stack pointer is outside what C promises, and that is why this is
   measurement only: the region is written once, in a frame that makes no
   calls while it writes, and never read until the program is finishing. */

static const uint64_t probe_pattern = 0x5a17c0de5a17c0deu;
static uintptr_t probe_top;     /* the initial stack pointer or the reservation top */
static uintptr_t probe_high;    /* the first word painted, just below this frame */
static uintptr_t probe_low;     /* the last word painted */

/* Room between the painting frame and the paint, for this frame's own locals
   and the red zone below the stack pointer. */
#define PROBE_GAP 1024u
#define PROBE_MAX_KIB (7u * 1024u)
/* What the probe reads is the deepest word written, and a frame can hold an
   unwritten buffer at its bottom. Anything written this close to the painted
   region's end may mean the stack went past it, so the reading is only a
   lower bound. */
#define PROBE_SATURATION_BYTES (16u * 1024u)

void d_measure_begin(char **argv)
{
    const char *setting = getenv("DISTILL_STACK_PROBE");
    if (!setting || !*setting || !argv) return;
    unsigned long long kib = strtoull(setting, NULL, 10);
    if (kib == 0) return;
    /* The painted region must stay inside the stack's limit, or painting it
       is the overflow. 7 MiB leaves room under Linux's default 8 MiB. */
    if (kib > PROBE_MAX_KIB) kib = PROBE_MAX_KIB;
    volatile unsigned char marker = 0;
    uintptr_t here = (uintptr_t)&marker;
#ifdef DISTILL_WINDOWS
    /* Windows builds argv on the heap. Paint inside the main thread's
       reserved stack and leave room for its guard and exception handling. */
    uintptr_t d_win_stack_top(void);
    uintptr_t top = d_win_stack_top();
    size_t reservation = d_proc_stack_limit();
    size_t margin = 64 * 1024 + PROBE_GAP;
    if (!reservation || top < here || top - here >= reservation) return;
    size_t available = reservation - (top - here);
    if (available <= margin) return;
    if (kib > (available - margin) / 1024) kib = (available - margin) / 1024;
    if (!kib) return;
    probe_top = top;
#else
    probe_top = (uintptr_t)argv;
#endif
    probe_high = (here - PROBE_GAP) & ~(uintptr_t)(sizeof(uint64_t) - 1);
    probe_low = probe_high - (uintptr_t)kib * 1024u;
    for (volatile uint64_t *word = (volatile uint64_t *)probe_high;
         (uintptr_t)word > probe_low;)
        *--word = probe_pattern;
}

bool d_measure_enabled(void)
{
    const char *path = getenv("DISTILL_STATS");
    return path && *path;
}

bool d_measure_stack_peak(size_t *peak, bool *saturated)
{
    if (!probe_low) return false;
    uintptr_t word = probe_low;
    while (word < probe_high && *(volatile const uint64_t *)word == probe_pattern)
        word += sizeof(uint64_t);
    *saturated = word - probe_low < PROBE_SATURATION_BYTES;
    *peak = (size_t)(probe_top - word);
    return true;
}

long d_measure_peak_rss_kib(void)
{
    return d_proc_peak_rss_kib();
}

#define LINE_FORMAT                                                         \
    "{\"process\":\"%s\",\"pid\":%ld,\"rss_peak_kib\":%ld,"                    \
    "\"stack_peak\":%s,\"stack_saturated\":%s%s%s}\n"

void d_measure_finish(const char *process, const char *fields)
{
    if (!d_measure_enabled()) return;
    long rss_kib = d_measure_peak_rss_kib();
    size_t peak = 0;
    bool saturated = false;
    bool probed = d_measure_stack_peak(&peak, &saturated);
    char stack[64];
    if (probed) snprintf(stack, sizeof(stack), "%zu", peak);
    else snprintf(stack, sizeof(stack), "null");
    const char *extra = fields && *fields ? fields : NULL;
    int length = snprintf(NULL, 0,
                          LINE_FORMAT,
                          process, d_proc_id(), rss_kib, stack,
                          saturated ? "true" : "false", extra ? "," : "", extra ? extra : "");
    if (length <= 0) return;
    char *line = malloc((size_t)length + 1);
    if (!line) return;
    snprintf(line, (size_t)length + 1,
             LINE_FORMAT,
             process, d_proc_id(), rss_kib, stack,
             saturated ? "true" : "false", extra ? "," : "", extra ? extra : "");
    /* One append, so the shell and the engines it spawns can share the file
       without interleaving inside a line. */
    d_fs_append(getenv("DISTILL_STATS"), line, (size_t)length);
    free(line);
}

/* The allocation header: the size, padded so the pointer handed out keeps
   malloc's alignment. */
#define HEAP_HEADER (alignof(max_align_t) > sizeof(size_t) ? alignof(max_align_t) : sizeof(size_t))

static void heap_account(DJsHeap *heap, size_t added, size_t removed)
{
    if (!heap) return;
    heap->current = heap->current - removed + added;
    if (heap->current > heap->peak) heap->peak = heap->current;
}

static size_t heap_size(const void *ptr)
{
    size_t size;
    memcpy(&size, (const unsigned char *)ptr - HEAP_HEADER, sizeof(size));
    return size;
}

void *d_js_heap_malloc(void *opaque, size_t size)
{
    if (size > SIZE_MAX - HEAP_HEADER) return NULL;
    unsigned char *base = malloc(size + HEAP_HEADER);
    if (!base) return NULL;
    memcpy(base, &size, sizeof(size));
    heap_account(opaque, size, 0);
    return base + HEAP_HEADER;
}

void *d_js_heap_calloc(void *opaque, size_t count, size_t size)
{
    if (size && count > (SIZE_MAX - HEAP_HEADER) / size) return NULL;
    size_t total = count * size;
    unsigned char *base = calloc(1, total + HEAP_HEADER);
    if (!base) return NULL;
    memcpy(base, &total, sizeof(total));
    heap_account(opaque, total, 0);
    return base + HEAP_HEADER;
}

void d_js_heap_free(void *opaque, void *ptr)
{
    if (!ptr) return;
    heap_account(opaque, 0, heap_size(ptr));
    free((unsigned char *)ptr - HEAP_HEADER);
}

void *d_js_heap_realloc(void *opaque, void *ptr, size_t size)
{
    if (!ptr) return d_js_heap_malloc(opaque, size);
    if (size == 0) { d_js_heap_free(opaque, ptr); return NULL; }
    if (size > SIZE_MAX - HEAP_HEADER) return NULL;
    size_t old = heap_size(ptr);
    unsigned char *base = realloc((unsigned char *)ptr - HEAP_HEADER, size + HEAP_HEADER);
    if (!base) return NULL;
    memcpy(base, &size, sizeof(size));
    heap_account(opaque, size, old);
    return base + HEAP_HEADER;
}

size_t d_js_heap_usable_size(const void *ptr)
{
    return ptr ? heap_size(ptr) : 0;
}
