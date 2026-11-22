/* psprecomp — the ad hoc / PSN libraries in the MMO build (ENABLE_MMO_NET).
 *
 * Replaces offline.c's stubs (registered after it; re-registering replaces):
 *   - every stub is traced: its name and first arguments go to the network
 *     log, so the calls the game makes on its way into multiplayer are visible;
 *     its result stays what offline.c answered, unless implemented here;
 *   - sceNetAdhocctlInit, the game's entry into multiplayer, brings the
 *     installed transport up, joins the configured server and sends
 *     MSG_C2S_HANDSHAKE.
 * The rest of ad hoc (PDP/PTP, Adhocctl groups, Matching) follows in later tasks. */

#include "psprecomp/hle.h"
#include "psprecomp/net.h"
#include "psprecomp/net_proto.h"
#include "psprecomp/net_transport.h"

#include <stdio.h>
#include <string.h>

static char g_server[128] = "127.0.0.1:29001";
static char g_name[PSP_MMO_NAME_LEN] = "PSP2i";
static char g_version[PSP_MMO_VERSION_LEN] = "psp2i-mmo";
static int  g_joined;
static uint64_t g_session;

void psp_net_mmo_configure(const psp_net_mmo_config *cfg) {
    if (!cfg) return;
    if (cfg->server)      snprintf(g_server, sizeof g_server, "%s", cfg->server);
    if (cfg->player_name) snprintf(g_name, sizeof g_name, "%s", cfg->player_name);
    if (cfg->version)     snprintf(g_version, sizeof g_version, "%s", cfg->version);
}

/* ---- tracing the stubs ----------------------------------------------------------------- */

#define MAX_TRACED 160
static struct { uint32_t nid; int succeeds; } g_traced[MAX_TRACED];
static int g_ntraced;

static void hle_traced(void) {
    const uint32_t nid = psp_hle_current_nid();
    int ok = 0;
    for (int i = 0; i < g_ntraced; i++) if (g_traced[i].nid == nid) { ok = g_traced[i].succeeds; break; }
    const char *name = psp_hle_name_of(nid);
    psp_net_log("mmo: %s%s0x%08X(0x%08X, 0x%08X, 0x%08X) -> %s", name ? name : "", name ? " " : "NID ", nid,
                psp_arg(0), psp_arg(1), psp_arg(2), ok ? "0" : "0x80020001 (not implemented)");
    psp_ret(ok ? 0 : SCE_KERNEL_ERROR_ERROR);
}

/* ---- the handshake ---------------------------------------------------------------------------- */

static int mmo_connect(void) {
    const psp_net_transport *t = psp_net_get_transport();
    if (t == psp_net_null_transport()) { psp_net_log("mmo: no transport installed"); return -1; }
    uint8_t mac[6];
    psp_net_ether_addr(mac);
    if (t->initialize(t->ctx, mac) != PSP_NET_OK) { psp_net_log("mmo: transport %s failed to start", t->name); return -1; }
    psp_net_log("mmo: WLAN/PSN checks bypassed; joining %s over %s", g_server, t->name);
    if (t->join_session(t->ctx, g_server, "PSP2I") != PSP_NET_OK) { psp_net_log("mmo: could not reach %s", g_server); return -1; }

    uint8_t m[PSP_MMO_HANDSHAKE_LEN];
    psp_mmo_put_hdr(m, MSG_C2S_HANDSHAKE);
    memcpy(m + PSP_MMO_HDR_LEN, mac, 6);
    psp_mmo_put_str(m + PSP_MMO_HDR_LEN + 6, g_name, PSP_MMO_NAME_LEN);
    psp_mmo_put_str(m + PSP_MMO_HDR_LEN + 6 + PSP_MMO_NAME_LEN, g_version, PSP_MMO_VERSION_LEN);
    psp_net_peer server = { { 0 }, 1 };   /* the host: id 1 */
    const int rc = t->send_packet(t->ctx, &server, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m);
    psp_net_log("mmo: sent MSG_C2S_HANDSHAKE to %s (%u bytes, mac %02X:%02X:%02X:%02X:%02X:%02X, name '%s') -> %d",
                g_server, (unsigned)sizeof m, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], g_name, rc);
    g_joined = rc == PSP_NET_OK;
    return g_joined ? 0 : -1;
}

/* sceNetAdhocctlInit(stacksize, priority, product) */
static void hle_AdhocctlInit(void) {
    char product[10] = { 0 };
    if (psp_arg(2)) psp_mem_read_block(product, psp_arg(2) + 4, 9);
    psp_net_log("mmo: sceNetAdhocctlInit(stack 0x%X, prio %u, product '%s')", psp_arg(0), psp_arg(1), product);
    if (!g_joined) mmo_connect();
    psp_ret(0);   /* the library starts either way, as it would on a PSP */
}

static void on_packet(const psp_net_packet *p, void *user) {
    (void)user;
    if (p->port != PSP_MMO_CTRL_PORT) return;   /* game traffic: later tasks */
    const uint16_t type = psp_mmo_type(p->data, p->len);
    if (type == MSG_S2C_HANDSHAKE_ACK && p->len >= PSP_MMO_HANDSHAKE_ACK_LEN) {
        const uint8_t *d = p->data + PSP_MMO_HDR_LEN;
        const uint32_t status = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        uint64_t sid = 0; for (int i = 7; i >= 0; i--) sid = (sid << 8) | d[4 + i];
        g_session = sid;
        psp_net_log("mmo: MSG_S2C_HANDSHAKE_ACK status %u, session %llu", status, (unsigned long long)sid);
    } else {
        psp_net_log("mmo: control message 0x%04X (%u bytes) ignored", type, p->len);
    }
}

void psp_net_mmo_pump(void) {
    if (!g_joined) return;
    const psp_net_transport *t = psp_net_get_transport();
    t->poll_packets(t->ctx, on_packet, NULL);
}

void psp_net_mmo_register(void) {
    uint32_t nid; const char *lib, *name; int ok;
    for (int i = 0; psp_offline_stub(i, &nid, &lib, &name, &ok) && g_ntraced < MAX_TRACED; i++) {
        g_traced[g_ntraced].nid = nid; g_traced[g_ntraced].succeeds = ok; g_ntraced++;
        psp_hle_register(nid, lib, name, hle_traced);
    }
    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit", hle_AdhocctlInit);
}
