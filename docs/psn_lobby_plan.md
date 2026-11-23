# Plan: 12-player lobby rooms through Internet Multi Mode

Status: plan (2026-11-23). It replaces the visitor-avatar route for Tasks 3.2/3.3 ([lobby_visitors_research.md](lobby_visitors_research.md)).

## Goal

Players see and meet each other in the game's own **lobby rooms**: the Internet Multi Mode lobby, where each room holds **12 players**. Parties of up to 4 form there and leave for missions, as on the original service. Our dedicated server stands in for Sony's PSN servers and SEGA's game servers.

12 is the game's own limit for a lobby room. The room attribute builder `0x08A694BC` sets `Misc.NumPlayersMax` = 12, and the matching code keeps 12 member records of 136 bytes. A server of 32 players runs several lobby rooms side by side, listed in the game's room browser.

## What Internet Multi Mode uses

| Layer | Game side | Count | Ours today |
| --- | --- | --- | --- |
| PSN sign-in | `sceNp`, `sceNpAuth`, `sceNpService` (NP id, login ticket) | 16 functions | stubs: requests fail ([offline.c](../psprecomp/src/hle/offline.c)) |
| Room matching | `sceNpMatching2` (contexts, server/world/lobby info, room create/join/search/leave, member events, room data, room messages, signaling) | 23 | stubs: requests fail |
| SEGA's game server | `TPsuInfraClient`: a com/sub command protocol over HTTPS (`sceHttp` 23, `sceSsl` 2): login (`RcvInfraLogin`), keep-alive, `RenewSession`, character info, party name and comment, friends, blacklist, mail, web events | about 15 commands seen | none |
| Game traffic between members | `sceNetInet` sockets (23), `sceNetResolver`, `sceNetApctl` (connection state) | 32 | partly (the ad hoc relay carries Multi Mode today) |
| System dialogs | `sceUtility` network config / PSN sign-in screens | a few | stubs |

The executable also contains SEGA's own server code (`../../../Psu/Source/ServerCommon/...`: quest judging, lottery, `GameRoom/BossManager`). It documents what the game expects the server to compute, and some of it may be usable as reference for our implementation.

## Changes to the dedicated server (`psp2i_server`)

1. **Accounts and NP identity.** Give each player an NP id (online name) and a session ticket at the MMO handshake. Reuse the existing `accounts` / session tables in SQLite. No real PSN: the client asks our server instead.
2. **Matching service (stand-in for `sceNpMatching2`).** New control messages and server state:
   - one server, one "world", and the **lobbies** the game asks for;
   - **rooms**: lobby rooms (max 12) and match rooms (max 4), each with an owner, members, slots, room data (internal and external), password and search attributes;
   - requests: create/join room, join room, leave room, search rooms (the game's "%d room(s) found"), get/set room data, get member info, kick;
   - events pushed to clients: member joined/left, owner changed, room updated, room destroyed;
   - **room messages**: the game's lobby packets and chat (`sendRoomMessageLobbyPacket`, `handleRoomMessageLobbyPacket`, chat messages).

   Lobby rooms are created on demand. When one fills to 12, the next player gets a new room or picks one in the browser. The server's 32-player cap stays.
3. **Infra game server (stand-in for SEGA's).** Answer the `TPsuInfraClient` commands over our own connection rather than HTTPS:
   - login (user number), alive / renew session, logout;
   - character info and party name / comment, stored per session;
   - friends, blacklist, mail and partner card search: start with empty answers, and add storage in SQLite later if wanted;
   - web events: map to the server's existing event switches.

   The exact message layouts must be worked out from the client: its `Send_*` and `Rcv*` handlers and `ExecCom` (com, sub, length). This is the largest unknown.
4. **Game traffic relay.** Members of a room exchange game data directly in Internet Multi Mode (internet sockets, found through matching signaling). Route that traffic through the server, as the ad hoc relay does today, keyed by room and member. This includes the 12-player lobby rooms, so expect up to 11 peers per client.
5. **Admin and limits.** Show rooms and their members in the web admin panel and the console; add kick and close-room commands. Rate limits per room. Lobby room size stays at the game's 12; match rooms at 4.

## Changes to the client (MMO builds)

- **`sceNp*`:** report signed in, with the NP id and ticket from our server (instead of failing).
- **`sceNpMatching2`:** implement the 23 functions over the server's matching service, including asynchronous callbacks and events, delivered on the game's threads the way the ad hoc layer already delivers events.
- **`sceHttp` / `sceSsl`:** route the infra client's HTTPS requests to our server, with no real TLS: answer locally or forward over our connection.
- **`sceNetInet` / `sceNetApctl` / `sceNetResolver`:** report connected; carry member-to-member traffic through the relay.
- **Menus:** Internet Multi Mode becomes the path into the shared lobby. The current Multi Mode (ad hoc) path keeps working.
- **Party finder and warp:** check whether they're still needed. The game's own room browser and lobby replace parts of them.

## Phases and checks

| Phase | Work | Check |
| --- | --- | --- |
| 0. Recon | Trace the game's calls into Internet Multi Mode with all stubs logging (`PSP2I_NETTRACE`); list the order of NP, matching and HTTPS calls; map the `TPsuInfraClient` commands it sends first | A call log from title screen to the server-select screen |
| 1. Sign-in | NP sign-in succeeds; infra login and keep-alive answered | The game reaches server select / the lobby screen without errors |
| 2. Room browser | Lobby info, room search and room list | Two clients see the same list of lobby rooms |
| 3. Join a lobby room | Create/join, member events, room data | Two clients enter the same lobby room and see each other |
| 4. Moving together | Game traffic relay between members | Movement and actions show on both clients, at 60 fps |
| 5. Up to 12 | Rooms filling to 12, overflow to new rooms | 12 clients (or 2 real clients plus server-driven test players) in one room |
| 6. Parties and missions | Match rooms (4) created from the lobby; leaving and returning | A party of 4 from a lobby room plays a mission and comes back |
| 7. Extras | Chat bubbles and symbol chat (room messages), guild cards, friends | As wanted |

## Risks

- **The infra server protocol is undocumented.** Everything has to be worked out from the client. Mitigation: the client's own handlers name every command, and the PSP2i community (upstream credits "Infrastructure handling" to its Discord) may have documented or rebuilt it. Ask there before phase 1.
- **Mode differences.** Internet Multi Mode was a separate online mode with its own rules (missions and items). Some single-player or ad hoc features may behave differently there.
- **Testing at 12** needs either 12 client processes (heavy) or server-side test players that speak the protocol.
- **Size.** This is the largest piece of work in the project so far, comparable to Phases 1 and 2 together. Phase 0 is the decision point: after it the protocol volume is known.

## What stays

The Multi Mode (ad hoc) path, the party finder, lobby warps and weapons in the lobby keep working for players who use Multi Mode. The single-player build is unchanged.
