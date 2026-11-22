/* The interactive stdin console: a reader thread queues lines; the main loop
 * takes them between ticks (admin_exec runs on the server's thread). */
#ifndef PSP2I_SERVER_CONSOLE_H
#define PSP2I_SERVER_CONSOLE_H

#include <stddef.h>

int  console_start(void);                       /* 0 on success */
/* The next queued line (without the newline) into buf; 0 if none. */
int  console_poll(char *buf, size_t cap);
#endif
