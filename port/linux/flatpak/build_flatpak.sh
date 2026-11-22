#!/bin/bash
# Build and install the PSP2i Flatpak (Steam Deck desktop mode, or any Linux).
#
#   port/linux/flatpak/build_flatpak.sh            build + install for this user
#   port/linux/flatpak/build_flatpak.sh --bundle   also write psp2i.flatpak
#
# Needs flatpak. flatpak-builder is used if installed, else the
# org.flatpak.Builder app from Flathub (installed on first use). The game
# code (build/gen) is generated from EBOOT.BIN in the repository root first,
# with a native build of the recompiler (needs cmake, ninja and a C compiler;
# on the Deck those are easiest inside a distrobox/toolbox, or run this step
# on another machine and copy build/gen over).
set -euo pipefail
cd "$(dirname "$0")/../../.."
APP=io.github.zmbkilla.PSP2iRecomp
MANIFEST=port/linux/flatpak/$APP.yml

if [[ ! -f build/gen/recomp_funcs.c || EBOOT.BIN -nt build/gen/recomp_funcs.c ]]; then
    [[ -f EBOOT.BIN ]] || { echo "Put your decrypted EBOOT.BIN in $(pwd) first." >&2; exit 1; }
    cmake -S psprecomp -B build/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/recomp --target allegrexrecomp
    mkdir -p build/gen
    build/recomp/tools/allegrexrecomp/allegrexrecomp emit EBOOT.BIN build/gen recomp \
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
    --state-dir=build/flatpak/state \
    build/flatpak/build-dir "$MANIFEST"

if [[ "${1:-}" == --bundle ]]; then
    "${FB[@]}" --user --force-clean --repo=build/flatpak/repo \
        --state-dir=build/flatpak/state \
        build/flatpak/build-dir "$MANIFEST"
    flatpak build-bundle build/flatpak/repo build/psp2i.flatpak "$APP"
    echo "Wrote build/psp2i.flatpak"
fi
echo "Installed. Run: flatpak run $APP   (game files: ~/.var/app/$APP/data/psp2i)"
