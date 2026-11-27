#!/bin/bash
# Build PSP2i Recompiled as AppImages (Linux x86_64 / arm64, the Steam Deck included).
#
#   port/linux/appimage/build_appimage.sh             the game with online play, and the server
#   port/linux/appimage/build_appimage.sh --offline   the game without online play
#
# Writes the game folder build/PSP2i_Linux/: PSP2i-<arch>.AppImage (with --offline:
# PSP2i-offline-<arch>.AppImage) and server/psp2i_server-<arch>.AppImage. Put your
# EBOOT.BIN, game .iso and NPJH50332/ save folder in the root of build/PSP2i_Linux.
#
# Needs cmake, ninja, a C/C++ compiler, nasm (FFmpeg), curl, and the X11/Wayland/ALSA/
# PulseAudio development headers that SDL3 builds against. SDL3 and a minimal FFmpeg
# (H.264 decoding only) are built from source into build/appimage/deps, the same versions
# as the macOS build. AppImages run on distributions at least as new as the one they are
# built on: build on an older one (e.g. Ubuntu 22.04, or in a container) to reach more.
# The game itself (EBOOT.BIN, the ISO, fonts, saves) is not packaged.
set -euo pipefail
cd "$(dirname "$0")/../../.."
ROOT=$PWD
MMO=ON
[[ "${1:-}" == --offline ]] && MMO=OFF
ARCH=$(uname -m)
W=$ROOT/build/appimage
DEPS=$W/deps
mkdir -p "$W/src" "$DEPS"
export APPIMAGE_EXTRACT_AND_RUN=1            # run linuxdeploy without FUSE

# 1. The game code, generated from EBOOT.BIN.
if [[ ! -f build/gen/recomp_funcs.c || EBOOT.BIN -nt build/gen/recomp_funcs.c || port/hooks.txt -nt build/gen/recomp_funcs.c ]]; then
    [[ -f EBOOT.BIN ]] || { echo "Put your decrypted EBOOT.BIN in $ROOT first." >&2; exit 1; }
    cmake -S psprecomp -B build/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/recomp --target allegrexrecomp
    mkdir -p build/gen
    build/recomp/tools/allegrexrecomp/allegrexrecomp emit EBOOT.BIN build/gen recomp \
        --hooks port/hooks.txt --fixes port/fixes.txt --switches port/switches.txt
fi

fetch() {  # url sha256 file
    [[ -f "$W/src/$3" ]] || curl -fL -o "$W/src/$3" "$1"
    echo "$2  $W/src/$3" | sha256sum -c - >/dev/null
}

# 2. SDL3 and FFmpeg.
if [[ ! -f $DEPS/lib/libSDL3.so && ! -f $DEPS/lib64/libSDL3.so ]]; then
    fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz \
        f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f SDL3-3.4.20.tar.gz
    tar -xzf "$W/src/SDL3-3.4.20.tar.gz" -C "$W/src"
    cmake -S "$W/src/SDL3-3.4.20" -B "$W/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF
    cmake --build "$W/sdl-build" && cmake --install "$W/sdl-build"
fi
if [[ ! -f $DEPS/lib/libavcodec.so ]]; then
    fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
        8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
    tar -xJf "$W/src/ffmpeg-9.0.2.tar.xz" -C "$W/src"
    (cd "$W/src/ffmpeg-9.0.2" && ./configure --prefix="$DEPS" --disable-everything --disable-programs --disable-doc \
        --disable-static --enable-shared --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample \
        --enable-decoder=h264 && make -j"$(nproc)" && make install)
fi

# 3. The game (and, unless --offline, the server).
B=$W/build-$([[ $MMO == ON ]] && echo online || echo offline)
PKG_CONFIG_PATH=$DEPS/lib/pkgconfig cmake -S port/linux -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS" -DENABLE_MMO_NET=$MMO -DBUILD_DEDICATED_SERVER=$MMO
cmake --build "$B" --target psp2i $([[ $MMO == ON ]] && echo psp2i_server)

# 4. linuxdeploy packs each program with the libraries it needs.
LD=$W/linuxdeploy-$ARCH.AppImage
[[ -x $LD ]] || { curl -fL -o "$LD" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"; chmod +x "$LD"; }
export LD_LIBRARY_PATH=$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
A=port/linux/appimage
DIST=$ROOT/build/PSP2i_Linux
mkdir -p "$DIST/server"

pack() {  # name binary apprun desktop output [extra setup]
    local dir=$W/$1.AppDir
    rm -rf "$dir"
    mkdir -p "$dir/usr/bin"
    install -m755 "$2" "$dir/usr/bin/"
    [[ -n "${6:-}" ]] && "$6" "$dir"
    (cd "$W" && OUTPUT="$DIST/$5" LDAI_OUTPUT="$DIST/$5" "$LD" --appdir "$dir" -e "$dir/usr/bin/$(basename "$2")" \
        -d "$ROOT/$A/$4" -i "$ROOT/$A/psp2i.png" --custom-apprun "$ROOT/$A/$3" --output appimage)
    [[ -f "$DIST/$5" ]] || { echo "linuxdeploy did not write $DIST/$5" >&2; exit 1; }
    echo "Wrote $DIST/$5"
}
game_files() { install -m755 "$B/libpsp2i_atrac.so" "$1/usr/bin/"; }   # ATRAC music, loaded next to the game
server_files() { mkdir -p "$1/usr/share/psp2i_server/config"; cp server/config/* "$1/usr/share/psp2i_server/config/"; }

if [[ $MMO == ON ]]; then
    pack psp2i "$B/psp2i" AppRun-game psp2i.desktop "PSP2i-$ARCH.AppImage" game_files
    pack psp2i_server "$B/server/psp2i_server" AppRun-server psp2i_server.desktop "server/psp2i_server-$ARCH.AppImage" server_files
else
    pack psp2i-offline "$B/psp2i" AppRun-game psp2i.desktop "PSP2i-offline-$ARCH.AppImage" game_files
fi
# EBOOT.BIN, if it is not there yet.
[[ -f EBOOT.BIN && ! -f $DIST/EBOOT.BIN ]] && cp EBOOT.BIN "$DIST/"
echo "Game folder: $DIST"
