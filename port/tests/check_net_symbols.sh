#!/bin/sh
# Static check: every network-related firmware import of the game is answered
# by the psprecomp network HLE layer, and no network NID is registered outside it.
#
#   port/tests/check_net_symbols.sh [port/gen/recomp_imports.c]
#
# "Network-related" = the libraries sceNet*, sceWlanDrv, sceNp*, sceHttp,
# sceSsl, plus the sceUtility Netconf / PSN dialog NIDs. The network layer is
# the files listed in NET_FILES. Exit 0 when nothing is unmapped.
set -eu
cd "$(dirname "$0")/../.."
IMPORTS=${1:-port/gen/recomp_imports.c}
HLE=psprecomp/src/hle
NET_FILES="$HLE/net.c $HLE/net_sock.c $HLE/offline.c $HLE/http.c"
[ -d psprecomp/src/net ] && NET_FILES="$NET_FILES $(ls psprecomp/src/net/*.c 2>/dev/null)"
[ -f "$IMPORTS" ] || { echo "missing $IMPORTS: generate port/gen first" >&2; exit 2; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
NETLIB='^(sceNet[A-Za-z]*|sceWlanDrv|sceNp[A-Za-z0-9]*|sceHttp|sceSsl)$'

# Every 0xNNNNNNNN NID that appears in a network-layer file (registration
# calls and stub tables alike), upper-cased.
grep -ohE '0x[0-9A-Fa-f]{8}' $NET_FILES | tr a-f A-F | sed 's/^0X/0x/' | sort -u > "$tmp/net_nids"
# sceUtility NIDs the layer owns (Netconf, PSN dialog).
grep -hE '"sceUtility(Netconf|Psn)' $NET_FILES | grep -oE '0x[0-9A-Fa-f]{8}' | tr a-f A-F | sed 's/^0X/0x/' | sort -u > "$tmp/util_nids"

# The game's imports: "lib nid".
grep -oE '^/\* [A-Za-z0-9_]+ :: NID 0x[0-9A-Fa-f]{8}' "$IMPORTS" | awk '{print $2, toupper($5)}' | sed 's/ 0X/ 0x/' > "$tmp/imports"

total=0; unmapped=0
while read -r lib nid; do
    if echo "$lib" | grep -qE "$NETLIB"; then :
    elif [ "$lib" = sceUtility ] && grep -qx "$nid" "$tmp/util_nids"; then :
    else continue; fi
    total=$((total + 1))
    if ! grep -qx "$nid" "$tmp/net_nids"; then
        echo "UNMAPPED  $lib  $nid"; unmapped=$((unmapped + 1))
    fi
done < "$tmp/imports"

# Network registrations outside the layer (another file claiming a net NID).
stray=$(grep -rlE 'psp_hle_register[a-z_]*\(0x[0-9A-Fa-f]+, *"(sceNet|sceWlanDrv|sceNp|sceHttp|sceSsl)' psprecomp/src port/host 2>/dev/null \
        | grep -vxF "$(echo $NET_FILES | tr ' ' '\n')" || true)
[ -n "$stray" ] && { echo "network NIDs registered outside the network layer:"; echo "$stray"; unmapped=$((unmapped + 1)); }

echo "network imports: $total, unmapped: $unmapped"
[ "$unmapped" -eq 0 ]
