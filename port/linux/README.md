# Linux and Steam Deck

Linux uses the same SDL3 host as macOS and Android (`port/sdl_desktop.cmake`), with OpenGL 3.3 core. It runs on x86_64, which covers the Steam Deck, and on arm64.

## AppImage

An AppImage is a single file that runs on most distributions with nothing to install.

1. Get the repository onto a Linux machine, with your decrypted `EBOOT.BIN` in the repository root.
2. Install the build tools: cmake, ninja, a C/C++ compiler, nasm, curl, and the development headers SDL3 needs (X11, Wayland, ALSA, PulseAudio). On Debian or Ubuntu:

   ```sh
   sudo apt install cmake ninja-build build-essential nasm curl libx11-dev libxext-dev libxrandr-dev \
       libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libegl-dev libgl-dev \
       libasound2-dev libpulse-dev libudev-dev
   ```

   On the Steam Deck, build inside distrobox, or on another Linux machine and copy the AppImage over.
3. Run the build script:

   ```sh
   port/linux/appimage/build_appimage.sh             # the game with online play, and the server:
                                                     #   build/PSP2i_Linux/PSP2i-x86_64.AppImage
                                                     #   build/PSP2i_Linux/server/psp2i_server-x86_64.AppImage
   port/linux/appimage/build_appimage.sh --offline   # the game without online play:
                                                     #   build/PSP2i_Linux/PSP2i-offline-x86_64.AppImage
   ```

   The script generates `build/gen` from `EBOOT.BIN` if needed. It builds SDL3 3.4.20 and a minimal FFmpeg 9.0.2 (movie playback only) from source, then the game, and packs each program with `linuxdeploy`.

   An AppImage runs on distributions at least as new as the one it was built on. To reach more machines, build on an older release such as Ubuntu 22.04.

4. Put the game files in the root of `build/PSP2i_Linux/` (the folder that holds the AppImage; the build copies `EBOOT.BIN` in). You can move that whole folder anywhere. Or put them in `~/.local/share/psp2i/`, which is used when there's no `EBOOT.BIN` next to the AppImage. `PSP2I_DATA` overrides both.

   ```
   your .iso                 the first .iso in the folder is used (or psp2i.ini iso=/run/media/<card>/psp2i.iso)
   EBOOT.BIN
   font/                     the PSP fonts (*.pgf)
   NPJH50332/                saves; copy PPSSPP's NPJH50332 folder here to continue
   client_config.toml        which server to join (found on the LAN by default)
   ```

5. Run it: `./PSP2i-x86_64.AppImage`, or double-click it. On the Steam Deck, add it with Steam's "Add a Non-Steam Game" to play in Game Mode; the Deck's controls reach the game as an SDL3 gamepad.

### The server AppImage

`server/psp2i_server-x86_64.AppImage` runs the dedicated server, for example `cd server && ./psp2i_server-x86_64.AppImage --lan`. It keeps its database and `config/` (loot tables and rates; the defaults are copied in on first run) in its own folder, `build/PSP2i_Linux/server/`, or in `PSP2I_SERVER_DATA`. All its options are in [server/README.md](../../server/README.md).

## Building without an AppImage

```sh
cmake -S port/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --target psp2i
PSP2I_DATA=$PWD build/linux/psp2i
```

This needs SDL3 and FFmpeg development packages. If you don't have FFmpeg, add `-DPSP2I_LIBAV=OFF`; the movies will then be black. Online play is on by default; add `-DBUILD_DEDICATED_SERVER=ON` for the server too, or `-DENABLE_MMO_NET=OFF` for a game without online play.
