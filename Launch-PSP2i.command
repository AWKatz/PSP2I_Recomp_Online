#!/bin/zsh
# Runs PSP2i with this folder as the game folder: the build from ./build_macos.sh
# (or, if that is the only one, ./build_macos.sh --offline).
set -e
cd "${0:A:h}"
export PSP2I_DATA="$PWD"
GAME=build/macos/psp2i
[[ -x $GAME ]] || GAME=build/macos-offline/psp2i
[[ -x $GAME ]] || { echo "Build the game first: ./build_macos.sh"; exit 1; }
exec ./$GAME
