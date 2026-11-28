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

/* The lobby room this client is in (its entry point: 0 Bridge, 1 Deck,
 * 2 Cafe, 3 Shop Plaza, 4 Central Plaza, 5 Counter Room, 9 Battle Shop,
 * 11 Style Shop, 15 the Style Shop's Salon, 17 Little Wing; the others are
 * spots in the plaza). */
int  lobby_entry(void);

/* Enter the area this client is in again: the move the game last made (its
 * destination, area index, zone and entry, as requested), made once more, so
 * the area loads and the player is built again from the character data (a
 * character look from the F2 menu then shows). Only in a lobby or other
 * area of type 2 (towns: a mission field would start over), and only when
 * that move led to the zone this client is in. 0 when asked, -1 when not. */
int  lobby_reenter(void);

/* Weapons in the lobby. A change shows fully after re-entering the room
 * (a warp): on, the palette appears; off, drawn weapons are put away. */
void lobby_set_weapons(int on);
int  lobby_weapons(void);

#endif
