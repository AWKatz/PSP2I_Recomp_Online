/* PSP2i MMO client start-up (ENABLE_MMO_NET builds only).
 *
 * client_config.toml, next to the game data (PSP2I_DATA):
 *
 *   server_ip     = "127.0.0.1"   # the server to join
 *   server_port   = 29001
 *   lan_discovery = true          # look for a server on the LAN when server_ip is unset
 *   player_name   = "Ann"         # sent in the handshake
 *
 * A missing file means those defaults. LAN discovery needs the server's
 * discovery responder (Task 2.1); until then it falls back to 127.0.0.1. */

#include "mmo_client.h"

#include <psprecomp/net.h>
#include <psprecomp/net_enet.h>
#include <psprecomp/toml_lite.h>

#include <stdio.h>

#define DEFAULT_PORT 29001

static psp_net_enet *g_enet;

void mmo_client_init(const char *base) {
    char path[1200], server[160];
    snprintf(path, sizeof path, "%s/client_config.toml", base);
    toml_doc doc;
    const int rc = toml_parse_file(path, &doc);
    if (rc == -2) psp_net_log("mmo: %s not found, using defaults", path);
    else if (rc != 0) psp_net_log("mmo: %s: %s; using defaults", path, doc.error);
    else psp_net_log("mmo: read %s", path);

    const char *ip = toml_get_str(&doc, "server_ip", "");
    long long port = toml_get_int(&doc, "server_port", DEFAULT_PORT);
    const int lan = toml_get_bool(&doc, "lan_discovery", 1);
    const char *name = toml_get_str(&doc, "player_name", "PSP2i");
    if (port < 1 || port > 65535) { psp_net_log("mmo: server_port %lld out of range, using %d", port, DEFAULT_PORT); port = DEFAULT_PORT; }
    if (!*ip) {
        ip = "127.0.0.1";
        psp_net_log(lan ? "mmo: no server_ip; LAN discovery arrives with the server (Task 2.1), using %s"
                        : "mmo: no server_ip, using %s", ip);
    }
    snprintf(server, sizeof server, "%s:%lld", ip, port);
    psp_net_log("mmo: server_ip %s, server_port %lld, lan_discovery %s, player_name '%s'", ip, port, lan ? "true" : "false", name);

    psp_net_mmo_config mc = { server, name, "psp2i-mmo " __DATE__ };
    psp_net_mmo_configure(&mc);
    toml_free(&doc);

    psp_net_enet_config ec = { NULL, 0, 3000, NULL, NULL, NULL };
    g_enet = psp_net_enet_create(&ec);
    psp_net_set_transport(psp_net_enet_transport(g_enet));
    psp_net_set_forced_off(0);
    psp_net_set_wlan_forced_on(1);   /* no "WLAN switch is off" stop */
}

void mmo_client_frame(void) { psp_net_mmo_pump(); }

void mmo_client_shutdown(void) {
    psp_net_set_transport(NULL);     /* shuts the transport down */
    psp_net_enet_destroy(g_enet);
    g_enet = NULL;
}
