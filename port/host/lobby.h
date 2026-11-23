/* The Multi Mode lobby (MMO builds): party warps between its rooms and
 * "weapons in the lobby". docs/area_research.md has how both were found.
 *
 *   - Warps: the game mode's "request move" (0x0893E424), called on the game
 *     thread from the area manager's per-frame update (0x089F9B00). Every
 *     client moves itself; the party leader's choice reaches the members
 *     through the server (psp_net_mmo_party_warp).
 *   - Weapons in the lobby: the lobby is a town, so it has no weapon palette
 *     and the session server does not tell the players about a weapon change.
 *     With the option on, the town checks (0x089F98AC / 0x089F98E4) answer
 *     "not a town" in the lobby (except for the scene loader, whose answer
 *     picks the rooms' map data), and the server's "no field object" test
 *     (0x0886C4DC) is passed at the three places that hold back an equipment
 *     change's announcement. */
#ifndef PSP2I_LOBBY_H
#define PSP2I_LOBBY_H

#include <stdint.h>

void lobby_init(void);

/* 1 while the current area is the Multi Mode lobby (area type 2, zone 2000). */
int  lobby_here(void);

/* Ask this client's game to move (destination = quest table * 1000 + entry;
 * the lobby is 2010000, area index 0, zone 2000). Runs on the next frame. */
void lobby_warp(int32_t dest, int16_t area_index, int16_t zone, int16_t entry);

/* Weapons in the lobby. The palette appears from the next lobby entry. */
void lobby_set_weapons(int on);
int  lobby_weapons(void);

#endif
