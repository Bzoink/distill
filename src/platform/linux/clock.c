/* dclock on Linux: CLOCK_MONOTONIC and nanosleep. */

#define _POSIX_C_SOURCE 200809L

#include "dclock.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

uint64_t d_clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* 2026-01-01T00:00:00Z. A clock before a date this build cannot predate is
   certainly wrong; Linux gives no finer signal than that. */
#define TRUSTED_TIME_FLOOR 1767225600

bool d_clock_wall_trusted(void)
{
    const char *test = getenv("DISTILL_TEST_UNTRUSTED_TIME");
    if (test && *test && *test != '0') return false;
    return time(NULL) >= TRUSTED_TIME_FLOOR;
}

void d_clock_sleep_ms(uint32_t ms)
{
    struct timespec delay = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}
