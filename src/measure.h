#ifndef DISTILL_MEASURE_H
#define DISTILL_MEASURE_H

/* Resource measurement for the Muroxi budgets (muroxi_compat.md, M0).

   Off unless the environment asks for it, so an ordinary run pays nothing:
   DISTILL_STATS names a file that each measured process appends one JSON line
   to when it finishes, and DISTILL_STACK_PROBE sets how many KiB of the main
   thread's stack to paint so the deepest point reached can be read back.

   Painting commits every page it touches, so a run with the probe on reports
   an inflated peak RSS. Measure stack and memory in separate runs. */

#include <stddef.h>
#include <stdint.h>

/* Call first thing in main, with main's own argv. On the System V ABI the
   argv array sits where the initial stack pointer was, so depth is measured
   from there and includes the C runtime's frames and main's own. */
void d_measure_begin(char **argv);

/* The deepest the main thread's stack has gone so far, in bytes below the
   initial stack pointer. False when the probe is off. */
bool d_measure_stack_peak(size_t *peak, bool *saturated);

/* Peak resident memory so far, in KiB: the high-water mark of this address
   space where the platform reports one, else getrusage's. */
long d_measure_peak_rss_kib(void);

/* Whether DISTILL_STATS is set. */
bool d_measure_enabled(void);

/* Append one line to DISTILL_STATS:
   {"process":NAME,"pid":..,"rss_peak_kib":..,"stack_peak":..,
    "stack_saturated":..,FIELDS}
   FIELDS is a JSON object body without braces, or NULL. stack_peak is null
   when the probe was off; stack_saturated means the stack was written within
   16 KiB of the painted region's end, so the real peak is at least
   stack_peak. The probe reads the deepest word written: an unwritten buffer
   at the bottom of the deepest frame is not seen. */
void d_measure_finish(const char *process, const char *fields);

/* A QuickJS allocator that knows each allocation's size without asking the
   C library. QuickJS's default asks malloc_usable_size(), which only glibc,
   Apple, Windows and the BSDs have; elsewhere it reports 0, and the
   runtime's memory accounting -- and so JS_SetMemoryLimit -- counts nothing.
   A size header costs 16 bytes per allocation and works everywhere. */
typedef struct {
    size_t current;
    size_t peak;
} DJsHeap;

void *d_js_heap_calloc(void *opaque, size_t count, size_t size);
void *d_js_heap_malloc(void *opaque, size_t size);
void d_js_heap_free(void *opaque, void *ptr);
void *d_js_heap_realloc(void *opaque, void *ptr, size_t size);
size_t d_js_heap_usable_size(const void *ptr);

#endif
