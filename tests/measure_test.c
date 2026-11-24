/* The M0 instruments, checked against depths and sizes known in advance.

   A stack probe that reads the wrong number fails silently: every budget
   chosen from it would be wrong in the same direction. So the probe is run
   against recursion of a known depth, and must report that depth -- not less,
   and not much more -- and must say so when the recursion outruns the region
   it painted. */

#define _POSIX_C_SOURCE 200809L

#include "measure.h"

#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_BYTES 2048u

static char **stack_top;

/* Each level keeps FRAME_BYTES live on the stack. The volatile writes and the
   use of the result stop the compiler from shrinking or merging the frames. */
/* Each level keeps FRAME_BYTES live on the stack, and every 8-byte word of
   the frame is written: the probe compares whole words, and an untouched
   word inside a frame would stop its scan above the frame's deepest byte.
   A sparser stride also invites the optimizer to strip the unwritten part
   of the array and allocate frames far smaller than FRAME_BYTES, which
   would make the probe's arithmetic lie. The call goes through a volatile
   pointer so the optimizer cannot see that the callee is the function
   itself and turn accumulation-minus-call into an iterative loop over one
   shared frame, which would likewise collapse the levels. */
static unsigned (*volatile recurse)(unsigned levels);

static unsigned real_recurse(unsigned levels)
{
    volatile unsigned char frame[FRAME_BYTES];
    for (unsigned i = 0; i < FRAME_BYTES; i += 8) frame[i] = (unsigned char)levels;
    if (levels == 0) return frame[0];
    return recurse(levels - 1) + frame[FRAME_BYTES - 64];
}

static size_t peak_now(bool *saturated)
{
    size_t peak = 0;
    bool probed = d_measure_stack_peak(&peak, saturated);
    assert(probed);
    return peak;
}

static void check_stack(void)
{
    bool saturated = true;
    size_t before = peak_now(&saturated);
    assert(!saturated);
    /* Depth is measured from the initial stack pointer, not from wherever the
       probe was armed: this very frame is already that deep. */
    volatile unsigned char here = 0;
    assert(before >= (size_t)((uintptr_t)stack_top - (uintptr_t)&here));

    /* 256 KiB of frames: the reported growth must cover them. Call overhead
       adds a little per level, so allow an eighth on top. */
    unsigned levels = (256u * 1024u) / FRAME_BYTES;
    volatile unsigned sink = recurse(levels);
    (void)sink;
    size_t after = peak_now(&saturated);
    assert(!saturated);
    size_t grown = after - before;
    size_t expected = (size_t)levels * FRAME_BYTES;
    printf("stack: %zu bytes reported for %zu of frames\n", grown, expected);
    assert(grown >= expected);
    assert(grown <= expected + expected / 8);

    /* A shallower call afterwards must not lower the peak. */
    sink = recurse(4);
    assert(peak_now(&saturated) == after);

    /* Past the painted region (1 MiB, set in main), the probe must say the
       real peak is at least what it reports rather than pass it off as exact. */
    sink = recurse((1536u * 1024u) / FRAME_BYTES);
    (void)peak_now(&saturated);
    assert(saturated);
}

static void check_heap(void)
{
    DJsHeap heap = {0};
    unsigned char *a = d_js_heap_malloc(&heap, 100);
    assert(a && heap.current == 100 && d_js_heap_usable_size(a) == 100);
    /* The pointer handed out keeps malloc's alignment. */
    assert(((uintptr_t)a % alignof(max_align_t)) == 0);
    unsigned char *b = d_js_heap_calloc(&heap, 10, 30);
    assert(b && heap.current == 400 && b[299] == 0);
    a = d_js_heap_realloc(&heap, a, 1000);
    assert(a && heap.current == 1300 && heap.peak == 1300);
    a = d_js_heap_realloc(&heap, a, 10);
    assert(a && heap.current == 310 && heap.peak == 1300);
    d_js_heap_free(&heap, a);
    d_js_heap_free(&heap, b);
    d_js_heap_free(&heap, NULL);
    assert(heap.current == 0 && heap.peak == 1300);
    d_js_heap_free(&heap, d_js_heap_realloc(&heap, NULL, 0));
    /* An impossible size is refused, not wrapped into a small allocation. */
    assert(d_js_heap_calloc(&heap, SIZE_MAX / 2, 4) == NULL);
    assert(heap.current == 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    setenv("DISTILL_STACK_PROBE", "1024", 1);
    stack_top = argv;
    recurse = real_recurse;
    d_measure_begin(argv);
    check_stack();
    check_heap();
    puts("measure: ok");
    return 0;
}
