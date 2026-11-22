/* psprecomp — the ad hoc / PSN libraries in the MMO build (ENABLE_MMO_NET).
 *
 * Replaces offline.c's stubs (registered after it; re-registering replaces):
 *   - every stub is traced: its name and first arguments go to the network
 *     log, so the calls the game makes on its way into multiplayer are visible;
 *     its result stays what offline.c answered, unless implemented here;
 *   - sceNetAdhocctlInit, the game's entry into multiplayer, brings the
 *     installed transport up, joins the configured server and sends
 *     MSG_C2S_HANDSHAKE.
 *   - sceNetAdhocctlAddHandler / DelHandler / GetState, and the network
 *     dialog's ad hoc actions: the game's Form Party connects to its group
 *     and its handler gets the CONNECT event (delivered between frames).
 * The rest of ad hoc (Matching, PDP/PTP, scanning for other groups) follows. */

#include "adhoc_internal.h"

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

/* ---- Adhocctl event handlers ------------------------------------------------------------------- */

/* The game's handlers: handler(event, error, arg), called on Adhocctl events. */
#define MAX_HANDLERS 4
static struct { uint32_t func, arg; } g_handlers[MAX_HANDLERS];

/* sceNetAdhocctlAddHandler(handler, arg) -> handler id */
static void hle_AdhocctlAddHandler(void) {
    const uint32_t fn = psp_arg(0), arg = psp_arg(1);
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (g_handlers[i].func) continue;
        g_handlers[i].func = fn;
        g_handlers[i].arg = arg;
        psp_net_log("mmo: sceNetAdhocctlAddHandler(0x%08X, 0x%08X) -> %d", fn, arg, i);
        psp_ret((uint32_t)i);
        return;
    }
    psp_net_log("mmo: sceNetAdhocctlAddHandler: no free slot");
    psp_ret(0x80410B12u);                         /* SCE_NET_ADHOCCTL_ERROR_TOO_MANY_HANDLERS */
}

/* sceNetAdhocctlDelHandler(id) */
static void hle_AdhocctlDelHandler(void) {
    const uint32_t id = psp_arg(0);
    if (id < MAX_HANDLERS && g_handlers[id].func) { g_handlers[id].func = 0; psp_ret(0); }
    else psp_ret(0x80410B07u);                    /* SCE_NET_ADHOCCTL_ERROR_INVALID_ARG */
    psp_net_log("mmo: sceNetAdhocctlDelHandler(%u)", id);
}

/* ---- Adhocctl state and events --------------------------------------------------------------------- */

enum { ADHOCCTL_STATE_DISCONNECTED = 0, ADHOCCTL_STATE_CONNECTED = 1, ADHOCCTL_STATE_SCANNING = 2, ADHOCCTL_STATE_GAMEMODE = 3 };
enum { ADHOCCTL_EVENT_ERROR = 0, ADHOCCTL_EVENT_CONNECT = 1, ADHOCCTL_EVENT_DISCONNECT = 2, ADHOCCTL_EVENT_SCAN = 3, ADHOCCTL_EVENT_GAME = 4 };
static int  g_ctl_state;
static char g_group[9];
static struct { int event; uint32_t error; } g_events[16];
static int g_nevents;

static void queue_event(int ev, uint32_t err) {
    if (g_nevents < 16) { g_events[g_nevents].event = ev; g_events[g_nevents].error = err; g_nevents++; }
}

/* The game's handlers run between frames, as the firmware's Adhocctl thread would call them. */
static void deliver_events(void) {
    for (int k = 0; k < g_nevents; k++)
        for (int i = 0; i < MAX_HANDLERS; i++)
            if (g_handlers[i].func) {
                psp_net_log("mmo: Adhocctl event %d (error 0x%08X) -> handler 0x%08X", g_events[k].event, g_events[k].error, g_handlers[i].func);
                psp_call_guest(g_handlers[i].func, (uint32_t)g_events[k].event, g_events[k].error, g_handlers[i].arg);
            }
    g_nevents = 0;
}

/* The network dialog's ad hoc actions: join (or create) the group named in
 * its ad hoc parameters (pspUtilityNetconfData.adhocparam: group name[8], timeout). */
static int adhoc_dialog(int action, uint32_t param) {
    const uint32_t ap = param ? psp_read32(param + 0x34) : 0;
    memset(g_group, 0, sizeof g_group);
    if (ap) psp_mem_read_block(g_group, ap, 8);
    for (int i = 0; i < 8; i++) if (g_group[i] && (g_group[i] < 0x20 || g_group[i] > 0x7E)) g_group[i] = '?';
    if (!g_joined) {
        psp_net_log("mmo: network dialog (action %d): not connected to a server -- failed", action);
        return (int)0x80410B05u;                              /* SCE_NET_ADHOCCTL_ERROR_TIMEOUT: the game shows its network error */
    }
    g_ctl_state = ADHOCCTL_STATE_CONNECTED;
    queue_event(ADHOCCTL_EVENT_CONNECT, 0);
    psp_net_log("mmo: network dialog (action %d): connected to group '%s'", action, g_group);
    return 0;
}

static void connect_group(const char *how, uint32_t name_addr) {
    memset(g_group, 0, sizeof g_group);
    if (name_addr) psp_mem_read_block(g_group, name_addr, 8);
    for (int i = 0; i < 8; i++) if (g_group[i] && (g_group[i] < 0x20 || g_group[i] > 0x7E)) g_group[i] = '?';
    g_ctl_state = ADHOCCTL_STATE_CONNECTED;
    queue_event(ADHOCCTL_EVENT_CONNECT, 0);
    psp_net_log("mmo: %s: connected to group '%s'", how, g_group);
}

/* sceNetAdhocctlConnect(group name*) / Create(group name*) */
static void hle_AdhocctlConnect(void) { connect_group("sceNetAdhocctlConnect", psp_arg(0)); psp_ret(0); }
static void hle_AdhocctlCreate(void) { connect_group("sceNetAdhocctlCreate", psp_arg(0)); psp_ret(0); }
/* sceNetAdhocctlJoin(SceNetAdhocctlScanInfo*): next(4), channel(4), group name(8), ... */
static void hle_AdhocctlJoin(void) { connect_group("sceNetAdhocctlJoin", psp_arg(0) ? psp_arg(0) + 8 : 0); psp_ret(0); }

/* sceNetAdhocctlDisconnect() */
static void hle_AdhocctlDisconnect(void) {
    if (g_ctl_state != ADHOCCTL_STATE_DISCONNECTED) {
        g_ctl_state = ADHOCCTL_STATE_DISCONNECTED;
        queue_event(ADHOCCTL_EVENT_DISCONNECT, 0);
    }
    psp_net_log("mmo: sceNetAdhocctlDisconnect (group '%s')", g_group);
    psp_ret(0);
}

/* sceNetAdhocctlGetParameter(SceNetAdhocctlParameter*): channel s32, group name[8], bssid[6], nickname[128] */
static void hle_AdhocctlGetParameter(void) {
    const uint32_t p = psp_arg(0);
    if (p) {
        uint8_t mac[6];
        psp_net_ether_addr(mac);
        psp_write32(p, 1);
        psp_mem_write_block(p + 4, g_group, 8);
        psp_mem_write_block(p + 12, mac, 6);
        char nick[128] = { 0 };
        snprintf(nick, sizeof nick, "%s", g_name);
        psp_mem_write_block(p + 18, nick, 128);
    }
    psp_ret(0);
}

/* sceNetAdhocctlGetState(int *state) */
static void hle_AdhocctlGetState(void) {
    if (psp_arg(0)) psp_write32(psp_arg(0), (uint32_t)g_ctl_state);
    psp_ret(0);
}

int mmo_net_ready(void) { return g_joined; }

static void on_packet(const psp_net_packet *p, void *user) {
    (void)user;
    if (p->port != PSP_MMO_CTRL_PORT) {
        if ((int)p->port == mmo_matching_port()) mmo_matching_on_packet(p);
        return;                                   /* other game traffic (PDP/PTP): later */
    }
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
    if (g_nevents) deliver_events();
    if (g_joined) {
        const psp_net_transport *t = psp_net_get_transport();
        t->poll_packets(t->ctx, on_packet, NULL);
    }
    mmo_matching_pump();
}

void psp_net_mmo_register(void) {
    uint32_t nid; const char *lib, *name; int ok;
    for (int i = 0; psp_offline_stub(i, &nid, &lib, &name, &ok) && g_ntraced < MAX_TRACED; i++) {
        g_traced[g_ntraced].nid = nid; g_traced[g_ntraced].succeeds = ok; g_ntraced++;
        psp_hle_register(nid, lib, name, hle_traced);
    }
    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit", hle_AdhocctlInit);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_AdhocctlAddHandler);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_AdhocctlDelHandler);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState", hle_AdhocctlGetState);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_AdhocctlDisconnect);
    psp_hle_register(0x0AD043ED, "sceNetAdhocctl", "sceNetAdhocctlConnect", hle_AdhocctlConnect);
    psp_hle_register(0xEC0635C1, "sceNetAdhocctl", "sceNetAdhocctlCreate", hle_AdhocctlCreate);
    psp_hle_register(0x5E7F79C9, "sceNetAdhocctl", "sceNetAdhocctlJoin", hle_AdhocctlJoin);
    psp_hle_register(0xDED9D28E, "sceNetAdhocctl", "sceNetAdhocctlGetParameter", hle_AdhocctlGetParameter);
    psp_net_set_adhoc_dialog(adhoc_dialog);
    mmo_matching_register();
}
