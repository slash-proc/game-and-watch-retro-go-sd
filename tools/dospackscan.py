#!/usr/bin/env python3
"""Report which executables inside a DOS .dsk image are SELF-EXTRACTING.

    python3 tools/dospackscan.py                 # every roms/dos/*.dsk + fd.img
    python3 tools/dospackscan.py roms/dos/WOLF3D.dsk --all

WHY THIS EXISTS. This port maps guest memory granules straight at a NOR-flash
copy of the .dsk (dos_elide_set_image()), so a byte that came off disk unchanged
costs ZERO pool pages. A self-extracting executable -- UPX, LZEXE, PKLITE --
breaks that: it is decompressed INTO guest RAM, so every page of the program is
a dirty COW page that has to come out of the ~700 KB AXI pool, the LZ4 tier or
the SD pagefile. Shipping the same program EXPANDED trades NOR (64 MB, free) for
pool pages (700 KB, the binding constraint), which is the trade we want every
time. It also removes the unpacking work itself: FreeDOS's UPX-packed KERNEL.SYS
and COMMAND.COM cost ~1.87 M guest instructions on EVERY boot.

So this is the census that says WHERE that cost is. It does not unpack anything
-- unpacking needs `upx -d` / `unlzexe` / a PKLITE unpacker, none of which are
installed here.

Detection is by the packers' own signatures, and it is deliberately
conservative: a file is only reported when the signature is where that packer
puts it, so "0 packed" means something.
"""

import argparse
import os
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "external" / "8086tiny" / "test286"))
import fatscan  # noqa: E402  (an independent FAT12/FAT16 walker already in-tree)

# Only these are loaded and executed by DOS; data files are irrelevant to the
# pool question because nothing decompresses them into low RAM at load time.
EXEC_EXT = (".EXE", ".COM", ".SYS", ".OVL", ".OVR", ".DRV")


def classify(data: bytes) -> str | None:
    """Name the self-extracting packer, or None for a plain executable."""
    tags = []
    if b"UPX!" in data[:1024] or b"UPX!" in data[-1024:]:
        tags.append("UPX")
    # LZEXE stamps its version at 0x1C, in the OEM field of the MZ header, so it
    # only ever applies to an EXE.
    if data[:2] in (b"MZ", b"ZM") and data[0x1C:0x20] in (b"LZ09", b"LZ91"):
        tags.append("LZEXE")
    # PKLITE writes "PKLITE" (usually "PKLITE Copr. ...") into its stub, near the
    # front of the file. It compresses .COM files too, which have no MZ header --
    # KEEN4's DEALERS.EXE is one, despite the extension.
    if b"PKLITE" in data[:0x200]:
        tags.append("PKLITE")
    return "+".join(tags) or None


def files(path: Path):
    img = open(path, "rb").read()
    vol = fatscan.open_image(img)
    if not vol:
        raise ValueError(f"{path}: no FAT12/FAT16 volume (or MBR) found")
    for name, first, size in fatscan.walk(img, vol):
        if size == 0:
            continue
        chain = fatscan.chain(img, vol, first)
        blob = b"".join(
            img[fatscan.cluster_off(vol, c):
                fatscan.cluster_off(vol, c) + vol["spc"] * vol["bps"]]
            for c in chain
        )[:size]
        yield name, size, blob


def scan(path: Path, every: bool) -> tuple[int, int]:
    packed = packed_bytes = looked = 0
    rows = []
    for name, size, blob in files(path):
        if not every and not name.upper().endswith(EXEC_EXT):
            continue
        looked += 1
        tag = classify(blob)
        if tag is None:
            continue
        packed += 1
        packed_bytes += size
        rows.append((name, size, tag))
    # "0 packed" is only news when something was actually examined -- an image
    # whose walk yielded nothing at all would otherwise read as a clean bill of
    # health, so the denominator is always printed.
    print(f"== {path.name}: {packed} packed of {looked} executable(s), "
          f"{packed_bytes} B")
    for name, size, tag in sorted(rows):
        print(f"   {tag:<12} {size:>8}  {name}")
    return packed, packed_bytes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="*", type=Path,
                    help="disk images (default: roms/dos/*.dsk and fd.img)")
    ap.add_argument("--all", action="store_true",
                    help="examine every file, not just executable extensions")
    args = ap.parse_args(argv)

    images = args.images
    if not images:
        images = sorted((REPO / "roms" / "dos").glob("*.dsk"))
        fd = REPO / "external" / "8086tiny" / "fd.img"
        if fd.exists():
            images.append(fd)
    if not images:
        print("no images found (roms/dos is not populated in this checkout)",
              file=sys.stderr)
        return 2

    total = totalb = 0
    for p in images:
        try:
            n, b = scan(p, args.all)
        except Exception as e:                       # keep going over 17 images
            print(f"== {p.name}: ERROR {e}")
            continue
        total += n
        totalb += b
    print(f"-- {total} packed executables over {len(images)} images, {totalb} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
