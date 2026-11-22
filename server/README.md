# psp2i_server

`psp2i_server` is the PSP2i dedicated server. It runs headless: no game runtime, rendering, audio or SDL. It links only `psprecomp_net`, which provides the ENet transport, the control protocol, LAN discovery and the config reader. ENet is compiled into the binary, so the server has no runtime dependencies beyond the system C library.

## Build

```sh
cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build server/build
```

You can also build it alongside the game: pass `-DBUILD_DEDICATED_SERVER=ON` to the `port/macos`, `port/linux` or `port` (Windows) build.

On Linux, `-DPSP2I_SERVER_STATIC=ON` produces a fully static binary you can copy to a VPS.

## Run

```sh
psp2i_server --lan                       # LAN party: clients find it by themselves
psp2i_server --online --port 29001       # a public server
psp2i_server --help
```

| Mode | Binds | Discovery | Rate limit | Before the handshake |
| --- | --- | --- | --- | --- |
| `--lan` | 0.0.0.0 (or `--bind`) | answers probes on UDP 29000 | none | other traffic is ignored |
| `--online` | 0.0.0.0 (or `--bind`) | off | `--rate-limit` packets/s per player (default 120, bursts up to 2x) | other traffic gets the peer disconnected |

Rules that apply in both modes:

- **Handshake checks.** Every connection must send a valid `MSG_C2S_HANDSHAKE` within `--handshake-timeout` (default 5 s). A valid handshake has:
  - the right protocol version;
  - a printable UTF-8 name;
  - a unicast MAC address that isn't already in a session.

  A handshake that fails is answered with an error status, and the peer is then disconnected.
- **Relaying.** Game traffic is relayed only between players who have passed the handshake.
- **Rate-limit kicks** (online mode). A player who sends over the limit for 3 seconds in a row is disconnected.
- **Disconnect reasons.** Whenever the server disconnects a client, it sends a reason code (`PSP_MMO_DISC_*` in `net_proto.h`).

The tick runs at a fixed 30 Hz (33.33 ms): read the network, run session upkeep, answer LAN probes, then sleep to the next deadline.

Every `--stats` seconds the server logs a status line: tick rate, tick work time, CPU, RSS and packet counts. It logs a summary on exit as well. Ctrl-C or SIGTERM shuts the server down and tells every client (`DISC_SHUTDOWN`).

## Clients

With `lan_discovery = true` and no `server_ip` in `client_config.toml`, an MMO-build client broadcasts a probe. It then joins the first server that answers with the same version hash (`PSP_MMO_VERSION_STRING`).

## Tests

- `cmake --build server/build --target test_server && server/build/test_server` runs the server on its own thread at 30 Hz against real ENet clients. It covers the handshake checks, LAN discovery, relay gating, and the timeout, protocol, rate-limit and shutdown disconnects.
- `python3 server/tools/lan_probe.py [--expect-name NAME] [--expect-players N]` broadcasts a probe and checks the replies against a running server.

## Measured (macOS, Apple Silicon, release build, 0 clients, 65 s)

| | Target | Measured |
| --- | --- | --- |
| Memory (RSS) | < 50 MB | 1.8 MB |
| CPU (one core) | < 2% | 0.13% (0.0–0.1% in `ps`) |
| Tick rate | 30 Hz | 30.01 Hz, 0 late ticks; each tick's work averages 0.026 ms, max 0.111 ms |

The Linux and Windows builds have not been measured yet.
