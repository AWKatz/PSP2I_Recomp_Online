#!/bin/zsh
set -e
cd "${0:A:h}"
export PSP2I_DATA="$PWD"
exec ./build/macos/psp2i
