#!/bin/bash
# Build and install the PSP2i Flatpak (Steam Deck desktop mode, or any Linux).
#
#   port/linux/flatpak/build_flatpak.sh            build + install for this user
#   port/linux/flatpak/build_flatpak.sh --bundle   also write psp2i.flatpak
#
# Needs flatpak. flatpak-builder is used if installed, else the
# org.flatpak.Builder app from Flathub (installed on first use). The game
# code (port/gen) is generated from EBOOT.BIN in the repository root first,
# with a native build of the recompiler (needs cmake, ninja and a C compiler;
# on the Deck those are easiest inside a distrobox/toolbox, or run this step
# on another machine and copy port/gen over).
set -euo pipefail
cd "$(dirname "$0")/../../.."
APP=io.github.zmbkilla.PSP2iRecomp
MANIFEST=port/linux/flatpak/$APP.yml

if [[ ! -f port/gen/recomp_funcs.c || EBOOT.BIN -nt port/gen/recomp_funcs.c ]]; then
    [[ -f EBOOT.BIN ]] || { echo "Put your decrypted EBOOT.BIN in $(pwd) first." >&2; exit 1; }
    git submodule update --init --recursive
    cmake -S psprecomp -B psprecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build psprecomp/build --target allegrexrecomp
    mkdir -p port/gen
    psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit EBOOT.BIN port/gen recomp \
        --hooks port/hooks.txt --fixes port/fixes.txt --switches port/switches.txt
fi

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
if command -v flatpak-builder >/dev/null; then
    FB=(flatpak-builder)
else
    flatpak install --user -y flathub org.flatpak.Builder
    FB=(flatpak run org.flatpak.Builder)
fi
"${FB[@]}" --user --install --force-clean --install-deps-from=flathub \
    --state-dir=port/linux/flatpak/.flatpak-builder \
    port/linux/flatpak/build-dir "$MANIFEST"

if [[ "${1:-}" == --bundle ]]; then
    "${FB[@]}" --user --force-clean --repo=port/linux/flatpak/repo \
        --state-dir=port/linux/flatpak/.flatpak-builder \
        port/linux/flatpak/build-dir "$MANIFEST"
    flatpak build-bundle port/linux/flatpak/repo psp2i.flatpak "$APP"
    echo "Wrote psp2i.flatpak"
fi
echo "Installed. Run: flatpak run $APP   (game files: ~/.var/app/$APP/data/psp2i)"
