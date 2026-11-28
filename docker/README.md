# psp2i_server in Docker (Synology Container Manager)

This folder runs the PSP2i dedicated server (`psp2i_server`) as a container: on a Synology NAS with **Container Manager**, or on any machine with Docker. The image is built from this repository's source on the machine that runs it, so it suits any NAS CPU (Intel/AMD or ARM). It holds only the server: no game files.

| File | |
| --- | --- |
| `Dockerfile` | builds a static `psp2i_server` (Alpine Linux), about 10 MB |
| `third_party/enet/` | ENet, the server's network library (MIT), at the version the project pins, so the build downloads nothing from GitHub |
| `entrypoint.sh` | at start: default config files into `data/config/`, the first admin account, then the server |
| `../docker-compose.yml` | at the repository's top: the container (host network, `docker/data/` folder, the server's options) |

## What you get

- The game server, on **UDP 29001**.
- **LAN discovery** on UDP 29000: games on your network find the server by themselves.
- The **web admin panel** at `http://<NAS address>:8080/`.
- Everything the server keeps, in `docker/data/`: `psp2i_server.db` (accounts, characters, drops) and `config/` (`drop_tables.json`, `rates.toml`, `terms_of_use.txt`, `notices.txt`). The server reloads the drop tables and rates when you edit them. The Terms of Use and Notices are read each time a player joins.

## Setting it up on a Synology NAS

You need DSM 7.2 or later with **Container Manager** installed (Package Center). The NAS downloads Alpine Linux and its compilers during the first build, so it needs internet access. Nothing else is downloaded: ENet comes with this folder.

1. **Put the repository on the NAS.** The simplest way is `git clone`, which gives exactly the layout the build expects, and later updates are a `git pull`:
   - Install **Git Server** from Package Center (it provides the `git` command), and enable SSH (Control Panel > Terminal & SNMP).
   - SSH into the NAS and clone into a shared folder, for example:

     ```sh
     cd /volume1/docker
     git clone https://github.com/AWKatz/PSP2I_Recomp_Online.git psp2i
     ```

   Or copy the files over by hand: into a folder such as `docker/psp2i`, copy from the repository's top folder the `psprecomp/`, `server/` and `docker/` folders and the `docker-compose.yml` and `.dockerignore` files. Either way it should look like this:

   ```
   docker/psp2i/
     docker-compose.yml
     .dockerignore
     docker/       Dockerfile, entrypoint.sh, README.md, third_party/enet/
     psprecomp/
     server/
   ```

   These are the **repository's** `psprecomp/` and `server/` (the source code: `server/` holds `CMakeLists.txt`, `src/`, `web/`, `config/`, ...), not the `server/` folder inside your game folder (`build/PSP2i_macOS/server/`), which holds only the built program and its config. Don't copy your game files or `build/` folder: the image doesn't use them. (`.dockerignore` starts with a dot, so your Mac's Finder hides it: press Cmd+Shift+. to show it.)

2. **Set the admin password.** Open `docker/psp2i/docker-compose.yml` (File Station's Text Editor, or any editor) and change `PSP2I_ADMIN_PASSWORD` (at least 8 characters). You can also change `PSP2I_ADMIN_USER`, the server's name and the web panel's port (`--web-port`) here.

3. **Create the project.** In Container Manager, open **Project** and choose **Create**:
   - Project name: `psp2i`
   - Path: `docker/psp2i` (the folder holding `docker-compose.yml`)
   - Source: **Use existing docker-compose.yml**

   Click through to **Done**. Container Manager builds the image (a few minutes the first time) and starts the container.

4. **Check it's running.** Container Manager > **Container** > `psp2i-server` > **Log** should show lines like:

   ```
   'PSP2i Server': LAN mode, game port 29001 (udp), discovery port 29000 (udp), up to 12 players, ...
   web: admin panel at http://0.0.0.0:8080/ ...
   ```

5. **Open the web panel** at `http://<NAS address>:8080/` and sign in with the admin account from step 2. Then remove the two `PSP2I_ADMIN_*` lines from `docker-compose.yml` (the password is visible in the container's settings), and rebuild the project (Project > `psp2i` > **Action** > **Build**). The account stays in the database.

6. **If DSM's firewall is on** (Control Panel > Security > Firewall), allow UDP 29000–29001 and TCP 8080.

## Connecting the games

- **On the same network:** nothing to set. With no `server_ip` in `client_config.toml`, the game finds the NAS by LAN discovery. If another server is also running on your network (say, `psp2i_server` on your Mac), stop it, or the game may join that one.
- **Or name the NAS:** in `client_config.toml` next to the game, set `server_ip = "<NAS address>"`. Give the NAS a fixed address (DHCP reservation on your router) so this stays right.

## Players outside your home

1. In `docker-compose.yml`, change `--lan` to `--online` in `command`. Online mode has strict rate limits and accepts nothing before a client's handshake. LAN discovery is off in online mode.
2. On your router, forward **UDP 29001** to the NAS.
3. Players set `server_ip` to your public address, or a DNS name for it, in their `client_config.toml`.

**Don't forward the web panel's port.** It is plain HTTP. For the panel from outside, use a VPN to your home network, or DSM's reverse proxy with HTTPS: Control Panel > Login Portal > Advanced > Reverse Proxy, from `https://<a name>:<port>` to `http://localhost:8080`.

## Day to day

- **Logs:** Container > `psp2i-server` > Log.
- **Stop / start / restart:** Container Manager's buttons. A stop sends SIGTERM, and the server shuts down cleanly, finishing its database writes.
- **Server commands:** the console is off in the container. Use the web panel, or Container > `psp2i-server` > **Terminal** (Create > `sh`), for example: `cd /data && psp2i_server --add-admin NAME PASSWORD`.
- **Back up** the `docker/psp2i/docker/data` folder (Hyper Backup works). The database is SQLite in WAL mode: for a copy you can rely on, stop the container first, or include `psp2i_server.db-wal` and `-shm` with it.
- **Updating the server:** `git pull` in the cloned folder (or replace `psprecomp/`, `server/` and `docker/` with the new versions), then Project > `psp2i` > **Action** > **Build**. `docker/data/` is kept.
- **Your files' owner:** by default the server runs as root, so the files in `data/` belong to root. To have them belong to your DSM user, set `PUID` and `PGID` in `docker-compose.yml` to your user's ids (`id` over SSH; the first DSM user is usually 1026, group `users` is 100).

## If the build fails

**First, make sure the NAS runs the current version.** The log's first line names the startup script, for example `psp2i-server container (entrypoint 2026-11-28b): starting`. If it's missing or older, or an error from the list below persists after you updated, the container is still on an old image. Over SSH:

```sh
cd /volume1/docker/psp2i
git pull
git log -1 --oneline                                  # the newest commit on GitHub
sudo docker compose build --no-cache                  # a fresh image (older DSM: sudo docker-compose ...)
sudo docker compose up -d --force-recreate            # a new container on it
sudo docker logs --tail 20 psp2i-server
```

The project in Container Manager shows the same container afterwards.


- **`unable to prepare context: unable to evaluate symlinks in Dockerfile path: lstat /volume1/docker/...`**: Docker didn't find `docker/Dockerfile` in the project's folder. Check that the project's path is the folder holding `docker-compose.yml`, and that `docker/Dockerfile` sits next to it, as in the layout in step 1 (names are case-sensitive). The full path in the message shows where Docker looked.
- **`CMake Error: The source directory "/src/server" does not appear to contain CMakeLists.txt`**, or the build stops with "Missing server/CMakeLists.txt": the `server/` folder on the NAS isn't the source folder. It's most likely the game folder's `server/` (the built program and config only). Replace it with the repository's `server/` folder.
- **`Failed to clone repository: 'https://github.com/lsalzman/enet.git'`**: an older `docker/Dockerfile`, which still downloaded ENet. Copy the current `docker/` folder, including `docker/third_party/enet/`, and build again.
- **`cp: omitting directory '/usr/local/share/psp2i_server/config/@eaDir'`**, and the container stops: an old image. Synology's indexing service puts `@eaDir` folders in shared folders, and the first version copied them as config. Update and rebuild as above.
- **The container can't mount `docker/data` because it doesn't exist:** a copy from before the folder was part of the repository. Create the empty folder `docker/data` next to `docker/Dockerfile` (or `git pull`) and start again.
- **A failure in `apk add`**: the NAS couldn't reach Alpine's package mirrors. Check its internet access and DNS, then build again.

## Without Synology

Any Docker host with the Compose plugin:

```sh
# from the repository's top folder
docker compose up -d --build      # build and start
docker compose logs -f            # the log
docker compose down               # stop
```

`network_mode: host` only gives LAN discovery on Linux hosts. Docker Desktop on macOS and Windows runs containers in a VM, so there you'd use port mappings (`ports:` with `29001:29001/udp` and `8080:8080`) instead of the host network, and set `server_ip` in the games.
