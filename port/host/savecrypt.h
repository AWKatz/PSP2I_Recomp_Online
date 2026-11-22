/* PSP savedata encryption (see savecrypt.c). */
#ifndef PSP2I_SAVECRYPT_H
#define PSP2I_SAVECRYPT_H

#include <psprecomp/hle.h>

/* The provider, for tests. */
const psp_savedata_crypto *savecrypt_provider(void);
/* 0: behave as older PPSSPP, without the fuse commands (tests). */
void savecrypt_set_fuse(int on);
/* Hand it to the runtime: saves are read and written encrypted, as on a PSP. */
void savecrypt_install(void);

#endif
