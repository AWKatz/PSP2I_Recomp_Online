<h1 align="center"> Phantasy Star Portable 2 Infinity Recomp</h1>

<p align="center">
   <img src="asset/Logo.png" alt="Icon">
</p>

A recompilation project for **Phantasy Star Portable 2 Infinity (PSP2i)**, bringing the game to a native PC environment while preserving the original game's functionality and feel.

## Quick Start

You need your own copy of the game: a decrypted `EBOOT.BIN` (needed to build) and the disc image (`.iso`). Everything that gets built or generated goes into `build/`.

### Build and play

| Platform | Build | Run | Your game files go in |
| --- | --- | --- | --- |
| macOS (Apple Silicon) | `brew install cmake ninja sdl3 ffmpeg`<br>`./build_macos.sh` | `./Launch-PSP2i.command` | the repository root |
| Linux / Steam Deck | `port/linux/flatpak/build_flatpak.sh` | `flatpak run io.github.zmbkilla.PSP2iRecomp` | `~/.var/app/io.github.zmbkilla.PSP2iRecomp/data/psp2i/` |
| Windows | `build_port.bat regen` (needs Visual Studio and CMake) | `build\windows\Release\psp2i.exe` | the folder with `psp2i.exe` |
| Windows release copy | `build_finalbuild.bat` (or `build_prod.bat` for the community build) | `build\dist\psp2i.exe` (`build\prod\psp2i.exe`) | the folder with `psp2i.exe` |
| Android | `build_android.bat` (see [port/android](port/android/README.md)) | install `build\dist\psp2i-android.apk` | the folder the app asks for on first start |

To build, put `EBOOT.BIN` in the repository root. The build scripts generate `build/gen` from it.

To play on macOS or Linux, put these in the folder from the last column. Windows has its own layout, with the extracted disc under `GameData\disc\`; see [port/prod_README.txt](port/prod_README.txt). Android: see [port/android](port/android/README.md).

```
EBOOT.BIN                             the decrypted game executable
psp2i.ini                             iso=game.iso  (path to your disc image; also fps=30|60)
GameData/flash/font/*.pgf             PSP firmware fonts (without them, menu text is missing)
GameData/ms/PSP/SAVEDATA/NPJH50332/   your save: copy PPSSPP's memstick/PSP/SAVEDATA/NPJH50332 folder as is
client_config.toml                    MMO builds only (see below)
```

PPSSPP and PSP saves load and save encrypted, so they work in both directions. In-game, Cross confirms.

### MMO client and dedicated server

```sh
./build_macos.sh --mmo        # build/macos-mmo/psp2i and build/macos-mmo/server/psp2i_server
cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/server    # the server alone, any OS: build/server/psp2i_server
cd server && ../build/server/psp2i_server --lan    # run it where config/ is, or pass --drop-tables/--rates
```

`psp2i_server` options:

| Option | Default | |
| --- | --- | --- |
| `--lan` / `--online` | `--lan` | LAN: answers discovery probes. Online: strict rate limits, nothing accepted before the handshake |
| `--port N` | 29001 | game port (UDP) |
| `--bind ADDR` | all interfaces | address to listen on |
| `--lan-port N` | 29000 | LAN discovery port (UDP) |
| `--name NAME` | PSP2i Server | name shown to LAN clients |
| `--max-players N` | 32 | 1–63 |
| `--rate-limit N` | 120 | online: packets per second per player (bursts up to 2x; kicked after 3 s over) |
| `--handshake-timeout S` | 5 | seconds a new connection has to send its handshake |
| `--kill-limit N` | 10 | enemy-kill reports per second per player |
| `--db PATH` | psp2i_server.db | the SQLite database (accounts, characters, drops, audit log) |
| `--drop-tables PATH` | config/drop_tables.json | loot tables |
| `--rates PATH` | config/rates.toml | rate multipliers |
| `--stats S` | 60 | print a status line every S seconds (0 = off) |
| `--run-seconds S` | off | exit after S seconds |
| `--help` | | |

Server commands:
- Ctrl-C or `SIGTERM` shuts down and tells every client.
- `SIGHUP` reloads the drop tables and rates at once. Editing either file also takes effect within a second, without disconnecting anyone.
- An interactive console, RCON and a web admin panel will come with Task 2.4.

Config files:

- **`server/config/rates.toml`:** `exp_rate`, `drop_rate`, `rare_drop_rate`, `meseta_rate`, `type_points_rate` (each 0–100, default 1.0).
- **`server/config/drop_tables.json`:**
  - `enemies[]`: `id`, `name`, `exp`, `type_points`, `drop_chance` (0–1), `meseta` `[min, max]`, `items[]`, `rares[]`.
    - each `items[]` entry is either `{id, name, weight}` or `{meseta: true, weight}`;
    - each `rares[]` entry is `{id, name, chance}`.
  - `missions[]`: `id`, `name`, `meseta`, `rewards[] {id, name, weight}`.
  - The shipped values are placeholders.
- **`client_config.toml`** (with your game files, MMO builds):
  - `server_ip`: leave it out to find a LAN server;
  - `server_port` (29001);
  - `lan_discovery` (true);
  - `discovery_port` (29000);
  - `player_name`.

More detail: [server/README.md](server/README.md), [port/linux/README.md](port/linux/README.md), [port/macos/README.md](port/macos/README.md).

## AI Disclosure

This project makes **heavy use of AI-assisted development, primarily Claude**, for code analysis, reverse engineering assistance, debugging, implementation, and development tooling.

AI-generated code and suggestions are reviewed, tested, and integrated as part of the development process.

## About

**PSP2I_Recomp** is a recompilation project for *Phantasy Star Portable 2 Infinity*.

The current goal is to produce a native PC build of the game while maintaining compatibility with the original game logic and improving the experience with modern PC features.

This project is based on the output of the PSP recompilation toolchain and ongoing community reverse-engineering efforts.

## Capabilities

Current capabilities include:

* Native Windows, Android, Mac Os executable
* Direct3D 11 rendering
* SDL3 input support
* Keyboard and controller input
* Right-stick camera control
* Save/load functionality
* Optional 60 FPS mode
* Original 30 FPS mode
* Infrastructure handling (Head to [psp2i discord](https://discord.gg/FeGvdBkrGa) for more info)
* Official website loading

> This project is actively being developed. Capabilities and compatibility may change as development progresses.


## Issues

Current issues include:

- 60 fps is not properly implemented
- Adhoc not supported properly (relay has best support)

## Building

### Requirements

- Windows
- Visual Studio with C++ development tools
- CMake
- Git
- FFMPEG 9.0.1
- SDL3 3.4.18
- A legally obtained copy of *Phantasy Star Portable 2 Infinity*

### Build

Use the commands under [Quick Start](#quick-start). On Windows, `build_port.bat regen` does all of this:

1. Builds the recompiler into `build\recomp`.
2. Regenerates `build\gen` from `EBOOT.BIN` (taken from the repository root, or from an older `FinalBuild\` folder).
3. Builds `build\windows\Release\psp2i.exe`.

Without `regen`, it only recompiles. Put the SDL3 and FFmpeg DLLs (`avcodec-63`, `avutil-61`, `swresample-7`) next to the exe.

### Optional Checks

The following commands can be used to verify that the expected PSP2i hooks are present:

```bash
grep -c "0x08D3F3B4\|0x08D3F418\|0x08D4C5C0\|0x08D3F470\|0x08D4C604\|0x08B8D840" port/hooks.txt
grep -c "hook_hang_fatal\|hook_game_report" port/host/gamelog.c
```

These commands only verify the source files and do not modify the project.

### Notes

The recompilation step requires a game `EBOOT.BIN` obtained from a legally owned copy of the game. The original game data is not included in this repository.

The generated sources in `build/gen` are derived from the game, so they are never committed.

## Credits

### PSP Recompilation Project

This project builds upon the work and tooling provided by **psprecomp**, which makes the recompilation of PSP software possible.

### FFmpeg

Audio functionality makes use of **FFmpeg**. pmf is not handled yet

### PSP2i Community

Thanks to the **Phantasy Star Portable 2 Infinity community** for research and documentation of the game and providing mods like the fan translated patch. Thanks to Blade for the logo

## Experimental Apple Silicon macOS build

A native SDL3/OpenGL build with FFmpeg movie decoding is available. See
[the macOS build instructions](port/macos/README.md) for dependencies, game-file
setup, controls, limitations, and decoder validation. Run `./build_macos.sh`
to build, then `./Launch-PSP2i.command` to play.
