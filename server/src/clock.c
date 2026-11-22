/* Monotonic time and absolute-deadline sleep: Linux, macOS, Windows. */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#  define _DARWIN_C_SOURCE
#  define _DEFAULT_SOURCE
#endif
#include "clock.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>

uint64_t clk_now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

void clk_sleep_until_ns(uint64_t t) {
    static HANDLE timer;
    const uint64_t now = clk_now_ns();
    if (now >= t) return;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((t - now) / 100);          /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    Sleep((DWORD)((t - now) / 1000000));
}

uint64_t clk_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    const uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime;
    const uint64_t ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100;
}

uint64_t clk_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS m;
    return GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m) ? (uint64_t)m.WorkingSetSize : 0;
}

#else
#  include <sys/resource.h>
#  include <time.h>
#  ifdef __APPLE__
#    include <mach/mach.h>
#  else
#    include <stdio.h>
#    include <unistd.h>
#  endif

uint64_t clk_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void clk_sleep_until_ns(uint64_t t) {
#  if defined(__linux__)
    struct timespec ts = { (time_t)(t / 1000000000u), (long)(t % 1000000000u) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {}   /* EINTR: again */
#  else
    for (;;) {                                                /* no absolute sleep: sleep the rest */
        const uint64_t now = clk_now_ns();
        if (now >= t) return;
        const uint64_t d = t - now;
        struct timespec ts = { (time_t)(d / 1000000000u), (long)(d % 1000000000u) };
        nanosleep(&ts, NULL);
    }
#  endif
}

uint64_t clk_cpu_ns(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
    return ((uint64_t)r.ru_utime.tv_sec + (uint64_t)r.ru_stime.tv_sec) * 1000000000u +
           ((uint64_t)r.ru_utime.tv_usec + (uint64_t)r.ru_stime.tv_usec) * 1000u;
}

uint64_t clk_rss_bytes(void) {
#  ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return (uint64_t)info.resident_size;
#  else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, rss = 0;
    const int ok = fscanf(f, "%lu %lu", &size, &rss) == 2;
    fclose(f);
    return ok ? (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
#  endif
}
#endif
