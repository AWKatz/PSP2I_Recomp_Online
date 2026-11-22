#!/bin/sh
# Flatpak entry point: the game's files live in the app's data folder,
# ~/.var/app/<id>/data/psp2i, unless PSP2I_DATA names another.
: "${PSP2I_DATA:=${XDG_DATA_HOME:-$HOME/.local/share}/psp2i}"
export PSP2I_DATA
mkdir -p "$PSP2I_DATA/GameData/flash/font" "$PSP2I_DATA/GameData/ms/PSP/SAVEDATA"
cd "$PSP2I_DATA" || exit 1
exec /app/bin/psp2i "$@"
