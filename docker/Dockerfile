# psp2i_server in a container (Synology Container Manager, or any Docker host).
# Build context: the repository root (the docker-compose.yml there sets it). Only
# psprecomp/ and server/ are used; .dockerignore keeps the rest (game files,
# builds) out of the build.

# ---- build: a static binary. ENet comes from docker/third_party/enet (the version psprecomp pins),
#      so nothing is downloaded from GitHub: CMake runs fully disconnected. ----
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base cmake ninja linux-headers
WORKDIR /src
COPY psprecomp psprecomp
COPY server server
COPY docker/third_party/enet enet
# The source folders, not the game folder's server/ (which holds only the built program and config/)
RUN for f in server/CMakeLists.txt server/src/main.c psprecomp/CMakeLists.txt psprecomp/src/net/transport.c; do \
        [ -f "$f" ] || { echo "Missing $f: copy the repository's own psprecomp/ and server/ folders (see docker/README.md)."; \
                         echo "server/ holds:"; ls -la server; echo "psprecomp/ holds:"; ls -la psprecomp; exit 1; }; \
    done
RUN cmake -S server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSP2I_SERVER_STATIC=ON \
          -DFETCHCONTENT_SOURCE_DIR_ENET=/src/enet -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
 && cmake --build /build --target psp2i_server \
 && strip /build/psp2i_server

# ---- run ----
FROM alpine:3.20
RUN apk add --no-cache su-exec tzdata
COPY --from=build /build/psp2i_server /usr/local/bin/psp2i_server
COPY server/config /usr/local/share/psp2i_server/config
# Only the config files: a NAS's own folders (Synology's @eaDir) out, whatever the build context held
RUN find /usr/local/share/psp2i_server/config -mindepth 1 ! -type f -prune -exec rm -rf {} + \
 && ls /usr/local/share/psp2i_server/config
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 755 /usr/local/bin/entrypoint.sh

# /data: the database (psp2i_server.db) and config/ (drop tables, rates, Terms of Use, Notices)
VOLUME /data
# 29001/udp the game; 29000/udp LAN discovery (--lan); 8080/tcp the web admin panel
EXPOSE 29001/udp 29000/udp 8080/tcp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["--lan", "--web-port", "8080", "--web-bind", "0.0.0.0"]
