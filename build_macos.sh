#!/bin/zsh
set -euo pipefail
cd "${0:A:h}"
# psprecomp now carries the Apple ARM64 fix (its commit ecc4521). A checkout
# where an earlier build applied the patch below as local changes: undo them,
# so the submodule update can move to that commit (the patch step then finds
# the fix already there). Only while the checked-out commit lacks the fix.
if ! git -C psprecomp grep -q _psp_fiber_switch HEAD -- src/hle/threadman.c 2>/dev/null &&
   git -C psprecomp apply --reverse --check "$PWD/port/macos/psprecomp-apple-arm64.patch" 2>/dev/null; then
    git -C psprecomp apply --reverse "$PWD/port/macos/psprecomp-apple-arm64.patch"
fi
git submodule update --init --recursive
patch_file="$PWD/port/macos/psprecomp-apple-arm64.patch"
if git -C psprecomp apply --check "$patch_file" 2>/dev/null; then
    git -C psprecomp apply "$patch_file"
elif ! git -C psprecomp apply --reverse --check "$patch_file" 2>/dev/null; then
    echo "The macOS runtime patch conflicts with local changes." >&2
    exit 1
fi
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
