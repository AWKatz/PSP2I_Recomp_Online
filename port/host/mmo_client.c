/* PSP2i MMO client start-up (ENABLE_MMO_NET builds only).
 *
 * client_config.toml, next to the game data (PSP2I_DATA):
 *
 *   server_ip     = "127.0.0.1"   # the server to join
 *   server_port   = 29001
 *   lan_discovery = true          # look for a server on the LAN when server_ip is unset
 *   discovery_port = 29000        # where LAN servers answer (psp2i_server --lan)
 *   player_name   = "Ann"         # sent in the handshake
 *   party_finder  = true          # open the party finder at the Matching Counter (F2 any time)
 *
 * A missing file means those defaults. With no server_ip and LAN discovery
 * on, the first server to answer a broadcast probe (with the same version
 * hash) is used; if none answers, 127.0.0.1. */

#include "mmo_client.h"
#include "party_ui.h"

#include <psprecomp/net.h>
#include <psprecomp/net_enet.h>
#include <psprecomp/net_lan.h>
#include <psprecomp/net_proto.h>
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
    long long dport = toml_get_int(&doc, "discovery_port", PSP_MMO_LAN_PORT);
    const char *name = toml_get_str(&doc, "player_name", "PSP2i");
    party_ui_init(toml_get_bool(&doc, "party_finder", 1));
    if (port < 1 || port > 65535) { psp_net_log("mmo: server_port %lld out of range, using %d", port, DEFAULT_PORT); port = DEFAULT_PORT; }
    char found[64];
    if (!*ip && lan) {
        if (dport < 1 || dport > 65535) dport = PSP_MMO_LAN_PORT;
        psp_mmo_lan_info info;
        if (psp_mmo_lan_discover((uint16_t)dport, 1500, &info, found, sizeof found) == 0) {
            if (info.version_hash != psp_mmo_fnv1a(PSP_MMO_VERSION_STRING)) {
                psp_net_log("mmo: LAN server '%s' at %s has version hash %08X, not ours (%08X, %s); ignoring it",
                            info.name, found, (unsigned)info.version_hash,
                            (unsigned)psp_mmo_fnv1a(PSP_MMO_VERSION_STRING), PSP_MMO_VERSION_STRING);
            } else {
                psp_net_log("mmo: found LAN server '%s' at %s:%u (%u/%u players)", info.name, found,
                            (unsigned)info.game_port, (unsigned)info.players, (unsigned)info.max_players);
                ip = found;
                port = info.game_port;
            }
        } else {
            psp_net_log("mmo: no LAN server answered on udp %lld", dport);
        }
    }
    if (!*ip) {
        ip = "127.0.0.1";
        psp_net_log("mmo: no server_ip, using %s", ip);
    }
    snprintf(server, sizeof server, "%s:%lld", ip, port);
    psp_net_log("mmo: server_ip %s, server_port %lld, lan_discovery %s, player_name '%s'", ip, port, lan ? "true" : "false", name);

    psp_net_mmo_config mc = { server, name, "psp2i-mmo " __DATE__ };
    psp_net_mmo_configure(&mc);
    toml_free(&doc);

    psp_net_enet_config ec = { .connect_timeout_ms = 3000 };
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
