# Linux and Steam Deck

Linux uses the same SDL3 host as macOS and Android (`port/sdl_desktop.cmake`), with OpenGL 3.3 core. It runs on x86_64, which covers the Steam Deck, and on arm64.

## Flatpak (Steam Deck)

1. Switch the Deck to Desktop Mode.
2. Get the repository onto the Deck, with your decrypted `EBOOT.BIN` in the repository root.
3. Run the build script:

   ```sh
   port/linux/flatpak/build_flatpak.sh
   ```

   The script does three things:
   - Generates `build/gen` from `EBOOT.BIN` if it isn't already there.
   - Builds the Flatpak: SDL3 3.4.20, a minimal FFmpeg 9.0.2 for movie playback, and the game, on the `org.freedesktop.Platform` 25.08 runtime.
   - Installs the Flatpak for your user.

   Generating `build/gen` needs cmake, ninja and a C compiler. On the Deck those are easiest inside distrobox. You can also copy `build/gen` over from a machine that has already built the game, and the script will skip that step.

   To get a single `psp2i.flatpak` file you can copy to other machines, add `--bundle`.

4. Put the game files in `~/.var/app/io.github.zmbkilla.PSP2iRecomp/data/psp2i/`:

   ```
   EBOOT.BIN
   psp2i.ini                 iso=/run/media/<card>/psp2i.iso  (or a file in this folder)
   GameData/flash/font/      the PSP fonts (*.pgf)
   GameData/ms/PSP/SAVEDATA/ saves; copy PPSSPP's NPJH50332 folder here to continue
   client_config.toml        MMO builds only
   ```

   The ISO can stay on the SD card or anywhere in your home folder. The app can read `/run/media` and your home folder, but only writes to its own data folder.

5. Run it with `flatpak run io.github.zmbkilla.PSP2iRecomp`, or from the app menu.

   To play in Game Mode, add the app with Steam's "Add a Non-Steam Game". The Deck's controls reach the game as an SDL3 gamepad.

The MMO client is off in this build. To turn it on, set `-DENABLE_MMO_NET=ON` in the manifest's `config-opts`.

## Building without Flatpak

```sh
cmake -S port/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --target psp2i
PSP2I_DATA=$PWD build/linux/psp2i
```

This needs SDL3 and FFmpeg development packages. If you don't have FFmpeg, add `-DPSP2I_LIBAV=OFF`; the movies will then be black.
