#!/bin/zsh
set -euo pipefail
cd "${0:A:h}"
cmake -S psprecomp -B build/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recomp --target allegrexrecomp -j 4
mkdir -p build/gen
G=build/gen/recomp_funcs.c
if [[ ! -f $G || EBOOT.BIN -nt $G || port/hooks.txt -nt $G || port/fixes.txt -nt $G || port/switches.txt -nt $G ]]; then
    build/recomp/tools/allegrexrecomp/allegrexrecomp emit EBOOT.BIN build/gen recomp --hooks port/hooks.txt --fixes port/fixes.txt --switches port/switches.txt
fi
# ./build_macos.sh             the game with online play, and the server: build/macos/psp2i, build/macos/server/psp2i_server
# ./build_macos.sh --offline   the game without online play:              build/macos-offline/psp2i
OUT=build/macos
EXTRA=(-DENABLE_MMO_NET=ON -DBUILD_DEDICATED_SERVER=ON)
if [[ "${1:-}" == --offline ]]; then
    OUT=build/macos-offline
    EXTRA=(-DENABLE_MMO_NET=OFF -DBUILD_DEDICATED_SERVER=OFF)
fi
cmake -S port/macos -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)" "${EXTRA[@]}"
cmake --build $OUT -j 4
echo "Built $OUT/psp2i$([[ -f $OUT/server/psp2i_server ]] && echo " and $OUT/server/psp2i_server")"
