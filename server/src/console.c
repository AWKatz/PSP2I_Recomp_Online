/* stdin console (see console.h). */
#include "console.h"

#include <stdio.h>
#include <string.h>

#define QLEN 32
#define LINE 1024

static char g_q[QLEN][LINE];
static int  g_head, g_tail;

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static CRITICAL_SECTION g_lock;
static void lock(void) { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
#else
#  include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

static void reader(void) {
    char line[LINE];
    while (fgets(line, sizeof line, stdin)) {            /* EOF (no terminal, a daemon): the thread ends */
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        lock();
        if ((g_tail + 1) % QLEN != g_head) {              /* full: the line is dropped */
            memcpy(g_q[g_tail], line, LINE);
            g_tail = (g_tail + 1) % QLEN;
        }
        unlock();
    }
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID u) { (void)u; reader(); return 0; }
int console_start(void) {
    InitializeCriticalSection(&g_lock);
    HANDLE h = CreateThread(NULL, 0, thread_main, NULL, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
}
#else
static void *thread_main(void *u) { (void)u; reader(); return NULL; }
int console_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
#endif

int console_poll(char *buf, size_t cap) {
    int got = 0;
    lock();
    if (g_head != g_tail) {
        snprintf(buf, cap, "%s", g_q[g_head]);
        g_head = (g_head + 1) % QLEN;
        got = 1;
    }
    unlock();
    return got;
}
