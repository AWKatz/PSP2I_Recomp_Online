/* HD texture packs in PPSSPP's format (TEXTURES/<game id>/textures.ini + PNGs).
 *
 * Textures are matched exactly as PPSSPP does, so packs made for PPSSPP work:
 * the 64-bit key (address << 32 | log2 size, XOR the palette hash for CLUT
 * formats) plus the 32-bit hash of the texture data ([options] hash = quick,
 * xxh32 or xxh64; ignoreAddress; reduceHash; [hashranges]; [reducehashranges]),
 * with PPSSPP's wildcard fallbacks ([hashes] keys with zeroed parts). A
 * texture with no [hashes] entry may also be a file named by its hash
 * (<16 hex><8 hex>.png).
 *
 * PSP2I_TEXDUMP=1 writes every texture the pack doesn't replace to
 * <pack>/new/<hash>.png and lists it in <pack>/new/textures.ini, which is
 * how packs are made (and how this was tested). */
#ifndef PSP2I_TEXPACK_H
#define PSP2I_TEXPACK_H

#include <stdint.h>

/* Load <dir>/textures.ini. 0 when a pack is active. Logs what it found. */
int texpack_init(const char *dir);
int texpack_active(void);

/* For the texture the GE is drawing now (psp_gpu_texture_info), decoded as
 * `orig` (w x h RGBA8): the replacement's RGBA pixels and size, or NULL to
 * keep the original. The pointer stays valid until texpack_shutdown. */
const uint8_t *texpack_lookup(const uint32_t *orig, int *w, int *h);

void texpack_stats(unsigned *keys, unsigned *replaced, unsigned *dumped);
void texpack_shutdown(void);

#endif
