#!/bin/sh
# psp2i_server's container start-up:
#   - /data/config gets the default config files that are not there yet (edited ones are kept);
#   - PUID / PGID (optional): the server runs as that user and owns /data (Synology: your DSM user's ids);
#   - PSP2I_ADMIN_USER / PSP2I_ADMIN_PASSWORD (optional): that admin account is created if it does
#     not exist yet (a password of at least 8 characters), for signing in to the web panel;
#   - then the server, from /data, with the container's arguments (compose "command"), console off.
set -e

echo "psp2i-server container (entrypoint 2026-11-28b): starting"

DATA=/data
DEFAULTS=/usr/local/share/psp2i_server/config

mkdir -p "$DATA/config"
for f in "$DEFAULTS"/*; do
    [ -f "$f" ] || continue        # files only: a NAS's own folders (Synology's @eaDir) are not config
    name=$(basename "$f")
    [ -e "$DATA/config/$name" ] || cp "$f" "$DATA/config/$name"
done

RUN=""
if [ -n "$PUID" ] && [ "$PUID" != "0" ]; then
    chown -R "$PUID:${PGID:-$PUID}" "$DATA"
    RUN="su-exec $PUID:${PGID:-$PUID}"
fi

cd "$DATA"

if [ -n "$PSP2I_ADMIN_USER" ] && [ -n "$PSP2I_ADMIN_PASSWORD" ]; then
    # "already exists" after the first start: not an error here
    $RUN psp2i_server --add-admin "$PSP2I_ADMIN_USER" "$PSP2I_ADMIN_PASSWORD" || true
fi

exec $RUN psp2i_server --no-console "$@"
