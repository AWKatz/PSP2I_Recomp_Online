/* Stand-in for SEGA's infrastructure server (Internet Multi Mode); see infra.c. */
#ifndef PSP2I_INFRA_H
#define PSP2I_INFRA_H

/* Installs the sceHttp transport that answers the game's infra packets. */
void infra_init(const char *base, const char *player_name);   /* base: the data folder (terms_of_use.txt, notices.txt: shown when the server sends none) */

#endif
