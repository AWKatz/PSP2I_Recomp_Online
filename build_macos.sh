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
# ./build_macos.sh             the game with online play, and the server
# ./build_macos.sh --offline   the game without online play
# Everything needed to play ends up in build/PSP2i_macOS/; put your EBOOT.BIN, game .iso
# and NPJH50332/ save folder in the root of that folder (see README.md).
OUT=build/macos
EXTRA=(-DENABLE_MMO_NET=ON -DBUILD_DEDICATED_SERVER=ON)
if [[ "${1:-}" == --offline ]]; then
    OUT=build/macos-offline
    EXTRA=(-DENABLE_MMO_NET=OFF -DBUILD_DEDICATED_SERVER=OFF)
fi
cmake -S port/macos -B $OUT -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)" "${EXTRA[@]}"
cmake --build $OUT -j 4

# The game folder: the programs; the server and its config/ (Terms of Use, Notices, rates, drop
# tables: defaults copied once, edited ones kept).
DIST=build/PSP2i_macOS
mkdir -p $DIST
cp $OUT/psp2i $OUT/libpsp2i_atrac.dylib $DIST/
[[ -f EBOOT.BIN && ! -f $DIST/EBOOT.BIN ]] && cp EBOOT.BIN $DIST/
if [[ -f $OUT/server/psp2i_server ]]; then
    mkdir -p $DIST/server/config
    cp $OUT/server/psp2i_server $DIST/server/
    for f in server/config/*; do [[ -f $DIST/server/config/${f:t} ]] || cp $f $DIST/server/config/; done
fi
echo "Built $DIST: run $DIST/psp2i$([[ -f $DIST/server/psp2i_server ]] && echo " (server: cd $DIST/server && ./psp2i_server)")"
