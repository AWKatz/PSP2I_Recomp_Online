# libkirk (vendored)

This is the software model of the PSP's KIRK crypto engine (AES, SHA-1, ECDSA). It comes from PPSSPP's `ext/libkirk`:

- Source: https://github.com/hrydgard/ppsspp
- Commit: 5ce7f1cfeae1bfcb1a5e88fef74ce127c696cdfc

The files are copied unchanged. `amctrl.c` (PGD/DRM) is left out because nothing here uses it.

License: GNU GPL v2 or v3 (see the headers of `bn.c`, `ec.c` and `kirk_engine.c`). This code is used only by the save-data crypto provider, `port/host/savecrypt.c`.
