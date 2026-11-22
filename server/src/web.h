/* The embedded web admin panel and its REST API (/api/v1/...).
 *
 * A small HTTP/1.1 server (non-blocking sockets, one request per
 * connection) run from the server's tick: no threads. The page, script and
 * style sheet are compiled in (web/ -> web_assets.c). Live telemetry is a
 * Server-Sent Events stream.
 *
 * Authentication: POST /api/v1/login with an admin account (accounts table,
 * is_admin = 1) returns a session token; every other API call needs
 * "Authorization: Bearer <token>" (the event stream takes ?token=). Failed
 * logins are throttled per address. Bind 127.0.0.1 (the default) unless the
 * panel must be reachable from elsewhere. */
#ifndef PSP2I_SERVER_WEB_H
#define PSP2I_SERVER_WEB_H

#include <stdint.h>

#include "admin.h"

typedef struct web web;

typedef struct {
    const char *bind;              /* default 127.0.0.1 */
    uint16_t    port;              /* 0 = any free port */
    admin_ctx  *admin;             /* the server, its database, telemetry */
    void      (*log)(const char *line);
} web_config;

web     *web_create(const web_config *cfg);    /* NULL if it cannot listen (logged) */
uint16_t web_port(const web *w);
void     web_tick(web *w, uint64_t now_ms);
void     web_destroy(web *w);

#endif
