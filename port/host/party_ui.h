/* The party finder (MMO builds): a panel over the game that lists the parties
 * being hosted on the server (psp_net_mmo_parties) and takes the player into
 * one, or starts their own, by pressing the Matching Counter's buttons for
 * them.
 *
 *   - Opens by itself when the game enters the Matching Counter (Multi Mode)
 *     and the player is in no party; F2 (keyboard) or L3 (controller) opens
 *     and closes it any time. In the panel: D-pad or stick to move, Circle to
 *     choose, Cross to close (X and Z on the keyboard).
 *   - JOIN: the game's Join Party search is steered to the chosen leader
 *     (psp_net_mmo_set_join_target), then Join Party, the party, and the
 *     confirmation are pressed, each once the network shows the game is ready
 *     for it.
 *   - START A PARTY: Form Party is pressed; Begin Party Play stays the
 *     leader's choice.
 *   - Anything that does not happen in time hands control back, with a notice.
 *   - In the Multi Mode lobby, F2 / L3 opens the lobby menu instead: the party
 *     leader warps the whole party to one of the lobby's rooms (sent through
 *     the server: psp_net_mmo_party_warp), and switches "weapons in the lobby"
 *     on or off for the whole party (sent with the warps; a switch re-enters
 *     the room so it shows at once). Members see both, greyed.
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



#endif
