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

The tick runs at a fixed 60 Hz by default (16.67 ms), matching the clients' 60 fps; `--tick-rate N` sets 10–120. Each tick reads the network, runs session upkeep and answers LAN probes, then waits on the network until the next deadline, so game traffic between players is relayed as soon as it arrives rather than once per tick.

Every `--stats` seconds the server logs a status line: tick rate, tick work time, CPU, RSS and packet counts. It logs a summary on exit as well. Ctrl-C or SIGTERM shuts the server down and tells every client (`DISC_SHUTDOWN`).

## Database

The server keeps its data in `--db PATH` (default `psp2i_server.db`), an embedded SQLite 3.54.1 database. It runs in WAL mode, so the game loop can write while the admin dashboard reads, with `synchronous=FULL` and foreign keys on. The schema is in `src/db.c` (`PRAGMA user_version` 1).

The tables pair plain columns with JSON:

- **`accounts`:** login names, PBKDF2-SHA256 password hashes (100,000 iterations, salted), admin flag, bans (reason and expiry), and the last IP.
- **`characters`:** plain columns for name, level, race, class and Meseta (never negative). JSON columns for appearance, inventory (always an array) and partner cards.
- **`audit_logs`:** every admin action, with JSON details.
- **`item_ledger`:** one row for every item the server has ever issued. Its `uid` is stamped into the item's JSON in the inventory.
- **`drops`:** items waiting on the field. The first claim wins.

Trades, Meseta transfers and drop claims each run as one `BEGIN IMMEDIATE` transaction (`db_trade`, `db_meseta_transfer`, `db_drop_claim`). A crash at any point leaves the database either before or after the whole move. Each item's `uid` makes that checkable: every ledger `uid` must appear in exactly one inventory.

## Admin: web panel, console, RCON

`--web-port N` serves the admin panel. It's bound to 127.0.0.1 unless you pass `--web-bind`, and it's compiled into the binary from `server/web/`. Sign-in needs an admin account: create one with `--add-admin NAME PASS` or `/account add NAME PASS admin`.

The REST API is under `/api/v1/` and every call needs `Authorization: Bearer <token>`. Endpoints:
- `login` (`POST`; returns the token) and `logout` (`POST`);
- `status` (`GET`); `stream` (`GET`, a Server-Sent Events feed of the status every second);
- `kick`, `ban`, `unban`, `admin`, `announce`, `reload` (`POST`);
- `rates` (`GET` / `POST`); `events` (`POST`); `drop_tables` (`GET` / `POST`, validated before it's saved);
- `rcon` (`POST`, any console command);
- `audit?q=&limit=&offset=`, `accounts?q=`, `bans` (`GET`).

Security:
- Logins are throttled after 5 failures a minute per address.
- Tokens expire after 8 hours of inactivity.
- Responses carry a strict Content-Security-Policy.

The stdin console, RCON and the panel share one command layer (`src/admin.c`). Every change is written to `audit_logs` with its actor: `console`, `rcon:<admin>` or `web:<admin>`.

## Clients

With `lan_discovery = true` and no `server_ip` in `client_config.toml`, an MMO-build client broadcasts a probe. It then joins the first server that answers with the same version hash (`PSP_MMO_VERSION_STRING`).

## Tests

- `cmake --build server/build --target test_server && server/build/test_server` runs the server on its own thread against real ENet clients. It covers the handshake checks, LAN discovery, relay gating, the party directory and party warps, and the timeout, protocol, rate-limit and shutdown disconnects.
- `test_db` (POSIX only) checks the crypto and the database. A child process runs a trade or a claim and is SIGKILLed partway through, at every crash point and in 300 random rounds. The test then reopens the database and checks that every item exists exactly once, total Meseta is unchanged, and `integrity_check` passes.
- `test_admin` covers the panel and its API end to end: the page, logins and throttling, rates set through the API changing a real client's drops, a ban disconnecting a client within 1 s and refusing it on reconnect, announcements, events, the drop-table editor, the audit log, RCON, the event stream and the console.
- `python3 server/tools/lan_probe.py [--expect-name NAME] [--expect-players N]` broadcasts a probe and checks the replies against a running server.

## Measured (macOS, Apple Silicon, release build, 0 clients, 65 s)

| | Target | Measured |
| --- | --- | --- |
| Memory (RSS) | < 50 MB | 1.8 MB (3.2 MB with the database open) |
| CPU (one core) | < 2% | 0.13% (0.0–0.1% in `ps`) |
| Tick rate | 60 Hz | 59.9–60.5 Hz, 0 late ticks; each tick's work averages 0.017–0.019 ms, max 0.074 ms; CPU 0.2% (Apple M4, idle server) |

The Linux and Windows builds have not been measured yet.
