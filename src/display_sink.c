/* The display interface's dispatch and the blitter every sink shares. */

#include "display_sink.h"

#include <string.h>

DDisplayMode d_display_mode(const DDisplay *display)
{
    return display->ops->mode(display);
}

size_t d_display_handles(DDisplay *display, int *handles, size_t capacity)
{
    return display->ops->handles(display, handles, capacity);
}

int d_display_wait_ms(DDisplay *display)
{
    return display->ops->wait_ms(display);
}

bool d_display_next_event(DDisplay *display, DEvent *event)
{
    return display->ops->next_event(display, event);
}

void d_display_present(DDisplay *display, const uint32_t *pixels, int stride,
                       const DRenderRect *damage, size_t damage_count)
{
    display->ops->present(display, pixels, stride, damage, damage_count);
}

void d_display_report(DDisplay *display, bool busy, const char *state, const char *title)
{
    if (display->ops->report) display->ops->report(display, busy, state, title);
}

void d_display_close(DDisplay *display)
{
    if (display) display->ops->close(display);
}

void d_display_copy(uint32_t *device, int device_stride, int width, int height,
                    const uint32_t *pixels, int stride,
                    const DRenderRect *damage, size_t damage_count)
{
#ifdef DISTILL_DISPLAY_DEFECT
    /* The negative control for the stride gate: the blit every framebuffer
       tutorial writes, one copy of the whole buffer, as if both sides were
       width * 4 bytes a row. The memory sink's padded device must catch it. */
    (void)device_stride; (void)stride; (void)damage; (void)damage_count;
    memcpy(device, pixels, (size_t)width * (size_t)height * 4);
#else
    for (size_t i = 0; i < damage_count; ++i) {
        DRenderRect r = damage[i];
        if (r.x < 0) { r.width += r.x; r.x = 0; }
        if (r.y < 0) { r.height += r.y; r.y = 0; }
        if (r.x + r.width > width) r.width = width - r.x;
        if (r.y + r.height > height) r.height = height - r.y;
        if (r.width <= 0 || r.height <= 0) continue;
        for (int y = r.y; y < r.y + r.height; ++y) {
            const uint32_t *from = (const uint32_t *)((const unsigned char *)pixels + (size_t)y * (size_t)stride) + r.x;
            uint32_t *to = (uint32_t *)((unsigned char *)device + (size_t)y * (size_t)device_stride) + r.x;
            memcpy(to, from, (size_t)r.width * 4);
        }
    }
#endif
}

bool d_display_parse_size(const char *text, int *width, int *height)
{
    int w = 0, h = 0;
    const char *p = text;
    while (*p >= '0' && *p <= '9' && w < 100000) w = w * 10 + (*p++ - '0');
    if (*p != 'x' || !w) return false;
    ++p;
    while (*p >= '0' && *p <= '9' && h < 100000) h = h * 10 + (*p++ - '0');
    if (!h) return false;
    *width = w;
    *height = h;
    return true;
}
