#!/usr/bin/env python3
"""Create a blank SD-card image with a single FAT32 primary partition.

This replaces the `dd` + `parted` pair that used to build build/sdcard.img.

`parted` is Linux-only: there is no Homebrew formula for it and no macOS port,
so `make gwemu_release` could not build an SD image on a Mac at all. mtools'
`mpartition` is not a drop-in either -- it refuses the `::` pseudo-drive that
every other mtools call in this build uses and would need a generated MTOOLSRC.

Writing the 64-byte MBR partition table ourselves is less machinery than either,
and it is byte-for-byte identical on every host, which matters here: the guiding
rule of the gwemu integration is that the emulator runs the same bytes as the
hardware. python3 is already a hard build dependency, so this adds no new one.

The output matches what `parted -s img mklabel msdos mkpart primary fat32 1MiB 100%`
produced on Linux in every field that is read:

    type 0x0C (FAT32 LBA), first LBA 2048 (=1MiB), and the remaining sectors --
    all three verified identical against a parted-generated reference image.

The legacy CHS triples differ, and deliberately so: parted derives a per-image
fake geometry (4/32 for a 256 MB file), while this writes the conventional
255/63 that fdisk and every distro image builder use. Partition type 0x0C means
"LBA addressing is authoritative", so nothing -- not FatFS on the device, not
mtools, not QEMU -- consults those bytes. Reproducing parted's geometry
heuristic would be guesswork for a field no reader looks at.

The filesystem itself is still laid down by mformat at the @@1M offset, exactly
as before -- this script only creates the container and the partition table.

Sizing
------
`--size-mb N` fixes the size. `--fit DIR` (repeatable) instead measures what is
going to be copied in and picks the smallest *real* SD card size that holds it:
256M, 512M, 1G, 2G, 4G or 8G. Sizes are powers of two because that is what SD
cards actually come in -- nothing in the stack requires it.

The measurement is done here in Python rather than with `du` on purpose. `du -sb`
is GNU-only (BSD/macOS has no -b) and `du -sk` reports *allocated* blocks, which
is the host filesystem's slack, not FAT32's. Walking the tree with os.walk and
rounding each file up to the FAT cluster size models the filesystem the files are
actually going onto, and does it identically on Linux and macOS.
"""

import argparse
import math
import os
import struct
import sys

SECTOR = 512
# The partition starts at 1 MiB, which is what the mformat/mcopy calls in
# Makefile.common address as `img@@1M`. Keep the two in step.
FIRST_LBA = 2048
PART_TYPE_FAT32_LBA = 0x0C

# Sizing bounds, in MB. 256 is the historical fixed size and a sane floor; 8192
# (8 GiB) is the largest card we are willing to emulate.
#
# Sizes snap to a POWER OF TWO. Nothing in the stack demands it -- FatFS, mtools
# and QEMU all read the LBA fields and would accept 704 MB quite happily -- but
# no such SD card exists, and the point of this integration is that the emulator
# sees what the hardware sees. Real cards are 256M/512M/1G/2G/4G/8G, so those are
# the only sizes we produce.
MIN_MB = 256
MAX_MB = 8192
SIZES_MB = [256, 512, 1024, 2048, 4096, 8192]

# FAT32 cluster size. Microsoft's default table gives 4 KiB for volumes from
# 256 MB to 8 GB, which is the whole of our range, and mformat follows it. Used
# only to model per-file slack when sizing.
CLUSTER = 4096

# Headroom on top of the measured content: directory growth and the
# savestates/saves the guest writes at runtime. (The FAT tables themselves are
# accounted for separately.) Kept modest because sizes snap to powers of two: an
# over-generous margin here does not cost a little extra space, it doubles the
# card -- 10% keeps a 900 MB collection on a 1 GB card instead of a 2 GB one.
MARGIN_PCT = 10


def chs(lba, heads=255, sectors=63):
    """Legacy CHS triple for `lba`, clamped to the classic 1023/254/63 maximum.

    Nothing reads these -- the partition is type 0x0C ("LBA") precisely so that
    the LBA fields are authoritative -- but parted fills them in and some tools
    sanity-check them, so we reproduce the same convention.
    """
    c, rem = divmod(lba, heads * sectors)
    h, s = divmod(rem, sectors)
    if c > 1023:
        c, h, s = 1023, heads - 1, sectors - 1
    return bytes([h, ((c >> 2) & 0xC0) | ((s + 1) & 0x3F), c & 0xFF])


def measure(paths):
    """Bytes needed on a FAT32 volume to hold everything under `paths`.

    Each file is rounded up to a whole cluster, because that is what it will
    occupy -- a directory of many small ROMs costs far more than the sum of its
    file sizes. Directories cost a cluster each. Missing paths are skipped, so a
    checkout with no roms/ sizes to the floor rather than failing.
    """
    total = 0
    for root in paths:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            total += CLUSTER * (1 + len(dirnames))
            for name in filenames:
                try:
                    size = os.path.getsize(os.path.join(dirpath, name))
                except OSError:
                    continue          # vanished or unreadable: not ours to fail on
                total += CLUSTER * max(1, math.ceil(size / CLUSTER))
    return total


def auto_size_mb(paths):
    """Clamped, rounded image size in MB that will hold `paths`."""
    content = measure(paths)
    # FAT32 keeps two copies of a 4-bytes-per-cluster table.
    fat = 2 * 4 * math.ceil(content / CLUSTER)
    need = content + fat
    need += need * MARGIN_PCT // 100
    need += FIRST_LBA * SECTOR          # the 1 MiB before the partition

    mb = math.ceil(need / (1024 * 1024))
    for size in SIZES_MB:                # smallest real card that fits
        if size >= mb:
            return size, content, mb
    return MAX_MB, content, mb           # caller warns: mb > MAX_MB


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="output image path")
    ap.add_argument("--size-mb", type=int, help="total image size in MiB")
    ap.add_argument(
        "--fit",
        action="append",
        metavar="DIR",
        default=[],
        help="size the image to hold DIR's contents (repeatable); "
        f"snapped to a real card size in [{MIN_MB}, {MAX_MB}] MB",
    )
    args = ap.parse_args()

    if args.size_mb is None:
        if not args.fit:
            ap.error("give --size-mb, or --fit DIR to size automatically")
        args.size_mb, content, needed = auto_size_mb(args.fit)
        present = [d for d in args.fit if os.path.isdir(d)]
        print(
            f"Sizing for {', '.join(present) or '(nothing found)'}: "
            f"{content // (1024 * 1024)}MB of content on FAT32 "
            f"-> {args.size_mb}MB image"
        )
        # Only a genuine overflow is a problem. Landing on MAX_MB because that
        # is the card the content fits is the normal, correct outcome.
        if needed > MAX_MB:
            print(
                f"WARNING: needs ~{needed}MB but the ceiling is {MAX_MB}MB; "
                "mcopy will fail partway through. Trim what you are copying in.",
                file=sys.stderr,
            )
    elif args.fit:
        ap.error("--size-mb and --fit are mutually exclusive")

    total_sectors = args.size_mb * 1024 * 1024 // SECTOR
    if total_sectors <= FIRST_LBA:
        sys.exit(f"error: --size-mb {args.size_mb} leaves no room for a partition")
    part_sectors = total_sectors - FIRST_LBA

    # Create the file sparsely: a 256 MB image of zeros costs no real disk space
    # until the guest writes to it. `dd if=/dev/zero` allocated the lot.
    with open(args.image, "wb") as f:
        f.truncate(args.size_mb * 1024 * 1024)

        entry = (
            b"\x00"                                  # not bootable
            + chs(FIRST_LBA)                         # CHS of first sector
            + bytes([PART_TYPE_FAT32_LBA])           # partition type
            + chs(FIRST_LBA + part_sectors - 1)      # CHS of last sector
            + struct.pack("<II", FIRST_LBA, part_sectors)
        )
        assert len(entry) == 16

        f.seek(446)
        f.write(entry + b"\x00" * 48)                # entry 1, entries 2-4 empty
        f.write(b"\x55\xaa")                         # MBR signature

    print(
        f"Created {args.size_mb}MB SD card image with a FAT32 partition "
        f"at sector {FIRST_LBA} ({part_sectors} sectors): {args.image}"
    )


if __name__ == "__main__":
    main()
