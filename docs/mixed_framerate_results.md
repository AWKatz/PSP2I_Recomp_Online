# Task 4.1: mixed frame rate play (30 fps and 60 fps in one party)

Measured 2026-11-23 with two clients in the Clad 6 lobby (party through the party finder), 4× render, HD pack, server at 60 Hz.

## Method

`PSP2I_TEST_INPUT` commands drive both clients: `sticku2000` holds the stick up for 2 s, and `watch<hex>` logs a float triple to `logs/watch.txt` 20 times a second. Each client logs the walking player's position, the walker from its own copy and the other client from its copy of that player.

| Address | What it holds |
| --- | --- |
| `0x09002ED4` | your own player's position (x, height, z) |
| `0x0906AED4` | the leader's player, on a member's client |
| `0x0906A604` | the member's player, on the leader's client |

The last two are where one session put them; they can move between sessions. To find them again, search a `dump` for the other player's final position.

## Results

| Walker | Walker's own speed | Seen by the other client at | Final position |
| --- | --- | --- | --- |
| A at 30 fps | ~60 u/s (55/64 alternating*) | B at 60 fps: 64 u/s, steady | identical on both |
| A at 60 fps, three legs | 60 u/s, path 281 | B at 30 fps: 60 u/s, path 276 | identical on both |
| B at 30 fps, three legs | ~60 u/s, path 278 | A at 60 fps: 60–64 u/s, path 220 vs own 212 | identical on both |

\* A 30 fps player moves about 2.1 units per frame. A 0.25 s sample window holds 7 or 8 of those frames, so the per-window speed alternates.

- **Run speed is the same at 30 and 60 fps.** The game's own frame-rate handling (`framerate.c`) holds, and the network adds nothing on top.
- **Positions agree.** Every walk ended at exactly the same coordinates on both clients.
- **Remote movement is mostly smooth.** There was one hitch in about 25 s of walking: the remote copy stalled for 0.25 s (14 u/s), then caught up (91 u/s). The walker had slowed against an obstacle at that moment. Remote players aren't interpolated; position updates are applied as they arrive. If hitches show up in play, interpolation is the fix.
- **Traffic** is unchanged from the 60/60 measurements: nothing while idle, about 450 B/s while walking. A 30 fps client is no worse.
- **Jump arcs:** I don't believe the game has a jump button, so there were none to compare.

## Still to cover

- Combat at mixed rates: attack timing, enemy hits, and drops in a mission.
- 120 fps clients.
