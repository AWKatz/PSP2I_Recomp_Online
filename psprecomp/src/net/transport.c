/* psprecomp — the multiplayer transport registry and the null transport.
 * Built only with ENABLE_MMO_NET; see include/psprecomp/net_transport.h. */

#include "psprecomp/net_transport.h"

#include <stddef.h>

static int  null_initialize(void *c, const uint8_t self_mac[6]) { (void)c; (void)self_mac; return PSP_NET_ERR_UNSUPPORTED; }
static int  null_host(void *c, const char *group, int max_players) { (void)c; (void)group; (void)max_players; return PSP_NET_ERR_UNSUPPORTED; }
static int  null_join(void *c, const char *address, const char *group) { (void)c; (void)address; (void)group; return PSP_NET_ERR_UNSUPPORTED; }
static int  null_send(void *c, const psp_net_peer *to, int channel, uint16_t port, const void *data, uint32_t len) {
    (void)c; (void)to; (void)channel; (void)port; (void)data; (void)len;
    return PSP_NET_ERR_UNSUPPORTED;
}
static int  null_poll(void *c, psp_net_packet_fn fn, void *user) { (void)c; (void)fn; (void)user; return 0; }
static void null_shutdown(void *c)                             { (void)c; }

static const psp_net_transport NULL_TRANSPORT = {
    "null", NULL, null_initialize, null_host, null_join, null_send, null_poll, null_shutdown
};

static const psp_net_transport *g_transport = &NULL_TRANSPORT;

const psp_net_transport *psp_net_null_transport(void) { return &NULL_TRANSPORT; }
const psp_net_transport *psp_net_get_transport(void)  { return g_transport; }

void psp_net_set_transport(const psp_net_transport *t) {
    if (g_transport && g_transport != t) g_transport->shutdown(g_transport->ctx);
    g_transport = t ? t : &NULL_TRANSPORT;
}
