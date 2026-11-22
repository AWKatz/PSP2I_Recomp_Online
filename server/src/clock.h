/* Monotonic time and sleeping to an absolute deadline (the server's tick). */
#ifndef PSP2I_SERVER_CLOCK_H
#define PSP2I_SERVER_CLOCK_H

#include <stdint.h>

/* Nanoseconds on a monotonic clock (arbitrary origin). */
uint64_t clk_now_ns(void);
/* Sleep until clk_now_ns() >= t (returns at once if already past). */
void     clk_sleep_until_ns(uint64_t t);
/* Process CPU time used so far (user + system), in nanoseconds. */
uint64_t clk_cpu_ns(void);
/* Resident memory now, in bytes (0 if unknown). */
uint64_t clk_rss_bytes(void);

#endif
