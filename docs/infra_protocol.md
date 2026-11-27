# SEGA infra server protocol (Internet Multi Mode)

Status: worked out from the client, 2026-11-23. The stand-in is [port/host/infra.c](../port/host/infra.c) (MMO builds). It answers the game's HTTPS requests on the client for now; it can move to the dedicated server later.

## Transport

The game's `TPsuInfraClient` (created for `game.psp2infinity.jp`, port 12020) POSTs binary packets to `/psp2i/jp/cgi/front.fcgi` through `sceHttp`. Our `sceHttp` hands every request to the transport that `infra.c` installs, so nothing leaves the machine.

## Packets

Little endian.

| Offset | Size | Field |
| --- | --- | --- |
| +0 | u32 | packet length |
| +4 | u8 | com: requests `0x1E`, replies `0x1F` |
| +5 | u8 | sub |
| +6 | 2 bytes | `06 01` in every request; echoed in replies |
| +8 | s16 | replies: status (0 or 200 = OK) |
| +10 | s16 | replies: error code (0 = none) |
| +12 | | payload |

Requests carry `03 01 00 02` at +8, then their own payload.

Replies are dispatched by `ExecCom` (`0x08A6B08C`) through the table at `0x08E38590`. Some requests have no handler in that table. Their reply is a **raw body** with no packet header, read by the requesting screen.

## Requests seen, in order

| Request sub | What | Reply |
| --- | --- | --- |
| `0x80` | front: server status. Payload has a version string such as `06:29:1` | sub `0x81`, `RcvInfraServerStatus`. 16 bytes of `sInfraServerInfo`: +0 s8 state (0 open; 1 gives error 4004, 2 gives error 4003), +2 s16 keep-alive seconds |
| `0x10` | Terms of Use. Carries the NP ticket (+12 u32 length, +16 ticket) | raw body: UTF-16LE text, ending with a 0 character, shown as is. Ours is the server's (`server/config/terms_of_use.txt`, sent after the handshake as `MSG_S2C_SERVER_TEXT`); from a server that sends none, `terms_of_use.txt` in the game folder |
| `0x0E` | patch file | sub `0x0E`, `RcvInfraPatchFile`: +12 u32, nonzero means there's a patch to fetch. Ours: 0 |
| `0x0F` | news | raw body: UTF-16LE text for the Notices window (parsed by `0x0888FBB0`). Ours is the server's (`server/config/notices.txt`), the same way |
| `0x19` | news item, +12 u32 index from 1 | raw body, stored per index (`0x089976C4`). The game asks for the next index until a reply is **empty**. Ours: empty |
| `0x18` | after the notices; meaning unknown | raw body. A packet here caused a null read. Ours: empty |
| `0x14` | server load rates (`ServerNumRate`, 5 values) | sub `0x14`. Ours: zeros |

After `0x14` the game enters `MODE_INFRA_SERVER_SELECT` and starts PSN room matching (`sceNpMatching2Init`, then `CreateContext`). This is where it stops today.

## Reply table (com `0x1F`)

| Sub | Handler | Name |
| --- | --- | --- |
| `0x01` | `08A62F5C` | RcvInfraLogin. Payload: +12 u32 user number, +16..+35 session words, +40 u64, +48..+69 settings, +72 u32 event flags (`PUYO_SEGA_%02d`) |
| `0x02` | `08A63308` | RcvInfraRenewSession |
| `0x03` | `08A63394` | RcvInfraAlive |
| `0x04` / `0x05` | `08A63410` / `08A635A8` | RcvInfraFriends / FriendsNpId |
| `0x06`–`0x09` | | RcvInfraBlacklist, BlacklistInv, BlacklistAdd, BlacklistRemove |
| `0x0A` | `08A63704` | RcvInfraMailSend |
| `0x0D` | `08A631D0` | RcvInfraLogout |
| `0x0E` | `08A63274` | RcvInfraPatchFile |
| `0x11`–`0x13` | | RcvInfraWebEvent |
| `0x14` | `08A63C2C` | server load rates |
| `0x15` / `0x85` | `08A63674` | RcvInfraNewMail |
| `0x16` / `0x84` | `08A6349C` | RcvInfraFriendOnlineStatus |
| `0x1A`–`0x1C` | | RcvInfraPCardBaseSend, PCardCommSend, PCardRecvReq (partner cards) |
| `0x1E` | `08A63430` | RcvInfraVrCodes |
| `0x81` | `08A63A1C` | RcvInfraServerStatus (calls SetServerInfo `0x08A6ADEC`) |
| `0x82`, `0x83`, `0x0B`, `0x0C`, `0x86` | | RcvInfraSocialized (`0x86` holds 7 words at +12) |

## PSN pieces answered offline

These are in [psprecomp/src/net/np_mmo.c](../psprecomp/src/net/np_mmo.c) and `adhoc_mmo.c`:
- the PSN sign-in dialog;
- `sceNpGetNpId`: the player name as the online ID;
- content rating and chat restriction: none;
- `sceNpAuth`: a ticket whose callback is delivered on the next frame.

## Debugging

- `PSP2I_GAMELOG=1` logs the game's debug printf (`game:` lines). It narrates every step.
- `PSP2I_NETTRACE=1` logs request bodies and stub calls.
