/* ddisplay on Linux: which sinks this build has, chosen by the spec.

   The memory sink is in every build. The console is fbdev, the tty and
   evdev. The X11 window is compiled in only with DISTILL_DISPLAY_X11, which
   only distill-fb-x11 defines, so libX11 cannot reach distill-fb. */

#include "cutil.h"
#include "display_sink.h"

DDisplay *d_display_console_open(const char *spec, char **error);
void d_display_console_restore(void);
#ifdef DISTILL_DISPLAY_X11
DDisplay *d_display_x11_open(const char *spec, char **error);
#endif

DDisplay *d_display_open(const char *spec, char **error)
{
    *error = NULL;
    if (!spec || !*spec) spec = "console";
    if (d_starts(spec, "memory:")) return d_display_memory_open(spec, error);
    if (!strcmp(spec, "console") || d_starts(spec, "console:")) return d_display_console_open(spec, error);
#ifdef DISTILL_DISPLAY_X11
    if (!strcmp(spec, "x11") || d_starts(spec, "x11:")) return d_display_x11_open(spec, error);
#endif
    char message[320];
    snprintf(message, sizeof(message), "no display \"%.200s\" in this build (memory:WxH, console%s)", spec,
#ifdef DISTILL_DISPLAY_X11
             ", x11:WxH"
#else
             ""
#endif
    );
    *error = d_strdup(message);
    return NULL;
}

void d_display_emergency_restore(void)
{
    d_display_console_restore();
}
