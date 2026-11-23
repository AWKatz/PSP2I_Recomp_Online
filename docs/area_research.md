# Task 4.1 research: areas, warps, enemies, lobby weapons

Status: first research pass (2026-11-23). These notes back the roadmap's Task 4.1 additions: the warp panel (option A), weapons in the lobby (option 1) and an enemy-free field (option 2). Addresses are in the game's memory (EBOOT 0x08804000+). The heap addresses came from one session and can move.

## The area manager

| What | Where |
| --- | --- |
| Singleton pointer | `0x08ED4DD0`. The getter `0x089F97D4` creates it on first use (92 bytes) and has 103 callers. |
| Vtables | `0x08E30EE0` (primary) and `0x08E30F24` (sub-object at +32). It is a task object: +68 is its state, set to 1 by `0x089FA428` and to 3 by `0x089FA450`. |
| Area pairs | (area type u32, zone id s16) at +40, +48 and +56. |
| Readers | `0x089FA8FC` passes +40/+44 to the draw-distance table (`0x089FF210`, area types 2–23); `0x089FA2C0` uses the same pair for the quest counter. |
| Area predicates | `0x089FA134`… are small functions of the form "type 23 and zone in {1100, 1200}", "{3300, 3400}", 3500, 3600, 3100, 3200, 3000. |

Measured (logged by `test area:` lines in the client log, under `PSP2I_TEST_INPUT`):

| Moment | +40 | +48 | +56 |
| --- | --- | --- | --- |
| Title, Matching Counter | 0, 0 | 0, 0 | 0, 0 |
| Multi Mode party lobby | **2, 2000** | 2, 2000 | 0, 0 |

So **the lobby is area type 2, zone 2000**. My guess is that +40 is the current area, +48 the area being loaded, and +56 the previous area. That needs a transition between two real areas to confirm.

## Warps (option A): working

The move path, as logged on both clients during a lobby → mission → lobby trip (2026-11-23):

1. **Request:** the game mode object (`*0x08ED2958`, getter `0x08B34610`, vtable `0x08E1BC98`) has virtual +0xE0, `0x0893E424(mode, destination, area index, zone, entry)`. It stores the destination at mode +536 / +540 / +544 / +548.
   - Leaving the lobby: destination 0x7FFFFFFF (interim), from `0x0893EAB8`.
   - Entering a mission: destination 4219000 (the mission), the rest −1, from `0x0888AD60` (mission start).
   - Returning to the lobby: destination 2010000, area index 0, zone 2000, entry 0, from `0x0893D138`.
2. **Sequence step:** `0x0893DEA0` (run from `0x08941920`) carries the move out and resolves the area (`0x0893AC58`, `0x08940740`).
3. **Area manager:** `0x089F9580(sub, type, zone)` sets +48 (next) and requests code 2 at +68. The per-frame update `0x089F9B00` then commits (+48 → +40, +40 → +56, `0x089F9BCC`) and loads (`0x089F9CA4`).

**Every client makes its own move.** Members aren't pulled along by the leader: each client calls the request itself, and for missions the session tells them to.

**The destination id is a quest table number × 1000 + entry** (`ServerData/Quest/QuestTable/XB/table%04d.nbl`). Destination 2010000 is the Multi Mode lobby. A destination whose table isn't loaded is silently ignored (2020000 did nothing). Missions load their table through the quest-table request task first.

**Test command `warp<dest>,<area index>,<zone>,<entry>`.** It calls the request from a hook on `0x089F9B00`, on the game thread. Results with both clients warped together, with players, HUD and party intact:

| Command | Result |
| --- | --- |
| `warp2010000,0,2000,0` | lobby: the bridge with the mission terminal |
| `warp2010000,0,2000,1` | lobby: an open deck with four boards and a door, under a nebula sky |
| `warp2010000,0,2000,4` | lobby: the plaza (where Multi Mode starts) |
| `warp2010000,0,2000,5` | lobby: a counter room with four boards |
| `warp2010000,0,2300,0` | an empty void: the area index has no map for that zone; nothing crashed |

More entry points (named in game, 2026-11-23): 9 Battle Shop, 11 Style Shop, 15 and 16 the Style Shop's Salon, 17 Little Wing (any entry above 17 also lands there); 6, 8, 10, 12, 13 and 14 are spots in the plaza by its doors and gates, 7 another spot on the Observation Deck. Entry 2 is the Cafe, 3 the Shop Plaza. Social areas outside the lobby: see the next section.

## Social areas outside the lobby: blocked (2026-11-23)

**The candidates.** The quest file list in memory (`filelist.rel`: destination id → quest folder) names the social areas outside the lobby:

| Folder | Destination id |
| --- | --- |
| `city2` | 2011000 |
| `kaigan_map_0_0` (beach) | 2708900 |
| `hakobune_map_0_0` (ark) | 2709000 |
| `sidestory01`…`05` | 2301500, 2301600, 2301700, 2302000, 2302100 |
| `tm_001`…`006` | 2900100…2900600 |
| `robbie_off_0_0`…`0_2` | 2050000…2050002 |

None of them is in any quest table (tables 0–7, 12, 100–106, 201–206, 301–306 were each requested and searched); they are single-player quests.

**How Multi Mode moves.** Online, the move step (`0x0893DEA0`) does not move: it sends session message 0x0807 (destination, area index, zone, entry) to the leader's session server and waits. The server loads the destination's quest (quest data manager vfunc +0, `0x08D251D0`). 2010000 (the lobby) is built in (`city.rel`). Anything else is looked up in the server's `filelist.rel` (`0x08D25004`), then read from `QuestData/<folder>/chunk.nbl`, which must contain `quest.rel`.

**Where it stops.**

1. The server's `filelist.rel` has no entry for these quests, so the lookup returns nothing and the quest load fails (`0x08D25004` → 0). Supplying the folder name gets one step further.
2. There is no `quest.rel` to be found. `chunk.nbl` (missions' format) is missing or doesn't have it; the single-player `XB/quest_j.nbl` doesn't have it either.
3. Without the server, forcing the move step's offline path (`0x08ACFB3C` → 0) reloads the current area instead, with the players missing: the client can only reach destinations in the quest it has loaded (`city`).

**What it would take.**

- Teach the session server the single-player quest format: read `quest_j.nbl` and build the server-side quest from it.
- Or change the clients' loaded quest from `city` to `city2` (as story mode does when it enters these areas) and keep the session server in its lobby state.

Either is a larger reverse-engineering project than the lobby rooms.

Tools used (taken out again, simple to restore): a `loadtable<N>` test command (`0x0888B594(table, -1, 0)`, the counter's quest-table request) and logging of the quest data manager's lookups.

## Weapons in the lobby (option 1): working

The lobby is a town. Two separate things keep weapons out of it:

1. **No weapon palette in towns.** The client's town checks (`0x089F98AC`: area category 2, and `0x089F98E4`) turn off the field HUD, so the palette never appears. Answering "not a town" in the lobby brings the palette back. The one exception is the **scene loader** (the call returning to `0x08A00408`): its answer picks the rooms' map data, and the Bridge loads black when it is told "not a town".
2. **The session server holds back the change.** Picking a weapon set sends a 48-byte 0x0105 to the party leader's session server. The server applies it (`0x0882A288`), then asks `0x0886C4DC` ("no field object": obj+36 == 0) and, in a lobby, skips telling anyone. Three places ask:
   - `0x0882A4B8` (return `0x0882A4C0`) before calling room vfuncs +88 and +140;
   - room vfunc +88 (`0x08D1EF34`, the 0x0A01 notice) at `0x08D1EF60`;
   - room vfunc +140 (`0x08D1F17C`) at `0x08D1F294`, in front of the 56-byte 0x0105 equip message to every player.

   Passing those three makes the weapon appear for the player and for everyone else.

The lobby menu's "Weapons in the lobby" does both (`port/host/lobby.c`). It is the party leader's setting: it travels in every party warp (MSG_S2C_PARTY_WARP flags), and switching it warps the party back into its room. Re-entering the room is what makes it apply: on, the palette appears; off, drawn weapons are put away (the town checks alone only hide the palette). Armour changes always worked in the lobby: they take a different message (0x0105 with +6 = 7), which the server handles in any room.

How it was found: function traces of the same palette switch in a field and in the lobby, then the lobby weapon switch against a lobby armour switch (the `tracestart` / `tracestop` test commands in a `-DPSP2I_TRACE=ON` build). Things that did not matter: the scene mode (0x08EDA010), the "battle allowed" check (`0x08A38E2C`), the items' +32 flag and the lobby motion sets.

## Enemy-free field (option 2)

- Enemy placements come from per-area "set" data: `set.rel` and `enemy.rel` are loaded at `0x0880C3C4` / `0x0880C49C` / `0x0880D714`. Skipping or emptying the enemy entries there, behind an "event field" flag, is the likely hook. Quest gates like "defeat all enemies" are a separate check: `seq.get_destroy_enemy_count` (`0x08953EA8`).

## Tools added for this

- `test area:` log lines (test input builds): the three area pairs whenever they change.
- Test commands: `watch<hex>` / `unwatch` (float triples to `logs/watch.txt`), `poke<addr>=<value>`, `hold<button>` / `release`, `warp<dest>,<area index>,<zone>,<entry>`, `lobbyweapons<0|1>`, and `tracestart` / `tracestop` (trace builds).
- `PSP2I_PKT_TRACE=<type>|all`: the game's stack when it sends a session message of that type (ad hoc PTP sends).
