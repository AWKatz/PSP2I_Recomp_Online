/* Character swap: wear the look of one of the game's partner characters (the
 * 27 story characters and 60 preset partners of the AI partner system).
 *
 * Every partner has a record in the game's partner data (aNN.unr / pNN.unr,
 * loaded as 0xC0-byte "NUR" blocks one after another). From +0x10 a record is
 * laid out as your own character data ([0x08ED5010] + 0x420): short name at
 * +0x00, full name at +0x40, the appearance at +0x80 (a 4-byte header: race,
 * sex, class (0 Hunter, 1 Ranger, 2 Force, 3 Vanguard), one more byte; a word;
 * six part slots). A swap copies race, sex and the six part slots into your
 * character data; your class (and so your weapons), colours, proportions and
 * the rest stay yours. The game shows the new look the next time it
 * builds your character, when an area loads (an armour change does not); the
 * F2 menu enters a lobby again at once (lobby_reenter).
 *
 * Your own appearance is kept in <base>/character_backup.txt before the first
 * swap, so CHARACTER > MY OWN CHARACTER puts it back even after the game has
 * saved a swapped look and been restarted. */
#ifndef PSP2I_CHARSWAP_H
#define PSP2I_CHARSWAP_H

#include <stddef.h>
#include <stdint.h>

#define CHARSWAP_MAX 128

void charswap_init(const char *base_dir);

/* The partner records, found in memory (0 when the game has not loaded them,
 * or has no character loaded: the title screen). */
int  charswap_count(void);
/* Record i's name for a menu: "Emilia Percival", "GH410" (ASCII). */
const char *charswap_name(int i);
/* Record i's race and sex: "Human F", "CAST M". */
const char *charswap_kind(int i);
/* The record whose look you wear now, -1 for your own (or unknown). */
int  charswap_current(void);
/* Whether a backup of your own look exists to go back to. */
int  charswap_have_backup(void);

/* Wear record i's look, or your own again. 0 on success; msg gets a line for
 * the player either way. */
int  charswap_apply(int i, char *msg, size_t cap);
int  charswap_restore(char *msg, size_t cap);

/* For tests: where the game keeps the pointer to the save block, and the
 * offsets (the real values by default). */
#define CHARSWAP_SAVE_PTR   0x08ED5010u
#define CHARSWAP_CHARDATA   0x420u
#define CHARSWAP_APPEARANCE 0x80u

#endif
