/* The Multi Mode lobby: see lobby.h. The addresses are the game's (EBOOT
 * 0x08804000+); each hook is listed in port/hooks.txt. */
#include "lobby.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/net.h>
#include <psprecomp/net_proto.h>

#include <stdio.h>
#include <stdlib.h>

#define AREA_MANAGER   0x08ED4DD0u   /* *: the area manager; current area (type u32, zone s16) at +40 / +44 */
#define GAME_MODE      0x08ED2958u   /* *: the game mode object */
#define FN_REQUEST_MOVE 0x0893E424u  /* game mode vfunc +0xE0: (mode, destination, area index, zone, entry) */
#define FN_AREA_UPDATE 0x089F9B00u   /* area manager, per frame (runs requests at +68) */
#define FN_IS_TOWN     0x089F98ACu   /* area record: town? (category 2) */
#define FN_IS_TOWN2    0x089F98E4u   /* area record: special town? (category 2 + zones) */
#define FN_NO_FIELD    0x0886C4DCu   /* session server: obj+36 == 0 ("no field object") */
/* Internet Multi Mode: the infra client (*0x08ED59E8, NULL elsewhere) and its "send my
 * character data" (the lobby packet, room message category 4, built from the character data
 * as it is now): 0x08A68454(member) answers a request from that member; member 0 sends it to
 * the whole room. The game sends it on entering a room and in answer to requests, which a
 * room's owner never answers. */
#define INFRA_CLIENT        0x08ED59E8u
#define FN_SEND_LOBBY_DATA  0x08A68454u

/* The scene loader's town check: its answer picks the rooms' map data (the
 * Bridge loads black when it is told "not a town"). */
#define RA_SCENE_LOADER 0x08A00408u
/* Where the session server holds back an equipment change's announcement in
 * a lobby: the handler (0x0882A288), the 0x0A01 notice to the others (room
 * vfunc +88) and the 56-byte equip message to every player (room vfunc +140). */
static const uint32_t RA_EQUIP_NOTICE[] = { 0x0882A4C0u, 0x08D1EF68u, 0x08D1F29Cu };

#define LOBBY_DEST 2010000
#define LOBBY_TYPE 2
#define LOBBY_ZONE 2000

static struct { volatile int pending; int32_t dest; int16_t idx, zone, entry; } g_warp;
static volatile int g_weapons;
/* The move the game last requested (game mode +536..+548 while a move is under way;
 * the game sets them back to -1 when it is done): where this client is now. */
static struct { int have; int32_t dest; int16_t idx, zone, entry; } g_last_move;
/* After lobby_reenter: once the area has gone and come back, and the player exists (a few
 * seconds), the room hears this client's character data again, with its new look. */
enum { ANN_NONE, ANN_WAIT_UNLOAD, ANN_WAIT_LOAD, ANN_SETTLE };
static struct { int state, frames; } g_announce;

static void announce_step(void) {
    if (g_announce.state == ANN_NONE) return;
    const uint32_t m = psp_read32(AREA_MANAGER);
    const uint32_t type = m ? psp_read32(m + 40) : 0;
    g_announce.frames++;
    switch (g_announce.state) {
    case ANN_WAIT_UNLOAD:                                  /* the area going (type 0 between areas) */
        if (type == 0) { g_announce.state = ANN_WAIT_LOAD; g_announce.frames = 0; }
        else if (g_announce.frames > 600) { g_announce.state = ANN_WAIT_LOAD; g_announce.frames = 0; }
        break;
    case ANN_WAIT_LOAD:
        if (type == LOBBY_TYPE) { g_announce.state = ANN_SETTLE; g_announce.frames = 0; }
        else if (g_announce.frames > 60 * 60) g_announce.state = ANN_NONE;   /* no lobby within a minute: give up */
        break;
    case ANN_SETTLE:                                       /* the player and the room's players built */
        if (g_announce.frames < 180) break;
        g_announce.state = ANN_NONE;
        if (type == LOBBY_TYPE && psp_read32(INFRA_CLIENT)) {
            /* The sender (0x08A68548, run by the infra client's update) only sends while the
             * time at +624 (u64, network ticks) is still ahead -- the window an answer to a
             * request opens -- or when it is 0. A window from an earlier answer has long
             * closed, so it is cleared: the next update builds and sends the data. */
            const uint32_t ic = psp_read32(INFRA_CLIENT);
            psp_write32(ic + 624, 0);
            psp_write32(ic + 628, 0);
            psp_net_log("lobby: sending this player's character data to the room again (its new look)");
            psp_call_guest(FN_SEND_LOBBY_DATA, 0, 0, 0);
        }
        break;
    }
}
static int g_entry = 4;              /* the room this client is in (the plaza, where Multi Mode starts) */

int lobby_here(void) {
    const uint32_t m = psp_read32(AREA_MANAGER);
    return m && psp_read32(m + 40) == LOBBY_TYPE && (int16_t)psp_read16(m + 44) == LOBBY_ZONE;
}

void lobby_warp(int32_t dest, int16_t area_index, int16_t zone, int16_t entry) {
    g_warp.dest = dest; g_warp.idx = area_index; g_warp.zone = zone; g_warp.entry = entry;
    g_warp.pending = 1;
}

int  lobby_entry(void) { return g_entry; }

int lobby_reenter(void) {
    const uint32_t m = psp_read32(AREA_MANAGER);
    if (!m || !g_last_move.have) return -1;
    const uint32_t type = psp_read32(m + 40);
    const int16_t zone = (int16_t)psp_read16(m + 44);
    if (type != LOBBY_TYPE || zone != g_last_move.zone) return -1;
    psp_net_log("lobby: entering the area again (destination %d, zone %d, entry %d)", g_last_move.dest, zone, g_last_move.entry);
    lobby_warp(g_last_move.dest, g_last_move.idx, g_last_move.zone, g_last_move.entry);
    g_announce.state = ANN_WAIT_UNLOAD;                    /* then tell the room (Internet Multi Mode) */
    g_announce.frames = 0;
    return 0;
}
void lobby_set_weapons(int on) { g_weapons = on ? 1 : 0; }
int  lobby_weapons(void) { return g_weapons; }

static void hook_area_update(void (*original)(void)) {
    announce_step();
    psp_net_mmo_warp w;
    if (psp_net_mmo_take_warp(&w)) {                       /* the leader's choice, with the party's lobby options */
        if (w.flags & PSP_MMO_WARP_OPTIONS) g_weapons = (w.flags & PSP_MMO_WARP_WEAPONS) != 0;
        lobby_warp(w.dest, w.area_index, w.zone, w.entry);
    }
    {   /* the room a move into the lobby is going to (the game mode's destination +536 / entry +548) */
        const uint32_t mode = psp_read32(GAME_MODE);
        if (mode && (int32_t)psp_read32(mode + 536) == LOBBY_DEST) {
            const int32_t e = (int32_t)psp_read32(mode + 548);
            if (e >= 0 && e <= 17) g_entry = e;
        }
        if (mode && (int32_t)psp_read32(mode + 536) != -1) {   /* a move under way: remember it */
            g_last_move.have = 1;
            g_last_move.dest = (int32_t)psp_read32(mode + 536);
            g_last_move.idx = (int16_t)psp_read32(mode + 540);
            g_last_move.zone = (int16_t)psp_read32(mode + 544);
            g_last_move.entry = (int16_t)psp_read32(mode + 548);
        }
    }
    if (g_warp.pending) {
        g_warp.pending = 0;
        if (g_warp.dest == LOBBY_DEST) g_entry = g_warp.entry;
        const uint32_t mode = psp_read32(GAME_MODE);
        if (mode) {
            psp_net_log("lobby: warp to destination %d, area index %d, zone %d, entry %d", g_warp.dest, g_warp.idx, g_warp.zone, g_warp.entry);
            psp_call_guest5(FN_REQUEST_MOVE, mode, (uint32_t)g_warp.dest, (uint32_t)(int32_t)g_warp.idx, (uint32_t)(int32_t)g_warp.zone,
                            (uint32_t)(int32_t)g_warp.entry);
        }
    }
    original();
}

static void hook_is_town(void (*original)(void)) {
    const uint32_t ra = psp_cpu.r[31];
    original();
    if (g_weapons && psp_cpu.r[2] && ra != RA_SCENE_LOADER && lobby_here()) psp_cpu.r[2] = 0;
}

static void hook_no_field(void (*original)(void)) {
    const uint32_t ra = psp_cpu.r[31];
    original();
    if (!g_weapons || !psp_cpu.r[2]) return;
    for (size_t i = 0; i < sizeof RA_EQUIP_NOTICE / sizeof RA_EQUIP_NOTICE[0]; i++)
        if (ra == RA_EQUIP_NOTICE[i] && lobby_here()) { psp_cpu.r[2] = 0; return; }
}

/* Research (PSP2I_QUEST_TRACE=1): quest tables asked for by id, and the
 * client's quest-table requests. */
static void hook_quest_table(void (*original)(void)) {
    const int32_t id = (int32_t)psp_cpu.r[5];
    const uint32_t ra = psp_cpu.r[31];
    original();
    psp_net_log("lobby: quest table %d -> %s (caller 0x%08X)", id, psp_cpu.r[2] ? "loaded" : "not loaded", ra);
}
static void hook_table_request(void (*original)(void)) {
    psp_net_log("lobby: quest table request: table %d, slot %d, mode %d (caller 0x%08X)", (int32_t)psp_cpu.r[6], (int32_t)psp_cpu.r[7],
                (int32_t)psp_cpu.r[8], psp_cpu.r[31]);
    original();
}

void lobby_init(void) {
    if (getenv("PSP2I_QUEST_TRACE")) {
        psp_hook_set(0x08D25714u, hook_quest_table);
        psp_hook_set(0x0888B62Cu, hook_table_request);
    }
    psp_hook_set(FN_AREA_UPDATE, hook_area_update);
    psp_hook_set(FN_IS_TOWN, hook_is_town);
    psp_hook_set(FN_IS_TOWN2, hook_is_town);
    psp_hook_set(FN_NO_FIELD, hook_no_field);
}
