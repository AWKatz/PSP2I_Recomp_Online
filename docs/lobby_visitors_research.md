# Lobby visitor avatars (Task 3.2, route 2): research notes, stopped

Status: **stopped** (2026-11-23) in favour of the PSN lobby room route ([psn_lobby_plan.md](psn_lobby_plan.md)). The goal was to draw players from other parties in your Multi Mode lobby as extra characters.

## What works

- **Player factory** `0x08A47BD8(slot, character data, data 2, flag, t0)`. For your own slot online it builds the local player (2,416 bytes, constructor `0x08A2CA08`); otherwise a remote player (1,952 bytes, constructor `0x08A48A88`, vtable `0x08E34B00`). It returns an object id. The slot must be below 4.
- **Remote player position** at object +0x284 (x, height, z), with a copy at +0x278. An object that hasn't been placed sits at height −10000.
- **Session messages.** Header +0 length, +4/+5 type, +6 route, **+8 target slot**. The arrival message 0x0201 (788 bytes, handler `0x08B330B0`) creates the player; its slot is also at +36.
- **Replaying B's 0x0201 as slot 2** runs the full creation path: factory, constructor, model handle (`0x08A0818C`), model build `0x08A4B61C`. The party HUD then shows a third member.

## Where it stopped

- The copy is registered like a real remote player: the per-slot player array at `0x08ED53B4`, task lists, scene lists and a model object. But **it is never drawn**. Its model object has no transform and no body proportions, even after copying B's appearance block (+0x540, 0x80 bytes) and rebuilding with `0x08A4B61C`.
- Follow-up messages for a slot (0x0A05, 0x0233, 0x0208, 0x1D09, 0x0105) didn't make it appear. B's appearance probably comes from messages sent **before** its 0x0201 (0x0D02 character info, 0x0A08 items), which the recorder never captured.
- Party slots are 0–3, so this route would allow at most 2 extra characters in a 2-player party, and they'd appear in the party HUD.

## If it's picked up again

Record all session messages from the moment Multi Mode starts. Log what `0x08A4B61C` / `0x08A25138` read when B is built. Then build a copy directly from that data.
