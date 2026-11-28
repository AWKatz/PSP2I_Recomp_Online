<h1 align="center"> Phantasy Star Portable 2 Infinity Recomp</h1>

<p align="center">
   <img src="asset/Logo.png" alt="Icon">
<img width="1072" height="684" alt="Screenshot 2026-11-22 at 6 17 27 PM" src="https://github.com/user-attachments/assets/5928fb23-a0ef-4985-99ac-99bd553b3e49" />
<img width="1072" height="684" alt="Screenshot 2026-11-22 at 6 15 17 PM" src="https://github.com/user-attachments/assets/c2309d21-c338-4c73-890c-2ac3cc5242ef" />
<img width="1072" height="684" alt="Screenshot 2026-11-22 at 6 14 13 PM" src="https://github.com/user-attachments/assets/d6768855-1761-4723-90a3-284ba5190348" />



</p>

A fork of the recompilation project for **Phantasy Star Portable 2 Infinity (PSP2i)**, bringing the game PC/macOS/Linux environments while preserving the original game's functionality and feel.

**This fork (PSP2I_Recomp_Online)** brings back the game's online play as it was on the original PSN and SEGA servers, as a standing reproduction for small groups of players. You run a small server of your own, on your LAN or for friends, and the game connects to it as it once did to the official services: the online Terms of Use and Notices, the matching server, Internet Multi Mode's lobby rooms, and Multi Mode parties. Player limits are the original servers': 12 players per lobby room and 4 per party, with up to 12 players on a server. A few extras sit on top: warping your party around the lobby, showing your weapons in the lobby, and a simple web admin panel.

The game always builds with online play; Story Mode plays as before.

Online features at a glance:

- **Party finder**: pick a party being hosted on the server, or start your own, from the Matching Counter.
- **Lobby warps**: the party leader takes the whole party to any of ten rooms of the Multi Mode lobby (Bridge, Cafe, Battle Shop, Style Shop, Little Wing and more).
- **Weapons in the lobby**: the party leader can let everyone draw their weapons in the lobby to show off their gear, or keep the original game's look.
- **Character looks**: wear the look of any of the game's 87 partner characters (Emilia, Ethan, Nagisa and the other story characters, and the GH-series presets), and switch back to your own at any time. In the F2 menu, anywhere in the game.
- **Player limits as on the original servers**: Internet Multi Mode lobby rooms hold 12 players and Multi Mode parties hold 4, 1:1 with the original game servers. The server runs several lobby rooms side by side to reach its total.
- **Your own server**: stands in for the original PSN and SEGA services for up to 12 players, with server-side loot, SQLite persistence and a simple web admin panel.


## Quick Start

You need your own copy of the game: the disc image (`.iso`) and a decrypted `EBOOT.BIN`. The game is built on your machine from your `EBOOT.BIN`, so there are no prebuilt downloads.

**1. Put `EBOOT.BIN` in the repository root.**

**2. Build.**

| Platform | Install once | Build | You get |
| --- | --- | --- | --- |
| macOS (Apple Silicon) | `brew install cmake ninja sdl3 ffmpeg` | `./build_macos.sh` | `build/PSP2i_macOS/` |
| Linux / Steam Deck | the packages in [port/linux](port/linux/README.md) | `port/linux/appimage/build_appimage.sh` | `build/PSP2i_Linux/` |
| Windows | Visual Studio and CMake | `build_port.bat` | `build\PSP2i_Windows\` |
| Android | see [port/android](port/android/README.md) | `build_android.bat` | `build\dist\psp2i-android.apk` |

Every build, Android included, has online play; there is no offline build. Story Mode plays as before without a server.

**3. Put your game files in the build folder and play.**

Each desktop build produces one self-contained, portable folder, `build/PSP2i_<system>/`. It holds the game, the server and everything they read, with no installer, no system folders and no setup elsewhere. Everything you supply goes in the folder's root, next to the game. Copy or move the whole folder anywhere (another drive, a USB stick, another machine with the same OS) and it keeps working. The build copies `EBOOT.BIN` in for you. Every platform uses the same layout:

```
PSP2i_<system>/                 PSP2i_macOS, PSP2i_Linux or PSP2i_Windows
  psp2i                         the game (psp2i.exe on Windows, PSP2i-<arch>.AppImage on Linux)
  your .iso                     you add: the disc image, any name (the first .iso in the folder is used)
  EBOOT.BIN                     the decrypted game executable (copied in by the build)
  font/                         you add: the PSP firmware fonts (*.pgf); without them, menu text is missing
  NPJH50332/                    your save folder: copy PPSSPP's memstick/PSP/SAVEDATA/NPJH50332 here to continue
  psp2i.ini                     optional settings (see below); created when you change one in game
  client_config.toml            optional: which server to join (found on the LAN by default)
  terms_of_use.txt, notices.txt server owners: the texts shown when players connect
  TEXTURES/NPJH50332/           optional: an HD texture pack made for PPSSPP
  logs/                         created on first run
  server/                       the dedicated server, self-contained too
    psp2i_server                (psp2i_server.exe on Windows, the server AppImage on Linux)
    config/                     loot tables and rates (drop_tables.json, rates.toml)
                                its database is created here on first run
```

The game's macOS shortcut, `Launch-PSP2i.command` in the repository root, starts `build/PSP2i_macOS/psp2i`. The server runs from its own folder: `cd server && ./psp2i_server --lan`; see [Online play and the server](#online-play-and-the-server). Older layouts are moved into place on first start: fonts in `GameData/flash/font/` move to `font/`, and saves in `GameData/ms/PSP/SAVEDATA/` move to the root. Android keeps its own layout: the app asks for a folder on first start (see [port/android](port/android/README.md)).

Then run it. If something is missing, the game says what and where.

HD textures use PPSSPP's pack format and hashing, so PPSSPP packs work as they are: copy the whole `memstick/PSP/TEXTURES/NPJH50332` folder, including its `textures.ini`. Two environment variables help:
- `PSP2I_TEXTURES=DIR` uses a pack from somewhere else.
- `PSP2I_TEXDUMP=1` writes every texture the pack doesn't replace to `<pack>/new/` (as `<hash>.png`, listed in `new/textures.ini`), which is how packs are made.

Other Windows build scripts: `build_port.bat regen` (regenerates the game code first) and `build_prod.bat` (the community build in `build\prod\`). The older Windows layout, with the extracted disc in `GameData\disc\`, still works; see [port/prod_README.txt](port/prod_README.txt).

### Quick Start: the server in Docker (a NAS, a home server)

The server can also run on its own, always on, in a container: on a Synology NAS with Container Manager, or on any machine with Docker. It needs no game files, and the web admin panel works from the container too. The image is built from source on the machine that runs it.

1. **Get the repository onto that machine**, for example in a Synology shared folder `docker/psp2i`:

   ```sh
   git clone https://github.com/AWKatz/PSP2I_Recomp_Online.git psp2i
   ```

2. **Set the web panel's admin password** in `docker-compose.yml` at the repository's top (`PSP2I_ADMIN_PASSWORD`, at least 8 characters).

3. **Build and start it:**
   - **Synology:** Container Manager > Project > Create. Path: the cloned folder (the one holding `docker-compose.yml`). Source: **Use existing docker-compose.yml**.
   - **Docker elsewhere:** `docker compose up -d --build` in the cloned folder.

4. **Open the admin panel** at `http://<server address>:8080/` and sign in. Then remove the two `PSP2I_ADMIN_*` lines from `docker-compose.yml` and rebuild.

Games on the same network find the server by themselves. To join from elsewhere, set `server_ip` in `client_config.toml`. The server keeps its database and config in `docker/data/`. To update it, `git pull` and rebuild. Players outside your home, HTTPS for the panel, backups and troubleshooting: [docker/README.md](docker/README.md).


### Frame rate and resolution

Both are set in `psp2i.ini`, a plain-text file in the same folder as your game files (the last column of the table above; on macOS, the repository root next to `Launch-PSP2i.command`). Edit it with any text editor while the game is closed:

```
iso=game.iso
fps=60
resolution=1920x1080
```

| Setting | Values | What it does |
| --- | --- | --- |
| `fps` | `30` (default), `60` | The game's frame rate. 60 runs the game's own timing at 60, as PPSSPP's 60 fps cheat does, so no cheat file is needed. Also in the in-game settings menu (FRAME RATE), which saves it back to `psp2i.ini`. |
| `resolution` | `WIDTHxHEIGHT` (e.g. `1280x720`, `1920x1080`, `2560x1440`, `3840x2160`), or `native` | The window size and the internal rendering resolution: the nearest whole multiple of the PSP's 480x272, up to 8x (`1920x1080` renders at 4x, 1920x1088). `native` renders at 480x272. Without the line, it is `1920x1080` (4x). Read at startup only. |

`resolution` applies to the macOS, Linux and Android builds. On Windows the window size is chosen in the settings menu (RESOLUTION), and the internal resolution under GRAPHICS > INTERNAL RES: 1x to 4x the PSP's 480x272, 4x by default, used from the next start (OpenGL only; both kept in `psp2i_display.ini`). HD textures only show their detail above `native`. The settings menu rewrites its own keys in `psp2i.ini` and keeps the others, including `resolution`.

PPSSPP and PSP saves load and save encrypted, so they work in both directions. In-game, Cross confirms.


### Controls

The keyboard uses PPSSPP's default layout. Click the game window first. A gamepad works as soon as it's connected, including the Steam Deck's controls.

| PSP | Keyboard | Gamepad |
| --- | --- | --- |
| D-pad | Arrow keys | D-pad |
| Analog stick | I / J / K / L (up / left / down / right) | Left stick |
| Camera | — | Right stick |
| Circle (confirm) | X | B / Circle (east) |
| Cross (cancel) | Z | A / Cross (south) |
| Square | A | X / Square (west) |
| Triangle | S | Y / Triangle (north) |
| L / R | Q / W | Shoulders or triggers |
| Start | Space | Start / Menu |
| Select | V (or Enter) | Back / View |
| Settings menu | F1 | R3 or Guide |
| F2 menu (party, lobby, character) | F2 | L3 (left stick click) |

On Android, the cyan touch button at the top of the screen (right of MENU) opens the F2 menu.

This is the Japanese release, so Circle confirms and Cross cancels. On Windows the keys can be rebound in the settings menu (CONTROLS > KEYBOARD/MOUSE).


### Online play and the server

```sh
./build_macos.sh              # build/PSP2i_macOS/psp2i and build/PSP2i_macOS/server/psp2i_server
cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/server    # the server alone, any OS: build/server/psp2i_server
cd build/PSP2i_macOS/server && ./psp2i_server --lan    # run it where config/ is, or pass --drop-tables/--rates
```

To run the server persistently in a container (a Synology NAS's Container Manager, or any Docker host), web panel included, see [the Docker quick start](#quick-start-the-server-in-docker-a-nas-a-home-server) and [docker/README.md](docker/README.md).

`psp2i_server` options:

| Option | Default | |
| --- | --- | --- |
| `--lan` / `--online` | `--lan` | LAN: answers discovery probes. Online: strict rate limits, nothing accepted before the handshake |
| `--port N` | 29001 | game port (UDP) |
| `--bind ADDR` | all interfaces | address to listen on |
| `--lan-port N` | 29000 | LAN discovery port (UDP) |
| `--name NAME` | PSP2i Server | name shown to LAN clients |
| `--max-players N` | 12 | 1–12: players on the server in total (each Internet Multi Mode lobby room still holds 12, each Multi Mode party 4) |
| `--rate-limit N` | 120 | online: packets per second per player (bursts up to 2x; kicked after 3 s over) |
| `--handshake-timeout S` | 5 | seconds a new connection has to send its handshake |
| `--kill-limit N` | 10 | enemy-kill reports per second per player |
| `--db PATH` | psp2i_server.db | the SQLite database (accounts, characters, drops, audit log) |
| `--drop-tables PATH` | config/drop_tables.json | loot tables |
| `--rates PATH` | config/rates.toml | rate multipliers |
| `--tick-rate N` | 60 | server ticks per second (10–120); 60 matches the clients' 60 fps. Game traffic is relayed as it arrives, between ticks too |
| `--stats S` | 60 | print a status line every S seconds (0 = off) |
| `--run-seconds S` | off | exit after S seconds |
| `--web-port N` | off | serve the web admin panel on this TCP port |
| `--web-bind ADDR` | 127.0.0.1 | the panel's address: only this machine can reach it by default |
| `--no-console` | | don't read commands from stdin |
| `--add-admin NAME PASS` | | create an admin account and exit |
| `--help` | | |


Web admin panel:
<img width="1201" height="892" alt="Screenshot 2026-11-24 at 9 34 53 PM" src="https://github.com/user-attachments/assets/811d34bb-8437-45c8-8c14-cbd6d84361de" />
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
- **`client_config.toml`** (with your game files):
  - `server_ip`: leave it out to find a LAN server;
  - `server_port` (29001);
  - `lan_discovery` (true);
  - `discovery_port` (29000);
  - `player_name`;
  - `party_finder` (true): open the party finder by itself at the Matching Counter (the F2 menu opens it any time).
- **`terms_of_use.txt`** (with your game files): plain UTF-8 text shown in the game's Terms of Use window when a player connects in Internet Multi Mode. Edit it to set your server's rules; the window wraps long lines on its own. Without the file a one-line placeholder is shown.
- **`notices.txt`** (with your game files): the same, for the Notices window that follows the Terms of Use (server news).

**The F2 menu.** F2 (keyboard), L3 (controller) or the cyan touch button (Android) opens and closes it anywhere in the game, Story Mode included. It has two columns:

- **PARTY** (left): Party finder, the lobby's rooms, Lobby weapons, Close.
- **CHARACTER** (right): My own character, then the 87 partner characters with their race and sex.

Rows that don't apply where you are, such as the rooms outside the Multi Mode lobby, are greyed out, and say why if you choose them. When the menu opens, the cursor starts where it's most useful: on the rooms in the lobby, on the party finder at the Matching Counter, and on the character list elsewhere.

| | Keyboard | Controller | Touch (Android) |
| --- | --- | --- | --- |
| Move | Arrow keys or I / K (hold to repeat) | D-pad or left stick | the touch D-pad or stick, or tap a row |
| Switch column | Left / Right | Left / Right | tap the other column |
| Choose | X (Circle) | B / Circle (east) | tap the selected row again |
| Close | Z (Cross) | A / Cross (south) | the F2 button |

While the menu is open the game gets no input.

<img width="1072" height="684" alt="Screenshot 2026-11-27 at 10 00 34 PM" src="https://github.com/user-attachments/assets/ef0f893c-40b0-4b7a-9059-aff177592bd1" />

**Character looks.** Pick a character in the CHARACTER column to wear their look: their race, body and outfit. Your class, levels, weapons, colours and everything else stay yours. The game builds your model when an area loads, so the look shows then: **in a lobby, the menu enters the area again for you** at once. In a mission, it shows from the next area you enter (the menu won't reload a mission, which would start it over). Changing armour doesn't redraw you. A notice says which after each choice. The current look is marked `*` in the list.

- **My own character** puts your own look back, the same way.
- Your own look is saved to `character_backup.txt` in the game folder before your first swap. The game saves the look you wear with your character, so if it saves while you wear someone else's, you keep that look after a restart, and My own character still brings yours back. Keep that file until you have switched back.
- Pick a character of another race and you play as that race's body. Race may also change your stats; switch back if you'd rather not.
- The partner characters are part of the game's data once it's loaded (the lobby, missions). On the title screen the list is empty and says so.

**Party finder.** In Multi Mode, the Matching Counter opens a panel listing the parties being hosted on the server (leader, members, free slots), plus START A PARTY and CHOOSE MYSELF. It also opens from the F2 menu's Party finder row, and Cross takes you back to the menu. Pick a party and the client joins it for you: it presses Join Party, shows the game only that leader's party, and confirms. START A PARTY presses Form Party; Begin Party Play stays with the leader. If a step doesn't happen in time, the finder hands control back with a notice.

**Lobby warps and options.** In the Multi Mode lobby, the F2 menu's PARTY column has:

- **Warp the party to** one of the lobby's rooms. Only the party leader can choose; the server passes the choice to every member and the whole party moves together.

  | Room | What's there |
  | --- | --- |
  | Central Plaza | where Multi Mode starts |
  | Shop Plaza | the shop doors |
  | Bridge | the mission terminal |
  | Counter Room | the counters |
  | Cafe | the cafe |
  | Observation Deck | the open deck under the stars |
  | Battle Shop | weapons and armour |
  | Style Shop | clothes and parts |
  | Style Shop: Salon | the Style Shop's salon |
  | Little Wing | the Little Wing office |

- **Weapons in the lobby** (off by default; the party leader's choice for the whole party): show off your gear. With it on, the lobby gets the weapon palette (hold Cross / Z and pick a weapon set as in a mission), and everyone in the party sees the weapons you draw. Switching it re-enters the room you are in, so it applies at once; off puts every weapon away and hides the palette, as in the original game. Attacks do nothing there: the lobby has no enemies.

  In **Internet Multi Mode** (its lobby, and the party lobby you go to after forming a party) it is **each player's own setting**, in the same F2 row: switching it enters your area again so it applies at once. Whether the others see your drawn weapons there is still being tested.

**Test switches (any SDL build).** `PSP2I_TEST_INPUT=1` makes the client read button presses from `<data>/logs/input.cmd` (`start`, `circle`, `cross`, `up`, `down`, …, `wait500`, `f2`, `shot`, which saves the window's picture to `logs/shot_<n>.png`, `stick<u|d|l|r><ms>`, `hold<button>` / `release`, `watch<hex address>`, `dump`, `warp<dest>,<area index>,<zone>,<entry>` and `lobbyweapons<0|1>`), so test scripts can drive several clients at once without window focus. `PSP2I_NETTRACE=1` logs every ad hoc socket call; `PSP2I_RAMDUMP=1` makes F7 save the game's memory to `logs/`.

More detail: [server/README.md](server/README.md), [port/linux/README.md](port/linux/README.md), [port/macos/README.md](port/macos/README.md).


## AI Disclosure

This project makes **heavy use of AI-assisted development, primarily Claude**, for code analysis, reverse engineering assistance, debugging.

AI-generated code and suggestions are reviewed, tested, and integrated as part of the development process.

## About

**PSP2I_Recomp** is a recompilation project for *Phantasy Star Portable 2 Infinity*.

The current goal is to produce a native PC build of the game while maintaining compatibility with the original game logic and improving the experience with modern PC features.

This project is based on the output of the PSP recompilation toolchain and ongoing community reverse-engineering efforts.


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
3. Builds `psp2i.exe` and `psp2i_server.exe` into `build\PSP2i_Windows\`.

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
