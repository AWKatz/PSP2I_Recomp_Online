/* PSP2i MMO client start-up (ENABLE_MMO_NET builds only). */
#ifndef PSP2I_MMO_CLIENT_H
#define PSP2I_MMO_CLIENT_H

/* Read <base>/client_config.toml, install the ENet transport and the network
 * overrides (WLAN switch on, PSN bypassed). Call after psp_hle_init. */
void mmo_client_init(const char *base);
/* Once per frame. */
void mmo_client_frame(void);
void mmo_client_shutdown(void);

#endif
