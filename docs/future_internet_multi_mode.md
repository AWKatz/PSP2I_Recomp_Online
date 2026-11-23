# Future expansion: Internet Multi Mode

Status: **not started**. Saved for later. The current online work uses Multi Mode (ad hoc) over `psp2i_server`, with a server-side party directory to help players find parties.

The notes below come from the game's own strings and code layout. Internet Multi Mode has not been run.

## What the mode offers

| | Multi Mode (used now) | Internet Multi Mode |
| --- | --- | --- |
| Getting in | Multi Mode → Matching Counter | PSN login (`MODE_INFRA_LOGIN`, `TicketReceived`), then server select (`MODE_INFRA_SERVER_SELECT`) |
| Finding a party | ad hoc scan for nearby hosts | a **room browser**: leaders publish a room, other players search the list and join (`SUBMODE_INFRA_*_ParentPublishRoomPoll`, `ChildWaitForJoinRoomResult`, `%d room(s) found`, `Room owner:`) |
| Rooms | one party lobby | separate **lobby** rooms and **mission (match)** rooms (`SUBMODE_INFRA_LOBBY_*`, `SUBMODE_INFRA_MATCH_*`) |
| Extras | none | room text chat (`SCE_NP_MATCHING2_ROOM_MSG_EVENT_ChatMessage`), partner card search (`SUBMODE_INFRA_PARTNERCARD_SEARCH_*`), mail (`TInfraMailsManager`), kicking members, HTTPS content |
| Room size | 4 | set per room by the server (`Misc.NumPlayersMax`, `maxSlot`). The party arrays are still 4 ([player_limit_offsets.md](player_limit_offsets.md)), so mission rooms would stay at 4. Lobby rooms are unknown. |

## What it would take

All three of these are stubs today (`psprecomp/src/hle/offline.c`, [net_hle_audit.md](net_hle_audit.md)):

1. **PSN sign-in** (`sceNp*`): a signed-in account and a login ticket.
2. **PSN room matching** (`sceNpMatching2`, about 50 functions): server info, room create/search/join/leave, member events, room messages. `psp2i_server` would stand in for Sony's matching servers.
3. **SEGA's game server over HTTPS** (`sPsuCmdServerLoginData`, `TPsuInfraClient`, `THttpsStream` keep-alives). The servers are gone and the protocol is undocumented; this is the largest unknown and needs reverse engineering from the client.

The executable also contains SEGA's server-side game code (`../../Source/ServerCommon/...`: `QuestJudge`, `QuestTable`, `Lottery`, `GameRoom/BossManager`). Either the party host runs it in Internet mode, or it is shared code. Either way it documents what the server is expected to compute.

## Before starting

The upstream README credits "Infrastructure handling" to the PSP2i Discord community (https://discord.gg/FeGvdBkrGa). Check whether they have documented or rebuilt the Internet-mode server protocol; that would remove most of the risk in step 3.

A first investigation pass would trace what the game calls on its way into Internet Multi Mode (with `PSP2I_NETTRACE=1`) and size the three pieces above.
