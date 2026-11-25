#!/bin/zsh
# Runs PSP2i from its game folder, build/PSP2i_macOS (made by ./build_macos.sh).
set -e
cd "${0:A:h}/build/PSP2i_macOS" 2>/dev/null || { echo "Build the game first: ./build_macos.sh"; exit 1; }
export PSP2I_DATA="$PWD"
exec ./psp2i
