#!/usr/bin/env python3
"""LAN discovery check for psp2i_server (Task 2.1).

Broadcasts MSG_LAN_PROBE on UDP 29000 (or --port) and prints every server
that answers: name, address, game port, players, version hash. Exits 0 when at
least one valid reply came back (and, with --expect-*, matched), else 1.

    python3 server/tools/lan_probe.py
    python3 server/tools/lan_probe.py --expect-name "My Server" --expect-players 0

The wire format is psprecomp/include/psprecomp/net_proto.h: little-endian,
header = magic "P2IM" u32, type u16, version u16.
"""
import argparse
import os
import socket
import struct
import sys
import time

MAGIC = 0x4D493250
PROTO_VERSION = 1
MSG_LAN_PROBE = 0x0002
MSG_LAN_REPLY = 0x8002
VERSION_STRING = b"psp2i-mmo/1"   # PSP_MMO_VERSION_STRING
REPLY = struct.Struct("<IHH I HHH I 32s")   # hdr, nonce, game port, players, max, hash, name


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=29000, help="discovery port (default 29000)")
    ap.add_argument("--timeout", type=float, default=1.5, help="seconds to wait for replies")
    ap.add_argument("--target", default="255.255.255.255", help="where to send the probe (default: broadcast)")
    ap.add_argument("--expect-name")
    ap.add_argument("--expect-players", type=int)
    args = ap.parse_args()

    nonce = struct.unpack("<I", os.urandom(4))[0]
    probe = struct.pack("<IHHI", MAGIC, MSG_LAN_PROBE, PROTO_VERSION, nonce)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", 0))
    s.sendto(probe, (args.target, args.port))
    if args.target == "255.255.255.255":
        s.sendto(probe, ("127.0.0.1", args.port))   # a server on this machine

    want_hash = fnv1a(VERSION_STRING)
    seen, ok = set(), False
    end = time.monotonic() + args.timeout
    while (left := end - time.monotonic()) > 0:
        s.settimeout(left)
        try:
            data, (ip, _) = s.recvfrom(256)
        except socket.timeout:
            break
        if len(data) != REPLY.size:
            continue
        magic, typ, ver, n, gport, players, maxp, vhash, name = REPLY.unpack(data)
        if magic != MAGIC or typ != MSG_LAN_REPLY or ver != PROTO_VERSION or n != nonce:
            continue
        name = name.split(b"\0", 1)[0].decode("utf-8", "replace")
        if (name, gport) in seen:
            continue
        seen.add((name, gport))
        match = vhash == want_hash
        print(f"{name!r} at {ip}:{gport}  players {players}/{maxp}  version hash {vhash:08X}"
              f" ({'matches' if match else 'DIFFERENT from'} {VERSION_STRING.decode()} {want_hash:08X})")
        good = match
        if args.expect_name is not None and name != args.expect_name:
            print(f"  expected name {args.expect_name!r}")
            good = False
        if args.expect_players is not None and players != args.expect_players:
            print(f"  expected {args.expect_players} players")
            good = False
        ok = ok or good
    if not seen:
        print(f"no server answered on UDP {args.port} within {args.timeout:.1f} s")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
