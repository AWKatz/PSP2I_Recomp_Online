/* The menu window's PARTY and CHARACTER tabs (MMO builds; its SETTINGS tab is
 * menu.c's, in the same look). F2 (keyboard) or L3 (controller) opens the
 * window on PARTY, and closes it; L / R move between the tabs, handing over to
 * SETTINGS (party_ui_take_tab) and back (party_ui_open_tab).
 *
 *   - PARTY: the party finder, the lobby's rooms and weapons in the lobby
 *     (below). Rows that don't apply where you are are greyed, and the chosen
 *     one says why.
 *   - CHARACTER: wear the look of one of the game's partner characters, or
 *     your own again (charswap.c). Left / Right turn a page.
 *
 * D-pad or stick to move (Up / Down repeat when held), Circle to choose,
 * Cross to close (X and Z on the keyboard).
 *
 * The party finder lists the parties being hosted on the server
 * (psp_net_mmo_parties) and takes the player into one, or starts their own,
 * by pressing the Matching Counter's buttons for them.
 *
 *   - Opens by itself when the game enters the Matching Counter (Multi Mode)
 *     and the player is in no party; from the PARTY tab otherwise.
 *   - JOIN: the game's Join Party search is steered to the chosen leader
 *     (psp_net_mmo_set_join_target), then Join Party, the party, and the
 *     confirmation are pressed, each once the network shows the game is ready
 *     for it.
 *   - START A PARTY: Form Party is pressed; Begin Party Play stays the
 *     leader's choice.
 *   - Anything that does not happen in time hands control back, with a notice.
 *
 * In the Multi Mode lobby, the party leader warps the whole party to one of
 * the lobby's rooms (sent through the server: psp_net_mmo_party_warp), and
 * switches "weapons in the lobby" on or off for the whole party (sent with
 * the warps; a switch re-enters the room so it shows at once). Members see
 * both, greyed.
 * While the panel is open, the game gets no input. */
#ifndef PSP2I_PARTY_UI_H
#define PSP2I_PARTY_UI_H

#include <stdint.h>

#include "menu.h"

void party_ui_init(int auto_open);

/* Once per vblank. held / latched: MI_* inputs (as menu_update takes them);
 * MI_TOGGLE = the F2 hotkey. Returns 1 while the panel wants the game's input
 * hidden. *inject: PSP buttons to press in the game this frame. */
int party_ui_update(uint32_t held, uint32_t latched, uint32_t *inject);

/* The panel and any notice, into a 480x272 image; the box drawn is returned
 * (0 when there is nothing to draw). */
int party_ui_draw(menu_image *img, int *x, int *y, int *w, int *h);

int party_ui_open(void);

/* The menu window's tabs (menu.h TAB_*): open PARTY or CHARACTER; close them (the
 * SETTINGS tab is opening); and the tab the user moved to that isn't this
 * file's (TAB_SETTINGS), once, else -1 -- the caller opens it (menu_open_settings). */
void party_ui_open_tab(int tab);
void party_ui_close(void);
int  party_ui_take_tab(void);

/* A tap at (x, y) in the 480x272 picture (touch screens): on a row, it is
 * selected, and chosen if it already was. Returns 1 if the tap was on the
 * panel (it is the panel's, not the game's), 0 otherwise. */
int party_ui_touch(int x, int y);



#endif
