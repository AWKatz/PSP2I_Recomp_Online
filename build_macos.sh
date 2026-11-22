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
# ./build_macos.sh         the game (single player):           build/macos/psp2i
# ./build_macos.sh --mmo   the MMO client and the server:      build/macos-mmo/psp2i, build/macos-mmo/server/psp2i_server
OUT=build/macos
EXTRA=()
if [[ "${1:-}" == --mmo ]]; then
    OUT=build/macos-mmo
    EXTRA=(-DENABLE_MMO_NET=ON -DBUILD_DEDICATED_SERVER=ON)
fi
cmake -S port/macos -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)" "${EXTRA[@]}"
cmake --build $OUT -j 4
echo "Built $OUT/psp2i$([[ -f $OUT/server/psp2i_server ]] && echo " and $OUT/server/psp2i_server")"
