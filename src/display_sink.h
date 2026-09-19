#ifndef DISTILL_DISPLAY_SINK_H
#define DISTILL_DISPLAY_SINK_H

/* What a display sink implements (ddisplay.h). The display is the one
   platform interface chosen at run time, so it is the one with a table of
   functions; every other interface is chosen at link time. */

#include "ddisplay.h"

typedef struct {
    DDisplayMode (*mode)(const DDisplay *display);
    size_t (*handles)(DDisplay *display, int *handles, size_t capacity);
    int (*wait_ms)(DDisplay *display);
    bool (*next_event)(DDisplay *display, DEvent *event);
    void (*present)(DDisplay *display, const uint32_t *pixels, int stride,
                    const DRenderRect *damage, size_t damage_count);
    void (*report)(DDisplay *display, bool busy, const char *state, const char *title);
    void (*close)(DDisplay *display);
} DDisplayOps;

/* Every sink's own struct starts with this. */
struct DDisplay {
    const DDisplayOps *ops;
};

/* The memory sink: portable, and in every build. */
DDisplay *d_display_memory_open(const char *spec, char **error);

/* Copy the damaged rectangles from the back buffer onto a device, row by
   row, each side at its own stride: "the whole of the blitter". Rectangles
   are clipped to the device. */
void d_display_copy(uint32_t *device, int device_stride, int width, int height,
                    const uint32_t *pixels, int stride,
                    const DRenderRect *damage, size_t damage_count);

/* Parse "WxH" at `text`; false if it is not two positive numbers. */
bool d_display_parse_size(const char *text, int *width, int *height);

#endif
