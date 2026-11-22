<h1 align="center"> Phantasy Star Portable 2 Infinity Recomp</h1>

<p align="center">
   <img src="asset/Logo.png" alt="Icon">
<img width="1072" height="688" alt="Screenshot 2026-11-22 at 2 15 07 PM" src="https://github.com/user-attachments/assets/448d059a-fbe5-439f-8fba-1b685340e1c0" />


</p>

A recompilation project for **Phantasy Star Portable 2 Infinity (PSP2i)**, bringing the game to a native PC environment while preserving the original game's functionality and feel.

**This fork (PSP2I_Recomp_Online)** adds online play: an MMO-capable client build and a self-contained dedicated 32player server with a web admin panel. The single-player build is unchanged unless you build with `-DENABLE_MMO_NET=ON` (client) or `-DBUILD_DEDICATED_SERVER=ON` (server).

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
| `--max-players N` | 32 | 1–32: one lobby/hub holds at most 32 players |
| `--rate-limit N` | 120 | online: packets per second per player (bursts up to 2x; kicked after 3 s over) |
| `--handshake-timeout S` | 5 | seconds a new connection has to send its handshake |
| `--kill-limit N` | 10 | enemy-kill reports per second per player |
| `--db PATH` | psp2i_server.db | the SQLite database (accounts, characters, drops, audit log) |
| `--drop-tables PATH` | config/drop_tables.json | loot tables |
| `--rates PATH` | config/rates.toml | rate multipliers |
| `--stats S` | 60 | print a status line every S seconds (0 = off) |
| `--run-seconds S` | off | exit after S seconds |
| `--web-port N` | off | serve the web admin panel on this TCP port |
| `--web-bind ADDR` | 127.0.0.1 | the panel's address: only this machine can reach it by default |
| `--no-console` | | don't read commands from stdin |
| `--add-admin NAME PASS` | | create an admin account and exit |
| `--help` | | |

Web admin panel:
- Run `psp2i_server --add-admin NAME PASSWORD` once, then start the server with `--web-port 8080` and open http://127.0.0.1:8080/.
- From the panel you can:
  - watch live telemetry and the player list;
  - kick, ban and unban players, IPs and accounts, and promote or demote admins;
  - change rates and turn events on or off;
  - edit the drop tables;
  - send announcements;
  - search the audit log;
  - use a console.

Commands: type them in the server's terminal, in the panel's Console tab, or send them over RCON:
```sh
curl -H "Authorization: Bearer $TOKEN" -d '{"command":"/status"}' http://127.0.0.1:8080/api/v1/rcon
```

| Command | What it does |
| --- | --- |
| `/status`, `/players` | server health and who is on |
| `/kick <session> [reason]` | disconnect a player |
| `/ban <session\|ip:ADDR\|account:N> [minutes] [reason]` | ban (0 minutes or none = permanent); the player is disconnected at once |
| `/unban <ip:ADDR\|account:N>` | lift a ban |
| `/account add <name> <password> [admin]` | create an account |
| `/admin <account id> on\|off` | promote or demote an admin |
| `/announce [colour] <text>` | announce to every player (colours: white, yellow, red, green, blue, cyan, magenta, orange, or #RRGGBB) |
| `/setrate <name> <value>`, `/rates` | change or show the rate multipliers |
| `/reload drops\|rates\|all` | re-read the config files |
| `/event <name> on\|off`, `/events` | toggle an event or lobby theme (spring, summer, halloween, christmas, new_year, anniversary) |
| `/audit [text]` | the last 15 audit entries |
| `/shutdown`, `/help` | |

Signals:
- Ctrl-C or `SIGTERM` shuts the server down and tells every client.
- `SIGHUP` reloads the config files.
- Editing a config file also takes effect within a second, without disconnecting anyone.

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

## Online roadmap (this fork)

| Phase | Status |
| --- | --- |
| 1. Client networking: isolated network layer, ENet transport, MMO client entry | done |
| 2. Dedicated server: headless server, SQLite persistence, server-side loot, web admin | done |
| 3. Break the 4-player limits | next |

Details are in [docs/net_hle_audit.md](docs/net_hle_audit.md) and [server/README.md](server/README.md).

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
* Linux and Steam Deck (Flatpak; untested so far)
* PPSSPP and PSP saves load and save, encrypted as on hardware
* **Online (this fork):**
  * MMO client build: the game's ad hoc Multi Mode joins a server over ENet. The server is set in `client_config.toml` or found on the LAN automatically.
  * Dedicated server `psp2i_server`: headless, 30 Hz, no runtime dependencies, about 2–4 MB of RAM.
  * LAN and online modes, handshake checks, rate limiting, and bans by IP or account.
  * Embedded SQLite database for accounts, characters and JSON inventories. Trades are crash-safe, so items can't be duplicated.
  * Loot rolled by the server, with live rate multipliers and drop tables that reload while it runs.
  * Web admin panel, console and RCON, with an audit log.

> This project is actively being developed. Capabilities and compatibility may change as development progresses.


## Issues

Current issues include:

- 60 fps is not properly implemented
- Adhoc not supported properly (relay has best support)
- Online, this fork: the client completes the handshake and reaches the Matching Counter. Party play over the server (the rest of the ad hoc API, beginning with `sceNetAdhocctlAddHandler`) is not done yet.
- Online, this fork: the shipped drop tables are placeholders until PSP2i's real tables are extracted.
- The PPSSPP save support uses PPSSPP's `libkirk` (GPL; `port/third_party/libkirk`).

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
