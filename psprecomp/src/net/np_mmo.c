/* psprecomp — PSN sign-in without PSN (MMO build, Internet Multi Mode).
 *
 * The game signs in to PSN before it talks to its own game server. Here the
 * player is always signed in, under the name from client_config.toml:
 *   - sceNpGetNpId: the online id is the player name;
 *   - content rating and chat restriction: none;
 *   - sceNpAuth: a ticket request succeeds, its callback is called on the
 *     next frame with the ticket's length, and sceNpAuthGetTicket copies a
 *     ticket out. Nothing checks the ticket but our own server, so it only
 *     carries the online id.
 * Layouts follow PPSSPP's sceNp.h (SceNpId, SceNpAuthRequestParameter). */

#include "adhoc_internal.h"

#include "psprecomp/hle.h"
#include "psprecomp/net.h"

#include <stdio.h>
#include <string.h>

#define SCE_NP_AUTH_ERROR_REQUEST_NOT_FOUND 0x80550511u

#define NP_ID_LEN     36   /* SceNpId: handle {data[16], term, dummy[3]}, opt[8], reserved[8] */
#define TICKET_LEN    64   /* "PSP2I_TICKET", online id */

static char g_online_id[17];
static struct { int used, delivered; uint32_t id, cb, arg; } g_req[4];
static uint32_t g_next_req = 1;

void mmo_np_set_name(const char *name) {
    memset(g_online_id, 0, sizeof g_online_id);
    /* PSN online ids: letters, digits, '-' and '_' (3-16 characters) */
    size_t n = 0;
    for (const char *p = name; *p && n < 16; p++) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            g_online_id[n++] = c;
        else if (c == ' ') g_online_id[n++] = '_';
    }
    while (n < 3) g_online_id[n++] = '_';
}

static void write_np_id(uint32_t addr) {
    uint8_t id[NP_ID_LEN] = { 0 };
    memcpy(id, g_online_id, strlen(g_online_id));
    psp_mem_write_block(addr, id, sizeof id);
}

static void hle_NpGetNpId(void) {
    if (psp_arg(0)) write_np_id(psp_arg(0));
    psp_net_log("np: sceNpGetNpId -> '%s'", g_online_id);
    psp_ret(0);
}
static void hle_NpGetContentRatingFlag(void) {          /* (int *is_restricted, int *age) */
    if (psp_arg(0)) psp_write32(psp_arg(0), 0);
    if (psp_arg(1)) psp_write32(psp_arg(1), 20);
    psp_ret(0);
}
static void hle_NpGetChatRestrictionFlag(void) {        /* (int *is_restricted) */
    if (psp_arg(0)) psp_write32(psp_arg(0), 0);
    psp_ret(0);
}

/* sceNpAuthCreateStartRequest(const SceNpAuthRequestParameter *): ticket callback +28, its argument +32. */
static void hle_NpAuthCreateStartRequest(void) {
    const uint32_t p = psp_arg(0);
    for (int i = 0; i < 4; i++)
        if (!g_req[i].used) {
            g_req[i].used = 1; g_req[i].delivered = 0; g_req[i].id = g_next_req++;
            g_req[i].cb = p ? psp_read32(p + 28) : 0; g_req[i].arg = p ? psp_read32(p + 32) : 0;
            psp_net_log("np: ticket request %u (callback 0x%08X)", g_req[i].id, g_req[i].cb);
            psp_ret(g_req[i].id);
            return;
        }
    psp_ret(0x80550503u);                                 /* too many requests */
}

static int find_req(uint32_t id) {
    for (int i = 0; i < 4; i++) if (g_req[i].used && g_req[i].id == id) return i;
    return -1;
}

/* sceNpAuthGetTicket(id, buffer, length) -> bytes copied */
static void hle_NpAuthGetTicket(void) {
    if (find_req(psp_arg(0)) < 0) { psp_ret(SCE_NP_AUTH_ERROR_REQUEST_NOT_FOUND); return; }
    uint8_t t[TICKET_LEN] = { 0 };
    memcpy(t, "PSP2I_TICKET", 12);
    memcpy(t + 16, g_online_id, strlen(g_online_id));
    uint32_t n = psp_arg(2) < TICKET_LEN ? psp_arg(2) : TICKET_LEN;
    if (psp_arg(1) && n) psp_mem_write_block(psp_arg(1), t, n);
    psp_net_log("np: ticket %u copied (%u bytes)", psp_arg(0), n);
    psp_ret(n);
}

static void hle_NpAuthAbortOrDestroy(void) {
    const int i = find_req(psp_arg(0));
    if (i >= 0) g_req[i].used = 0;
    psp_ret(0);
}

void mmo_np_pump(void) {
    for (int i = 0; i < 4; i++)
        if (g_req[i].used && !g_req[i].delivered) {
            g_req[i].delivered = 1;
            if (g_req[i].cb) {
                psp_net_log("np: ticket %u ready -> callback 0x%08X", g_req[i].id, g_req[i].cb);
                psp_call_guest(g_req[i].cb, g_req[i].id, TICKET_LEN, g_req[i].arg);
            }
        }
}

void mmo_np_register(void) {
    psp_hle_register(0x633B5F71, "sceNp", NULL, hle_NpGetNpId);                 /* sceNpGetNpId */
    psp_hle_register(0xBB069A87, "sceNp", NULL, hle_NpGetContentRatingFlag);    /* sceNpGetContentRatingFlag */
    psp_hle_register(0x1D60AE4B, "sceNp", NULL, hle_NpGetChatRestrictionFlag);  /* sceNpGetChatRestrictionFlag */
    psp_hle_register(0xCD86A656, "sceNpAuth", NULL, hle_NpAuthCreateStartRequest);
    psp_hle_register(0x3F1C1F70, "sceNpAuth", NULL, hle_NpAuthGetTicket);
    psp_hle_register(0xD99455DD, "sceNpAuth", NULL, hle_NpAuthAbortOrDestroy);  /* sceNpAuthAbortRequest */
    psp_hle_register(0x72BB0467, "sceNpAuth", NULL, hle_NpAuthAbortOrDestroy);  /* sceNpAuthDestroyRequest */
}
