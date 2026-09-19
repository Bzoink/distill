/* dterm on Linux: /dev/tty, termios and TIOCGWINSZ. */

#define _POSIX_C_SOURCE 200809L

#include "dterm.h"

#include "cutil.h"
#include "dclock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* The mode the terminal had before we changed it, kept where a signal
   handler can reach it. */
static int saved_handle = -1;
static struct termios saved_mode;

int d_term_open(char **error)
{
    if (error) *error = NULL;
    int handle = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (handle < 0) {
        if (error) {
            char buffer[160];
            snprintf(buffer, sizeof(buffer), "no terminal to run in (%s)", strerror(errno));
            *error = d_strdup(buffer);
        }
        return -1;
    }
    if (tcgetattr(handle, &saved_mode) != 0) {
        close(handle);
        if (error) *error = d_strdup("not a terminal");
        return -1;
    }
    saved_handle = handle;
    return handle;
}

void d_term_close(int handle)
{
    if (handle < 0) return;
    if (handle == saved_handle) {
        tcsetattr(handle, TCSAFLUSH, &saved_mode);
        saved_handle = -1;
    }
    close(handle);
}

bool d_term_size(int handle, DTermSize *size)
{
    struct winsize window = {0};
    *size = (DTermSize){0};
    if (ioctl(handle, TIOCGWINSZ, &window) != 0) return false;
    *size = (DTermSize){window.ws_col, window.ws_row, window.ws_xpixel, window.ws_ypixel};
    return true;
}

bool d_term_write(int handle, const void *data, size_t length)
{
    const char *at = data;
    while (length) {
        ssize_t written = write(handle, at, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        at += written;
        length -= (size_t)written;
    }
    return true;
}

size_t d_term_read(int handle, void *buffer, size_t size, int timeout_ms)
{
    uint64_t deadline = d_clock_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        uint64_t now = d_clock_ms();
        int left = now >= deadline ? 0 : (int)(deadline - now);
        struct pollfd fd = {handle, POLLIN, 0};
        int ready = poll(&fd, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
        ssize_t got = read(handle, buffer, size);
        if (got < 0 && errno == EINTR) continue;
        return got > 0 ? (size_t)got : 0;
    }
}

void d_term_emergency_restore(const char *reset, size_t length)
{
    if (saved_handle < 0) return;
    /* write(2) and tcsetattr are both async-signal-safe. */
    if (reset && length) {
        ssize_t ignored = write(saved_handle, reset, length);
        (void)ignored;
    }
    tcsetattr(saved_handle, TCSAFLUSH, &saved_mode);
}
