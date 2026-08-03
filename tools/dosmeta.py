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

    dosmeta.py write roms/dos/KEEN4.dsk --pages-cold 149 --mach-kb 640
    dosmeta.py show  roms/dos/KEEN4.dosmeta

Where the numbers come from:

  --pages-cold  external/8086tiny/test286/poolmeasure.sh, which runs each image
                in the host harness (the same emulator the device runs) and
                reports the pool pages it actually needed. Run it with --emit to
                write every sidecar in one pass. NOTE that it injects no input,
                so its figure is a title-screen LOWER BOUND; the in-play figures
                are in external/8086tiny/docs/memory/25-demand-does-not-pay.md.

  --mach-kb     external/8086tiny/test286/machfloor.sh -- the BEHAVIOURAL floor,
                bisected by shrinking the machine until the title stops doing the
                work its timeline makes it do.

                Do NOT take it from the MCB chain. Every executable in roms/dos
                has e_maxalloc = 0xFFFF, so DOS hands each program all remaining
                conventional memory at load regardless of use, and the chain
                reports what DOS OFFERED rather than what the title needs.

                Do not take it from test286/machplay.sh either: its subject is
                built at the derived COW pool (146 pages) and KEEN 4 in play needs
                149, so KEEN 4's own reference run is exhausted and its page-count
                term compares two caps. machfloor.sh builds a pool that cannot
                saturate and refuses a title whose reference run lost a store.

Writing --mach-kb is what makes a sidecar version 2, and version 2 is what stops
external/8086tiny/dos_arena.c's dos_arena_open() refusing: with no machine size
there is no hole above the guest's conventional memory to carve a COW pool from,
so the pool gets 0 pages from the arena and per-title pool sizing stays inert.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = 0x54454D44          # "DMET"
# Version 2 adds mach_kb in what version 1 wrote as reserved[0] -- i.e. as a
# zero, which dos_meta_mach_kb() reads as "this title was never profiled". So a
# v1 sidecar is still accepted by a v2 reader. The reverse is not true: a v2
# sidecar handed to a v1 reader is rejected outright, so regenerate sidecars and
# update firmware together.
VERSION = 2
# Must equal DOS_META_ABI in external/8086tiny/dos_meta.h. Bump BOTH whenever a
# change can move a title's dirty high-water mark -- the granule, the demand
# region, the write-bit site selection. A skew makes every sidecar ignored and
# the pool falls back to the full array, which is the safe direction.
ABI = 1
BYTES = 64
CRC_SPAN = 65536            # DOS_META_CRC_SPAN

_FIELDS = ("magic", "version", "abi", "dsk_size", "dsk_crc", "gran",
           "demand_base", "demand_top", "pages_cold", "pages_xip",
           "measured_at", "mach_kb", "r1", "r2", "r3", "hdr_crc")


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
          demand_top: int = 0xA0000, measured_at: int = 0,
          mach_kb: int = 0) -> bytes:
    # mach_kb: conventional memory to give this title, in KB -- what INT 12h
    # returns. 0 = not profiled, keep 640 KB. Measured by
    # external/8086tiny/test286/machprofile.sh; the reader range-checks it to
    # [64, 640] and a multiple of 4, so an out-of-range value reads as 0.
    words = [MAGIC, VERSION, ABI, dsk_size, dsk_crc, gran,
             demand_base, demand_top, pages_cold, pages_xip, measured_at,
             mach_kb, 0, 0, 0]
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
              measured_at: int = 0, gran: int = 4096,
              mach_kb: int = 0) -> Path:
    size, crc = dsk_key(dsk)
    out = sidecar_path(dsk)
    out.write_bytes(build(size, crc, pages_cold, pages_xip, gran,
                          demand_base, demand_top, measured_at, mach_kb))
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
    w.add_argument("--gran", type=lambda s: int(s, 0), default=4096,
                   help="fold granule the measurement was taken at (DOS_FOLD_GRAN)")
    w.add_argument("--mach-kb", type=int, default=0,
                   help="conventional memory to give this title, in KB -- what "
                        "INT 12h returns. 0 = not profiled, keep 640 KB. This is "
                        "the field that makes the sidecar version 2 and funds the "
                        "COW arena; see the module docstring for where it comes "
                        "from. Must be 0, or in [64, 640] and a multiple of 4.")

    s = sub.add_parser("show", help="decode a .dosmeta")
    s.add_argument("meta", type=Path)

    args = ap.parse_args()
    if args.cmd == "write":
        if not args.image.is_file():
            print(f"error: {args.image} does not exist", file=sys.stderr)
            return 1
        # RANGE-CHECK mach_kb HERE, LOUDLY. dos_meta_mach_kb() applies exactly
        # this check and returns 0 when it fails, which the arena reads as "not
        # profiled" and degrades to today's behaviour -- correct, but silent. A
        # typo would then cost the title its pool with nothing to show for it,
        # and the only symptom would be an arena that still refuses. Fail at
        # write time instead, where there is someone to tell.
        if args.mach_kb and not (64 <= args.mach_kb <= 640
                                 and args.mach_kb % 4 == 0):
            print(f"error: --mach-kb {args.mach_kb} is out of range: must be 0, "
                  f"or in [64, 640] and a multiple of 4 (dos_meta.c reads any "
                  f"other value as 0, i.e. silently ignores it)", file=sys.stderr)
            return 1
        out = write_for(args.image, args.pages_cold, args.pages_xip,
                        args.demand_base, args.demand_top, args.measured_at,
                        gran=args.gran, mach_kb=args.mach_kb)
        print(f"  {out.name}: pages_cold={args.pages_cold} "
              f"pages_xip={args.pages_xip} mach_kb={args.mach_kb}"
              f"{'' if args.mach_kb else ' (v1 behaviour: arena will refuse)'}")
        return 0

    m = parse(args.meta.read_bytes())
    for k in _FIELDS:
        print(f"  {k:12s} {m[k]:#x} ({m[k]})")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
