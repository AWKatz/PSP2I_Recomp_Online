/* The Multi Mode lobby: see lobby.h. The addresses are the game's (EBOOT
 * 0x08804000+); each hook is listed in port/hooks.txt. */
#include "lobby.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/net.h>

#include <stdio.h>

#define AREA_MANAGER   0x08ED4DD0u   /* *: the area manager; current area (type u32, zone s16) at +40 / +44 */
#define GAME_MODE      0x08ED2958u   /* *: the game mode object */
#define FN_REQUEST_MOVE 0x0893E424u  /* game mode vfunc +0xE0: (mode, destination, area index, zone, entry) */
#define FN_AREA_UPDATE 0x089F9B00u   /* area manager, per frame (runs requests at +68) */
#define FN_IS_TOWN     0x089F98ACu   /* area record: town? (category 2) */
#define FN_IS_TOWN2    0x089F98E4u   /* area record: special town? (category 2 + zones) */
#define FN_NO_FIELD    0x0886C4DCu   /* session server: obj+36 == 0 ("no field object") */

/* The scene loader's town check: its answer picks the rooms' map data (the
 * Bridge loads black when it is told "not a town"). */
#define RA_SCENE_LOADER 0x08A00408u
/* Where the session server holds back an equipment change's announcement in
 * a lobby: the handler (0x0882A288), the 0x0A01 notice to the others (room
 * vfunc +88) and the 56-byte equip message to every player (room vfunc +140). */
static const uint32_t RA_EQUIP_NOTICE[] = { 0x0882A4C0u, 0x08D1EF68u, 0x08D1F29Cu };

#define LOBBY_TYPE 2
#define LOBBY_ZONE 2000

static struct { volatile int pending; int32_t dest; int16_t idx, zone, entry; } g_warp;
static volatile int g_weapons;

int lobby_here(void) {
    const uint32_t m = psp_read32(AREA_MANAGER);
    return m && psp_read32(m + 40) == LOBBY_TYPE && (int16_t)psp_read16(m + 44) == LOBBY_ZONE;
}

void lobby_warp(int32_t dest, int16_t area_index, int16_t zone, int16_t entry) {
    g_warp.dest = dest; g_warp.idx = area_index; g_warp.zone = zone; g_warp.entry = entry;
    g_warp.pending = 1;
}

void lobby_set_weapons(int on) { g_weapons = on ? 1 : 0; }
int  lobby_weapons(void) { return g_weapons; }

static void hook_area_update(void (*original)(void)) {
    psp_net_mmo_warp w;
    if (psp_net_mmo_take_warp(&w)) lobby_warp(w.dest, w.area_index, w.zone, w.entry);   /* the leader's choice */
    if (g_warp.pending) {
        g_warp.pending = 0;
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

void lobby_init(void) {
    psp_hook_set(FN_AREA_UPDATE, hook_area_update);
    psp_hook_set(FN_IS_TOWN, hook_is_town);
    psp_hook_set(FN_IS_TOWN2, hook_is_town);
    psp_hook_set(FN_NO_FIELD, hook_no_field);
}
