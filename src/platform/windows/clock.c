#include "dclock.h"
#include "win.h"
#include <time.h>

uint64_t d_clock_ms(void)
{
    return GetTickCount64();
}

bool d_clock_wall_trusted(void)
{
    const char *test = getenv("DISTILL_TEST_UNTRUSTED_TIME");
    return !(test && *test && *test != '0') && time(NULL) >= 1767225600;
}

void d_clock_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}
