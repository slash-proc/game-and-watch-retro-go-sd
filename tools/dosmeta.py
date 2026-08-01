#!/usr/bin/env python3
"""The .dosmeta sidecar: a title's measured COW-pool high-water mark.

This is the WRITER for the format external/8086tiny/dos_meta.h defines and
external/8086tiny/dos_meta.c parses. Read that header first -- it carries the
argument for why the number exists and, more importantly, why it is a
RESERVATION rather than a limit, so that a wrong number can never lose a guest
store.

The format is 16 little-endian 32-bit words, 64 bytes, CRC32 of the first 60 in
the last. Byte-wise on purpose: the reader is freestanding C and the writer is
Python, and a C struct layout is not a wire format.

    dosmeta.py write roms/dos/KEEN4.dsk --pages-cold 83
    dosmeta.py show  roms/dos/KEEN4.dosmeta

Where the numbers come from: external/8086tiny/test286/poolmeasure.sh, which
runs each image in the host harness (the same emulator the device runs) and
reports the pool pages it actually needed. Run it with --emit to write every
sidecar in one pass.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = 0x54454D44          # "DMET"
VERSION = 1
# Must equal DOS_META_ABI in external/8086tiny/dos_meta.h. Bump BOTH whenever a
# change can move a title's dirty high-water mark -- the granule, the demand
# region, the write-bit site selection. A skew makes every sidecar ignored and
# the pool falls back to the full array, which is the safe direction.
ABI = 1
BYTES = 64
CRC_SPAN = 65536            # DOS_META_CRC_SPAN

_FIELDS = ("magic", "version", "abi", "dsk_size", "dsk_crc", "gran",
           "demand_base", "demand_top", "pages_cold", "pages_xip",
           "measured_at", "r0", "r1", "r2", "r3", "hdr_crc")


def dsk_key(path: Path) -> tuple[int, int]:
    """(size, crc32 of the first CRC_SPAN bytes) -- the sidecar's cache key.

    Bounded because the device checks it during boot and BATTLECHESS.dsk is
    66 MB. It is a cache key, not a security boundary: all it has to catch is
    "this sidecar was measured against a different build of this image", and a
    regenerated .dsk changes its boot sector and its FAT, both of which are in
    the first 64 KB.
    """
    data = path.open("rb").read(CRC_SPAN)
    return path.stat().st_size, zlib.crc32(data) & 0xFFFFFFFF


def build(dsk_size: int, dsk_crc: int, pages_cold: int, pages_xip: int = 0,
          gran: int = 4096, demand_base: int = 0x10000,
          demand_top: int = 0xA0000, measured_at: int = 0) -> bytes:
    words = [MAGIC, VERSION, ABI, dsk_size, dsk_crc, gran,
             demand_base, demand_top, pages_cold, pages_xip, measured_at,
             0, 0, 0, 0]
    body = struct.pack("<15I", *words)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def parse(blob: bytes) -> dict:
    if len(blob) < BYTES:
        raise ValueError(f"short file: {len(blob)} bytes, want {BYTES}")
    words = struct.unpack("<16I", blob[:BYTES])
    m = dict(zip(_FIELDS, words))
    if m["magic"] != MAGIC:
        raise ValueError("not a .dosmeta (bad magic)")
    if m["hdr_crc"] != zlib.crc32(blob[:60]) & 0xFFFFFFFF:
        raise ValueError("header CRC mismatch")
    return m


def sidecar_path(dsk: Path) -> Path:
    return dsk.with_suffix(".dosmeta")


def write_for(dsk: Path, pages_cold: int, pages_xip: int = 0,
              demand_base: int = 0x10000, demand_top: int = 0xA0000,
              measured_at: int = 0, gran: int = 4096) -> Path:
    size, crc = dsk_key(dsk)
    out = sidecar_path(dsk)
    out.write_bytes(build(size, crc, pages_cold, pages_xip, gran,
                          demand_base, demand_top, measured_at))
    return out


def _main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("write", help="write <image>.dosmeta next to an image")
    w.add_argument("image", type=Path)
    w.add_argument("--pages-cold", type=int, required=True,
                   help="pool pages the title needed with no .xipimg armed")
    w.add_argument("--pages-xip", type=int, default=0,
                   help="... with its snapshot armed (0 = not measured)")
    w.add_argument("--demand-base", type=lambda s: int(s, 0), default=0x10000)
    w.add_argument("--demand-top", type=lambda s: int(s, 0), default=0xA0000)
    w.add_argument("--measured-at", type=lambda s: int(s, 0), default=0,
                   help="guest instructions retired at measurement (diagnostic)")

    s = sub.add_parser("show", help="decode a .dosmeta")
    s.add_argument("meta", type=Path)

    args = ap.parse_args()
    if args.cmd == "write":
        if not args.image.is_file():
            print(f"error: {args.image} does not exist", file=sys.stderr)
            return 1
        out = write_for(args.image, args.pages_cold, args.pages_xip,
                        args.demand_base, args.demand_top, args.measured_at)
        print(f"  {out.name}: pages_cold={args.pages_cold} "
              f"pages_xip={args.pages_xip}")
        return 0

    m = parse(args.meta.read_bytes())
    for k in _FIELDS:
        print(f"  {k:12s} {m[k]:#x} ({m[k]})")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
