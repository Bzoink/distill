#ifndef DISTILL_PLATFORM_DCLOCK_H
#define DISTILL_PLATFORM_DCLOCK_H

/* Time, for the core. Wall-clock dates, where anything needs them, are
   standard C's time(); what the core needs from the platform is a clock that
   never jumps, for budgets and timeouts, and a way to wait. */

#include <stdint.h>

/* Milliseconds on a monotonic clock with an arbitrary origin. */
uint64_t d_clock_ms(void);

/* Block the calling thread for about `ms` milliseconds. */
void d_clock_sleep_ms(uint32_t ms);

/* Whether the wall clock can be believed. Certificates are checked against
   it, and Muroxi's TLS refuses to connect at all without synchronized time;
   a refusal the user can fix by setting the clock must not read as a network
   failure they cannot. DISTILL_TEST_UNTRUSTED_TIME=1 says no, for tests. */
bool d_clock_wall_trusted(void);

#endif
