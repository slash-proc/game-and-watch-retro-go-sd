#!/usr/bin/env python3
"""Pack loose DOS software into bootable FAT12 floppy / FAT16 hard-disk images.

The MS-DOS core boots a disk image, not a bare executable, so a user with a
plain CAT.EXE has no way to launch anything. This turns either shape of input
into a bootable disk:

    roms/dos/CAT.EXE              -> roms/dos/CAT.dsk
    roms/dos/PRINCE_OF_PERSIA/    -> roms/dos/PRINCE_OF_PERSIA.dsk

Two sources of DOS are supported.

  --system freedos (default)
      System files are lifted out of external/8086tiny/fd.img, a bootable
      FreeDOS floppy shipped with the emulator (MIT/GPL-clean, unlike any game).

  --system msdos --media DIR
      System files are lifted out of user-supplied MS-DOS install floppies
      (IO.SYS, MSDOS.SYS and COMMAND.COM all sit uncompressed on Disk 1 of
      MS-DOS 6.22). Nothing from that media is ever written anywhere but the
      output .dsk -- it is proprietary and must not reach the repo or sd_content.

Either way a generated AUTOEXEC.BAT runs the target so launching drops straight
into the program instead of a C:\\> prompt. When the program exits it falls
through to that prompt -- it does not power the emulator off; leaving is
retro-go's overlay's job.

--bare builds a disk with no payload at all, which is just a bootable DOS that
lands at a prompt.

Anything that does not fit 1.44 MB gets a partitioned FAT16 hard-disk image
instead (--hdd, and automatically when the payload overflows a floppy). That
path has a pile of constraints a floppy does not; they are all written down in
the "hard disk" section further down this file, and the short version is:

    python3 tools/mkdosdisk.py --media dos_variants --hdd \\
        --entry WOLF3D.EXE --size 3096576 games/WOLF3D
    python3 tools/mkdosdisk.py --media dos_variants --hdd --subdir DF \\
        --entry DFDEMO.BAT --size 10321920 games/STAR_WARS_DARKFORCES
    python3 tools/mkdosdisk.py --media dos_variants --hdd \\
        --only CDCHESS --only DATA --entry 'CDCHESS\\CDCHESS.EXE' \\
        --size 66060288 games/BATTLECHESS

Usage:
    python3 tools/mkdosdisk.py                     # pack everything in roms/dos
    python3 tools/mkdosdisk.py --src roms/dos --dst roms/dos
    python3 tools/mkdosdisk.py --entry GAME.EXE PRINCE_OF_PERSIA
    python3 tools/mkdosdisk.py --bare --name msdos622   # plain bootable disk
    python3 tools/mkdosdisk.py --list-template     # show what the DOS source has
    python3 tools/mkdosdisk.py --media external/8086tiny/dos_variants CAT.EXE
    python3 tools/mkdosdisk.py --media dos_variants --util-set tools GAME/
    python3 tools/mkdosdisk.py --media dos_variants --verify CAT.EXE

Why a hand-rolled FAT12 writer instead of pyfatfs: pyfatfs was evaluated and
rejected on two counts, both verified against 1.0.5 --

  1. remove() unlinks the directory entry but never frees the cluster chain,
     so a template-and-strip approach cannot reclaim any space (112 free
     clusters before and after deleting a 55 KB file).
  2. mkfs() writes sectors-per-track = 0 and heads = 0, and picks 12
     sectors-per-FAT where a real 1.44 MB floppy uses 9. Zero CHS geometry
     breaks INT 13h addressing, and the differing FAT size moves the root
     directory, so fd.img's boot sector cannot be transplanted onto it.

FAT12 on a fixed-geometry floppy is simple enough that owning it outright is
smaller and safer than working around those, and it keeps requirements.txt
untouched.

What the MS-DOS 6.22 boot sector actually requires, read off a disassembly of
Disk 1 sector 0 rather than off folklore (see docs/storage/04-disk-packing.md
for the annotated listing):

  * It loads ONE sector of the root directory and does `repz cmpsb` of 11 bytes
    at entry 0 against "IO      SYS" and 11 bytes at entry 1 (+0x20) against
    "MSDOS   SYS". Either mismatch prints "Non-System disk" and stops. So those
    two must be the first two directory entries, in that order.
  * It takes IO.SYS's first cluster from the directory entry, so IO.SYS does
    *not* have to live at cluster 2 -- but it then reads 3 sectors with a plain
    LBA++ loop, so those first 3 sectors must be physically contiguous. The
    remainder is loaded by MSLOAD (inside those 3 sectors), which also assumes
    contiguity; we therefore make both system files fully contiguous, which
    sequential allocation gives for free.
  * Attributes are never examined. The genuine install media carries IO.SYS and
    MSDOS.SYS as 0x21 (read-only|archive), not the hidden|system 0x27 that
    SYS.COM writes, and boots fine. We write 0x27 anyway because that is what
    DOS itself expects to find, and it keeps DIR output looking normal.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# --- 1.44 MB floppy geometry -------------------------------------------------
# Fixed on purpose. 720 KB images were considered and rejected: SD cards are
# large, a second geometry means a second boot sector to validate, and every
# 8086tiny-era program fits 1.44 MB anyway.
BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 224
TOTAL_SECTORS = 2880
SECTORS_PER_FAT = 9
SECTORS_PER_TRACK = 18
NUM_HEADS = 2
MEDIA_DESCRIPTOR = 0xF0

ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
FAT_START = RESERVED_SECTORS
ROOT_START = RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT
DATA_START = ROOT_START + ROOT_DIR_SECTORS
TOTAL_CLUSTERS = (TOTAL_SECTORS - DATA_START) // SECTORS_PER_CLUSTER
IMAGE_SIZE = TOTAL_SECTORS * BYTES_PER_SECTOR

TEMPLATE = Path("external/8086tiny/fd.img")
MEDIA_DIR = Path("external/8086tiny/dos_variants")

# Files copied out of the template to make the image bootable. KERNEL.SYS and
# COMMAND.COM are mandatory; CONFIG.SYS sets SHELL= and is what makes
# COMMAND.COM load. QUITEMU.COM is 5 bytes and exits the emulator; it is kept on
# the disk as something a user can type, but AUTOEXEC.BAT no longer runs it (see
# make_autoexec).
SYSTEM_FILES = ("KERNEL.SYS", "COMMAND.COM", "CONFIG.SYS", "QUITEMU.COM")
REQUIRED_SYSTEM_FILES = ("KERNEL.SYS", "COMMAND.COM")

# MS-DOS. Order is load-bearing, not cosmetic: the boot sector checks root
# entries 0 and 1 by name (see the module docstring).
MSDOS_SYSTEM_FILES = ("IO.SYS", "MSDOS.SYS", "COMMAND.COM")
ATTR_ARCHIVE = 0x20
ATTR_SYSTEM_FILE = 0x27  # read-only | hidden | system | archive

# Every name a caller may pull off the install media with --util. Restricted to
# files that are stored uncompressed and can do something on an 8086: HIMEM.SYS
# and EMM386.EXE want a 286/386 and are compressed besides, so they are absent
# on purpose rather than by oversight.
MSDOS_UTILS = {
    "ATTRIB.EXE", "CHKDSK.EXE", "COUNTRY.SYS", "DEBUG.EXE", "DEFRAG.EXE",
    "EDIT.COM", "EXPAND.EXE", "FDISK.EXE", "FORMAT.COM", "KEYB.COM",
    "KEYBOARD.SYS", "MSCDEX.EXE", "NLSFUNC.EXE", "QBASIC.EXE", "SCANDISK.EXE",
    "SYS.COM", "CHOICE.COM", "HELP.COM", "MORE.COM", "UNFORMAT.COM",
    "DRVSPACE.EXE", "MEMMAKER.EXE", "MSD.EXE", "UNDELETE.EXE",
}

# Named convenience groups for --util-set. Deliberately small: a game needs no
# DOS utility at all, and every cluster spent here is a cluster the payload
# cannot have, so the default is none of them.
MSDOS_UTIL_SETS = {
    "none": (),
    # Enough to poke at a disk that will not run: look at files, edit a .BAT,
    # check the FAT, disassemble.  ~93 KB.
    "tools": ("ATTRIB.EXE", "CHKDSK.EXE", "EDIT.COM", "DEBUG.EXE"),
    # For someone who wants to expand the SZDD/KWAJ-compressed *._ files
    # themselves, inside the emulator, because we do not do it on the host.
    "expand": ("EXPAND.EXE",),
}

EXECUTABLE_SUFFIXES = (".EXE", ".COM", ".BAT")

# Names that are almost never the thing you want to run. Checked before the
# generic pick so a directory shipping SETUP.EXE next to the real binary does
# not boot into the installer.
ENTRY_DEPRIORITISED = (
    "SETUP", "INSTALL", "INSTALLE", "CONFIG", "CONFIGUR", "README",
    "UNINSTAL", "PATCH", "UPDATE", "REGISTER", "ORDER", "HELP", "MENU",
)


class DiskFullError(Exception):
    """Raised when the payload does not fit a 1.44 MB floppy."""


# --- minimal FAT12 reader (template only) ------------------------------------

class Fat12Reader:
    """Read-only FAT12 access, enough to lift system files out of fd.img."""

    def __init__(self, path: Path):
        self.data = path.read_bytes()
        d = self.data
        self.bps = struct.unpack_from("<H", d, 11)[0]
        self.spc = d[13]
        self.reserved = struct.unpack_from("<H", d, 14)[0]
        self.nfats = d[16]
        self.root_entries = struct.unpack_from("<H", d, 17)[0]
        self.spf = struct.unpack_from("<H", d, 22)[0]
        self.root_start = self.reserved + self.nfats * self.spf
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_start = self.root_start + self.root_sectors

    @property
    def boot_sector(self) -> bytes:
        return self.data[: self.bps]

    def _fat_entry(self, cluster: int) -> int:
        base = self.reserved * self.bps
        off = base + cluster + cluster // 2
        val = struct.unpack_from("<H", self.data, off)[0]
        return (val >> 4) if (cluster & 1) else (val & 0x0FFF)

    def _root_entries_raw(self):
        base = self.root_start * self.bps
        for i in range(self.root_entries):
            e = self.data[base + i * 32 : base + i * 32 + 32]
            if not e or e[0] == 0x00:
                return
            if e[0] == 0xE5 or (e[11] & 0x0F) == 0x0F:  # deleted / LFN
                continue
            yield e

    def listdir(self) -> list[str]:
        return [n for n, _, _ in self.entries()]

    def entries(self) -> list[tuple[str, int, bool]]:
        """(name, size, is_dir) for every live root entry."""
        out = []
        for e in self._root_entries_raw():
            name, ext = e[0:8].decode("latin1").strip(), e[8:11].decode("latin1").strip()
            out.append((f"{name}.{ext}" if ext else name,
                        struct.unpack_from("<I", e, 28)[0],
                        bool(e[11] & 0x10)))
        return out

    def read(self, filename: str) -> bytes | None:
        want = filename.upper()
        for e in self._root_entries_raw():
            name, ext = e[0:8].decode("latin1").strip(), e[8:11].decode("latin1").strip()
            full = f"{name}.{ext}" if ext else name
            if full != want:
                continue
            if e[11] & 0x10:  # directory
                return None
            size = struct.unpack_from("<I", e, 28)[0]
            cluster = struct.unpack_from("<H", e, 26)[0]
            chunks, remaining = [], size
            while 2 <= cluster < 0xFF0 and remaining > 0:
                off = (self.data_start + (cluster - 2) * self.spc) * self.bps
                take = min(self.spc * self.bps, remaining)
                chunks.append(self.data[off : off + take])
                remaining -= take
                cluster = self._fat_entry(cluster)
            return b"".join(chunks)
        return None


# --- FAT12 writer ------------------------------------------------------------

class Fat12Image:
    """A 1.44 MB FAT12 floppy built from scratch.

    Files are allocated contiguously from the start of the data area, in the
    order added. That matters: some DOS boot sectors require KERNEL.SYS to be
    contiguous, so it is written first and sequential allocation guarantees it.
    """

    def __init__(self, boot_sector: bytes):
        if len(boot_sector) != BYTES_PER_SECTOR:
            raise ValueError(f"boot sector must be {BYTES_PER_SECTOR} bytes")
        self.image = bytearray(IMAGE_SIZE)
        self.image[0:BYTES_PER_SECTOR] = boot_sector
        self._assert_geometry()

        # FAT[0] = media descriptor + 0xFFF, FAT[1] = 0xFFF. Cluster 0/1 reserved.
        self.fat = [0] * (TOTAL_CLUSTERS + 2)
        self.fat[0] = 0xF00 | MEDIA_DESCRIPTOR
        self.fat[1] = 0xFFF

        self.entries: list[bytes] = []
        self.next_cluster = 2

    def _assert_geometry(self):
        """The boot sector's BPB must describe exactly the layout we write.

        Transplanting a boot sector whose BPB disagrees with the filesystem is
        the single easiest way to produce an image that looks fine and does not
        boot, so it is checked rather than assumed.
        """
        d = self.image
        checks = (
            ("bytes/sector", struct.unpack_from("<H", d, 11)[0], BYTES_PER_SECTOR),
            ("sectors/cluster", d[13], SECTORS_PER_CLUSTER),
            ("reserved sectors", struct.unpack_from("<H", d, 14)[0], RESERVED_SECTORS),
            ("number of FATs", d[16], NUM_FATS),
            ("root entries", struct.unpack_from("<H", d, 17)[0], ROOT_ENTRIES),
            ("total sectors", struct.unpack_from("<H", d, 19)[0], TOTAL_SECTORS),
            ("media descriptor", d[21], MEDIA_DESCRIPTOR),
            ("sectors/FAT", struct.unpack_from("<H", d, 22)[0], SECTORS_PER_FAT),
            ("sectors/track", struct.unpack_from("<H", d, 24)[0], SECTORS_PER_TRACK),
            ("heads", struct.unpack_from("<H", d, 26)[0], NUM_HEADS),
        )
        for label, got, want in checks:
            if got != want:
                raise ValueError(
                    f"template boot sector BPB mismatch: {label} is {got}, expected {want}"
                )
        if struct.unpack_from("<H", d, 510)[0] != 0xAA55:
            raise ValueError("template boot sector missing 0xAA55 signature")

    @property
    def free_bytes(self) -> int:
        return (TOTAL_CLUSTERS - (self.next_cluster - 2)) * SECTORS_PER_CLUSTER * BYTES_PER_SECTOR

    def add(self, name: str, content: bytes, attr: int = ATTR_ARCHIVE):
        if len(self.entries) >= ROOT_ENTRIES:
            raise DiskFullError(f"root directory full ({ROOT_ENTRIES} entries)")

        cluster_bytes = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
        need = (len(content) + cluster_bytes - 1) // cluster_bytes
        if need > TOTAL_CLUSTERS - (self.next_cluster - 2):
            raise DiskFullError(
                f"{name} needs {len(content):,} bytes, only {self.free_bytes:,} free"
            )

        first = self.next_cluster if need else 0
        for i in range(need):
            c = self.next_cluster + i
            self.fat[c] = 0xFFF if i == need - 1 else c + 1
            off = (DATA_START + (c - 2) * SECTORS_PER_CLUSTER) * BYTES_PER_SECTOR
            chunk = content[i * cluster_bytes : (i + 1) * cluster_bytes]
            self.image[off : off + len(chunk)] = chunk
        self.next_cluster += need

        self.entries.append(self._dir_entry(name, first, len(content), attr))
        return first

    @staticmethod
    def _dir_entry(name: str, first_cluster: int, size: int,
                   attr: int = ATTR_ARCHIVE) -> bytes:
        stem, _, ext = name.upper().partition(".")
        e = bytearray(32)
        e[0:8] = stem[:8].ljust(8).encode("latin1")
        e[8:11] = ext[:3].ljust(3).encode("latin1")
        e[11] = attr
        struct.pack_into("<H", e, 22, 0)       # time
        struct.pack_into("<H", e, 24, 0x0021)  # date: 1980-01-01
        struct.pack_into("<H", e, 26, first_cluster)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def _pack_fat(self) -> bytes:
        raw = bytearray(SECTORS_PER_FAT * BYTES_PER_SECTOR)
        for c, val in enumerate(self.fat):
            off = c + c // 2
            cur = struct.unpack_from("<H", raw, off)[0]
            if c & 1:
                new = (cur & 0x000F) | ((val & 0x0FFF) << 4)
            else:
                new = (cur & 0xF000) | (val & 0x0FFF)
            struct.pack_into("<H", raw, off, new)
        return bytes(raw)

    def build(self) -> bytes:
        fat = self._pack_fat()
        for i in range(NUM_FATS):
            off = (FAT_START + i * SECTORS_PER_FAT) * BYTES_PER_SECTOR
            self.image[off : off + len(fat)] = fat
        off = ROOT_START * BYTES_PER_SECTOR
        blob = b"".join(self.entries)
        self.image[off : off + len(blob)] = blob
        return bytes(self.image)


# --- hard disk: geometry -----------------------------------------------------
#
# Everything below exists because a 1.44 MB floppy cannot hold Wolfenstein 3D,
# Dark Forces or Battle Chess, and 8086tiny's BIOS has opinions about what a
# hard disk image may look like that no partitioning tool knows about.
#
# The BIOS derives the whole CHS geometry from the *image size alone* -- there
# is no geometry stored anywhere, and nothing you can pass it
# (external/8086tiny/bios_source/bios.asm:157-195):
#
#     tracks = image_sectors / 63          ; integer division, 63 SPT is fixed
#     if tracks > 1024:
#         heads  = tracks / 1024           ; integer division again
#         tracks = 1024
#     else:
#         heads  = 1
#
# chs_to_abs (bios.asm:3000-3050) is then plain linear CHS->LBA and
# int13_read_disk (:2147) seeks LBA<<9, so the addressable disk is exactly
# 1024*heads*63 sectors once you are past a track count of 1024.
#
# Two consequences, both of which will bite anyone who "just resizes" an image:
#
#  1. The image must be an exact multiple of tracks*heads*63 sectors. A trailing
#     partial track is silently unreachable, and any file the FAT thinks lives
#     there reads back as garbage -- not as an error.
#
#  2. THE 31.5 MB - 63 MB DEAD ZONE. Up to 1024 tracks the geometry is 1 head
#     and sizes are continuous in 32,256-byte (one track) steps, topping out at
#     1024*1*63 = 31.5 MB. The moment the track count exceeds 1024 the integer
#     division `heads = tracks / 1024` truncates: 1025..2047 tracks all yield
#     heads = 1 and tracks clamped to 1024, i.e. the BIOS addresses only the
#     first 31.5 MB and the rest of the image does not exist as far as INT 13h
#     is concerned. The next size that works at all is 2048 tracks = 2 heads =
#     63 MB. So valid sizes are:
#
#         tracks <= 1024                 -> heads = 1   (up to 31.5 MB)
#         tracks == 1024 * N, N >= 2     -> heads = N   (63 MB, 94.5 MB, ...)
#         anything in between            -> BROKEN, silently
#
#     This is why BATTLECHESS is a 63 MB image holding 33 MB of content. Do not
#     "optimise" it down to 40 MB: 40 MB is inside the dead zone and the disk
#     will mount, boot, and then hand out corrupt data from the far end of
#     DATA.DAT. Use --size if you want a specific size; it is rounded UP to the
#     next size the BIOS can actually address, never down.
HDD_SECTORS_PER_TRACK = 63
HDD_MAX_TRACKS = 1024

# Sectors reserved ahead of the partition. 63 (one full track) is not a
# tradition here, it is arithmetic: the partition must start on a track
# boundary for the CHS fields in the partition table to be expressible, and
# track 0 is where the MBR lives.
HDD_PART_LBA = 63


def hdd_geometry(total_sectors: int) -> tuple[int, int]:
    """(tracks, heads) exactly as 8086tiny's BIOS will compute them."""
    tracks = total_sectors // HDD_SECTORS_PER_TRACK
    if tracks > HDD_MAX_TRACKS:
        return HDD_MAX_TRACKS, tracks // HDD_MAX_TRACKS
    return tracks, 1


def hdd_size_is_valid(total_sectors: int) -> bool:
    """True if the BIOS's geometry addresses exactly this image, no more, no less."""
    tracks, heads = hdd_geometry(total_sectors)
    return total_sectors == tracks * heads * HDD_SECTORS_PER_TRACK


def hdd_round_up(total_sectors: int) -> int:
    """Smallest BIOS-addressable image with at least `total_sectors` sectors.

    Refusing an unusable size outright would be defensible, but every caller
    would then have to reimplement this table, so we round up instead and print
    what happened. Rounding *down* is never right: it silently throws away
    payload space that was asked for.
    """
    track = HDD_SECTORS_PER_TRACK
    if total_sectors <= HDD_MAX_TRACKS * track:            # 1 head, continuous
        return max(1, (total_sectors + track - 1) // track) * track
    # Past 31.5 MB only whole multiples of 1024 tracks exist -- the dead zone.
    per_head = HDD_MAX_TRACKS * track
    heads = (total_sectors + per_head - 1) // per_head
    return heads * per_head


# --- hard disk: the MBR ------------------------------------------------------
#
# 8086tiny's BIOS boots a hard disk the way a real one does: it reads LBA 0 of
# drive 0x80 to 0000:7C00 and jumps there (bios.asm:345-357). It does *not*
# understand partitions -- but MS-DOS refuses to see C: without a partition
# table, so LBA 0 has to carry both a partition table and enough code to chain
# to the partition's own boot sector.
#
# THERE IS NO nasm IN THIS TREE (and adding an assembler to the build for 59
# bytes is not worth it), so the bootstrap below is a hand-assembled byte array.
# It is commented one instruction at a time precisely so it can be checked by
# eye, without a disassembler and without trusting this comment block:
MBR_BOOTSTRAP = bytes([
    0xFA,                    # cli                    ; no interrupts while SS:SP moves
    0x33, 0xC0,              # xor   ax, ax
    0x8E, 0xD0,              # mov   ss, ax           ; SS = 0
    0xBC, 0x00, 0x7C,        # mov   sp, 0x7C00       ; stack just below ourselves
    0x8E, 0xC0,              # mov   es, ax           ; ES = 0
    0x8E, 0xD8,              # mov   ds, ax           ; DS = 0
    0xFB,                    # sti
    0xBE, 0x00, 0x7C,        # mov   si, 0x7C00       ; copy ourselves out of the way:
    0xBF, 0x00, 0x06,        # mov   di, 0x0600       ;   0000:7C00 -> 0000:0600,
    0xB9, 0x00, 0x01,        # mov   cx, 256          ;   256 words = one sector,
    0xFC,                    # cld                    ;   because 7C00 is where the
    0xF3, 0xA5,              # rep   movsw            ;   VBR must be loaded next.
    0xEA, 0x1E, 0x06, 0x00, 0x00,   # jmp far 0000:061E  ; continue at the copy; 0x61E
                             #                        ; is 0x600 + the offset of the
                             #                        ; next byte (0x1E) in this array.
    0xBE, 0xBE, 0x07,        # mov   si, 0x07BE       ; 0x600+0x1BE = partition entry 1
    0xB2, 0x80,              # mov   dl, 0x80         ; drive: first hard disk. Forced
                             #                        ; rather than trusting the DL the
                             #                        ; BIOS came in with.
    0x8A, 0x74, 0x01,        # mov   dh, [si+1]       ; DH  = partition's start head
    0x8B, 0x4C, 0x02,        # mov   cx, [si+2]       ; CX  = start sector/cylinder,
                             #                        ;       already in INT 13h layout
    0xBB, 0x00, 0x7C,        # mov   bx, 0x7C00       ; ES:BX = load address
    0xB8, 0x01, 0x02,        # mov   ax, 0x0201       ; AH=02 read, AL=01 one sector
    0xCD, 0x13,              # int   0x13
    0x72, 0x05,              # jc    +5               ; on error fall into the halt
    0xEA, 0x00, 0x7C, 0x00, 0x00,   # jmp far 0000:7C00  ; hand over to the VBR
    0xF4,                    # hlt
    0xEB, 0xFD,              # jmp   $-1              ; wedge; nothing sane to do here
])
MBR_PART_TABLE_OFFSET = 0x1BE


def chs_bytes(lba: int, heads: int) -> bytes:
    """(head, sector|cyl_hi, cyl_lo) as a partition table stores CHS.

    Cylinders above 1023 cannot be expressed at all, which is exactly why the
    end-CHS of a 63 MB image points at cylinder 1023 head 0 rather than at the
    true last sector -- see hdd_partition_sectors.
    """
    spt = HDD_SECTORS_PER_TRACK
    cyl, rem = divmod(lba, heads * spt)
    head, sector = divmod(rem, spt)
    sector += 1                              # sectors are 1-based, heads/cyls are not
    cyl = min(cyl, 1023)
    return bytes([head, sector | ((cyl >> 2) & 0xC0), cyl & 0xFF])


def hdd_partition_sectors(total_sectors: int, heads: int) -> int:
    """How many sectors partition 1 gets: everything after the MBR track,
    rounded DOWN to a whole cylinder.

    DOS-era tools all end a partition on a cylinder boundary and some of them
    check. It costs at most one cylinder (63 sectors per head) and it keeps the
    end-CHS field honest.
    """
    cylinder = HDD_SECTORS_PER_TRACK * heads
    return ((total_sectors - HDD_PART_LBA) // cylinder) * cylinder


def build_mbr(total_sectors: int, heads: int, part_sectors: int) -> bytes:
    """LBA 0: the bootstrap above, one partition entry, and 0xAA55."""
    mbr = bytearray(BYTES_PER_SECTOR)
    mbr[0:len(MBR_BOOTSTRAP)] = MBR_BOOTSTRAP

    # Type 0x04 is "FAT16 below 32 MB", 0x06 is "FAT16, CHS, any size". MS-DOS
    # 6.22 mounts either; using the small-disk type where it applies is what
    # every period tool did, and 0x06 on a <32 MB disk makes some of them sulk.
    ptype = 0x04 if part_sectors < 65536 else 0x06

    e = bytearray(16)
    e[0] = 0x80                                        # active/bootable
    e[1:4] = chs_bytes(HDD_PART_LBA, heads)            # start CHS
    e[4] = ptype
    e[5:8] = chs_bytes(HDD_PART_LBA + part_sectors - 1, heads)   # end CHS
    struct.pack_into("<I", e, 8, HDD_PART_LBA)
    struct.pack_into("<I", e, 12, part_sectors)
    mbr[MBR_PART_TABLE_OFFSET:MBR_PART_TABLE_OFFSET + 16] = e
    struct.pack_into("<H", mbr, 510, 0xAA55)
    return bytes(mbr)


# --- hard disk: FAT16 --------------------------------------------------------

FAT16_MIN_CLUSTERS = 4085      # below this a driver is entitled to read it as FAT12
FAT16_MAX_CLUSTERS = 65524     # above this it would have to be FAT32
HDD_DEFAULT_SPC = 4            # 2 KiB clusters: what mformat picks for these sizes


def fat16_root_entries(part_sectors: int) -> int:
    """Root directory size, matching what mformat 4.0.48 chooses.

    Any value works as long as the BPB agrees with the bytes on disk, so this
    is not a correctness constraint -- it is here so that an image regenerated
    by this script has the same layout as the mformat-built ones it replaces
    and can be diffed against them. Thresholds read off mformat directly.
    """
    if part_sectors < 1024:
        return 64
    if part_sectors < 3969:
        return 224
    if part_sectors < 8001:
        return 240
    return 512


def fat16_cluster_size(part_sectors: int, root_entries: int) -> int:
    """Sectors per cluster: the smallest power of two that lands in FAT16.

    Start at 2 KiB and go *down* while the disk is small enough that 2 KiB
    clusters would give fewer than 4085 clusters (that is FAT12 territory, and
    the FAT12/FAT16 boundary is decided by cluster count, not by disk size --
    getting it wrong produces a filesystem DOS reads with the wrong FAT width).
    Go up if the disk is big enough to overflow a 16-bit FAT.
    """
    root_sectors = (root_entries * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR

    def clusters(spc: int) -> int:
        # sectors_per_fat depends on the cluster count and vice versa, so solve
        # it the way every FAT implementation does: assume, compute, iterate.
        spf = 1
        for _ in range(8):
            usable = part_sectors - RESERVED_SECTORS - NUM_FATS * spf - root_sectors
            n = max(0, usable // spc)
            new_spf = ((n + 2) * 2 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
            if new_spf == spf:
                return n
            spf = new_spf
        return n

    spc = HDD_DEFAULT_SPC
    while spc > 1 and clusters(spc) < FAT16_MIN_CLUSTERS:
        spc //= 2
    while spc < 128 and clusters(spc) > FAT16_MAX_CLUSTERS:
        spc *= 2
    n = clusters(spc)
    if not FAT16_MIN_CLUSTERS <= n <= FAT16_MAX_CLUSTERS:
        raise DiskFullError(
            f"no cluster size makes {part_sectors:,} sectors a FAT16 "
            f"(best was {n:,} clusters at {spc} sectors/cluster)"
        )
    return spc


class Fat16Image:
    """A partitioned FAT16 hard disk built from scratch, subdirectories and all.

    Same allocation discipline as Fat12Image -- strictly sequential from the
    start of the data area, in the order things are added -- for the same
    reason: IO.SYS must land at cluster 2 and be contiguous, and free lists are
    a problem this tool does not need to have.

    The boot sector is MS-DOS 6.22's, transplanted onto a BPB we compute:

      * bytes 0x00-0x02 (the JMP over the BPB) and 0x3E-0x1FD (the code) come
        from the donor floppy. That code is FAT-size-agnostic -- it reads the
        BPB for everything -- so a floppy's boot code drives a hard disk fine.
      * bytes 0x03-0x0A, the OEM name, come along too. Nothing reads it, but
        "MSDOS5.0" is what a DOS-formatted disk says and it makes the two
        images comparable.
      * byte 0x24 is the INT 13h drive number and MUST be 0x80 here. The same
        boot code on a floppy carries 0x00; it takes the drive to read from
        this byte, so leaving a floppy's 0x00 in place makes the disk try to
        boot itself off drive A: and hang.
      * bytes 0x1C-0x1F, hidden sectors, MUST be the partition's start LBA (63).
        DOS adds it to every CHS it computes; a zero here reads the MBR track
        instead of the filesystem.
    """

    def __init__(self, total_sectors: int, donor_boot_sector: bytes,
                 volume_serial: int = 0x12345678):
        if not hdd_size_is_valid(total_sectors):
            raise DiskFullError(
                f"{total_sectors} sectors is not a geometry 8086tiny's BIOS can "
                f"address (see the dead-zone comment above hdd_geometry)")
        self.total_sectors = total_sectors
        self.tracks, self.heads = hdd_geometry(total_sectors)
        self.part_lba = HDD_PART_LBA
        self.part_sectors = hdd_partition_sectors(total_sectors, self.heads)

        self.root_entries = fat16_root_entries(self.part_sectors)
        self.spc = fat16_cluster_size(self.part_sectors, self.root_entries)
        self.root_sectors = (self.root_entries * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR

        # Solve sectors-per-FAT against the cluster count it itself determines.
        spf = 1
        for _ in range(8):
            usable = (self.part_sectors - RESERVED_SECTORS - NUM_FATS * spf
                      - self.root_sectors)
            clusters = usable // self.spc
            new_spf = ((clusters + 2) * 2 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
            if new_spf == spf:
                break
            spf = new_spf
        self.spf = spf
        self.total_clusters = clusters

        self.fat_start = RESERVED_SECTORS
        self.root_start = RESERVED_SECTORS + NUM_FATS * self.spf
        self.data_start = self.root_start + self.root_sectors

        self.image = bytearray(total_sectors * BYTES_PER_SECTOR)
        self.image[0:BYTES_PER_SECTOR] = build_mbr(
            total_sectors, self.heads, self.part_sectors)
        self._write_boot_sector(donor_boot_sector, volume_serial)

        self.fat = [0] * (self.total_clusters + 2)
        self.fat[0] = 0xFF00 | 0xF8      # media descriptor F8 = fixed disk
        self.fat[1] = 0xFFFF
        self.next_cluster = 2

        # name -> list of 32-byte entries. "" is the fixed-size root.
        self.dirs: dict[str, list[bytes]] = {"": []}
        self.dir_clusters: dict[str, list[int]] = {}

    # -- geometry/BPB ---------------------------------------------------------

    def _write_boot_sector(self, donor: bytes, serial: int):
        if len(donor) != BYTES_PER_SECTOR:
            raise ValueError("donor boot sector must be 512 bytes")
        b = bytearray(BYTES_PER_SECTOR)
        b[0x00:0x03] = donor[0x00:0x03]        # JMP SHORT xx / NOP
        b[0x03:0x0B] = donor[0x03:0x0B]        # OEM name
        struct.pack_into("<H", b, 0x0B, BYTES_PER_SECTOR)
        b[0x0D] = self.spc
        struct.pack_into("<H", b, 0x0E, RESERVED_SECTORS)
        b[0x10] = NUM_FATS
        struct.pack_into("<H", b, 0x11, self.root_entries)
        # A 16-bit total-sector count is used when it fits and zeroed otherwise,
        # with the 32-bit field at 0x20 taking over. Filling in both, or the
        # wrong one, is a classic way to get a disk DOS sizes incorrectly.
        small = self.part_sectors if self.part_sectors < 0x10000 else 0
        struct.pack_into("<H", b, 0x13, small)
        b[0x15] = 0xF8
        struct.pack_into("<H", b, 0x16, self.spf)
        struct.pack_into("<H", b, 0x18, HDD_SECTORS_PER_TRACK)
        struct.pack_into("<H", b, 0x1A, self.heads)
        struct.pack_into("<I", b, 0x1C, self.part_lba)      # hidden sectors
        struct.pack_into("<I", b, 0x20, 0 if small else self.part_sectors)
        b[0x24] = 0x80                                      # INT 13h drive
        b[0x25] = 0x00
        b[0x26] = 0x29                                      # extended BPB signature
        struct.pack_into("<I", b, 0x27, serial)
        b[0x2B:0x36] = b"NO NAME    "
        b[0x36:0x3E] = b"FAT16   "
        b[0x3E:0x1FE] = donor[0x3E:0x1FE]                    # the boot code itself
        struct.pack_into("<H", b, 0x1FE, 0xAA55)
        off = self.part_lba * BYTES_PER_SECTOR
        self.image[off:off + BYTES_PER_SECTOR] = bytes(b)

    @property
    def cluster_bytes(self) -> int:
        return self.spc * BYTES_PER_SECTOR

    @property
    def free_bytes(self) -> int:
        return (self.total_clusters - (self.next_cluster - 2)) * self.cluster_bytes

    def _cluster_offset(self, cluster: int) -> int:
        return ((self.part_lba + self.data_start + (cluster - 2) * self.spc)
                * BYTES_PER_SECTOR)

    def _allocate(self, nclusters: int, what: str) -> int:
        if nclusters > self.total_clusters - (self.next_cluster - 2):
            raise DiskFullError(
                f"{what} needs {nclusters * self.cluster_bytes:,} bytes, "
                f"only {self.free_bytes:,} free")
        first = self.next_cluster
        for i in range(nclusters):
            c = first + i
            self.fat[c] = 0xFFFF if i == nclusters - 1 else c + 1
        self.next_cluster += nclusters
        return first

    # -- files and directories ------------------------------------------------

    def mkdir(self, name: str, expected_entries: int = 0) -> None:
        """Create a subdirectory of the root, sized up front.

        Sized up front because allocation here is sequential and append-only: a
        directory that had to grow after files were written to it would end up
        with its second cluster on the far side of the payload. DOS copes with
        that, but nothing else in this file does, so instead we ask the caller
        how many entries are coming and allocate in one go.
        """
        if name in self.dirs:
            return
        per_cluster = self.cluster_bytes // 32
        need = max(1, (expected_entries + 2 + per_cluster - 1) // per_cluster)
        first = self._allocate(need, f"directory {name}")
        self.dir_clusters[name] = list(range(first, first + need))
        # "." and ".." come first and are mandatory; ".."'s cluster is 0 for the
        # root, which is how DOS spells "the fixed root directory area".
        # NOT built with _dir_entry: that splits a name on "." into stem and
        # extension, which turns "." and ".." into blank names. They are raw
        # 11-byte fields here, exactly as DOS writes them.
        self.dirs[name] = [
            self._dot_entry(b".          ", first),
            self._dot_entry(b"..         ", 0),
        ]
        self.dirs[""].append(self._dir_entry(name, first, 0, 0x10))

    def add(self, name: str, content: bytes, attr: int = ATTR_ARCHIVE,
            directory: str = "") -> int:
        if directory not in self.dirs:
            raise DiskFullError(f"no such directory on the image: {directory}")
        if directory == "" and len(self.dirs[""]) >= self.root_entries:
            raise DiskFullError(f"root directory full ({self.root_entries} entries)")
        if directory:
            capacity = len(self.dir_clusters[directory]) * self.cluster_bytes // 32
            if len(self.dirs[directory]) >= capacity:
                raise DiskFullError(f"directory {directory} full ({capacity} entries)")

        need = (len(content) + self.cluster_bytes - 1) // self.cluster_bytes
        first = self._allocate(need, name) if need else 0
        for i in range(need):
            off = self._cluster_offset(first + i)
            chunk = content[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            self.image[off:off + len(chunk)] = chunk
        self.dirs[directory].append(
            self._dir_entry(name, first, len(content), attr))
        return first

    # Identical to the FAT12 one: the on-disk directory entry format does not
    # change between the two, only the FAT does.
    _dir_entry = staticmethod(Fat12Image._dir_entry)

    @staticmethod
    def _dot_entry(raw_name: bytes, first_cluster: int) -> bytes:
        e = bytearray(32)
        e[0:11] = raw_name
        e[11] = 0x10                            # directory
        struct.pack_into("<H", e, 24, 0x0021)   # 1980-01-01
        struct.pack_into("<H", e, 26, first_cluster)
        return bytes(e)

    # -- finalise -------------------------------------------------------------

    def _pack_fat(self) -> bytes:
        raw = bytearray(self.spf * BYTES_PER_SECTOR)
        for c, val in enumerate(self.fat):
            struct.pack_into("<H", raw, c * 2, val & 0xFFFF)
        return bytes(raw)

    def build(self) -> bytes:
        base = self.part_lba * BYTES_PER_SECTOR
        fat = self._pack_fat()
        for i in range(NUM_FATS):
            off = base + (self.fat_start + i * self.spf) * BYTES_PER_SECTOR
            self.image[off:off + len(fat)] = fat

        off = base + self.root_start * BYTES_PER_SECTOR
        blob = b"".join(self.dirs[""])
        self.image[off:off + len(blob)] = blob

        for name, clusters in self.dir_clusters.items():
            blob = b"".join(self.dirs[name])
            for i, c in enumerate(clusters):
                chunk = blob[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
                if not chunk:
                    break
                self.image[self._cluster_offset(c):
                           self._cluster_offset(c) + len(chunk)] = chunk
        return bytes(self.image)

    def describe(self) -> str:
        return (f"{self.total_sectors * BYTES_PER_SECTOR:,} bytes, "
                f"C/H/S {self.tracks}/{self.heads}/{HDD_SECTORS_PER_TRACK}, "
                f"FAT16 {self.cluster_bytes:,}-byte clusters, "
                f"{self.total_clusters:,} clusters")


# --- where the DOS comes from ------------------------------------------------

def describe_dos(reader: Fat12Reader, path: Path) -> str:
    """Name the DOS on an image from what is actually on it.

    The label used to be hardcoded per code path, so pointing --list-source at an
    MS-DOS image (or at one this tool produced) reported "FreeDOS" simply because
    --media had not been passed. Detection is by the system files present, which
    is the same thing the boot sectors care about.
    """
    names = {n.upper() for n in reader.listdir()}
    oem = reader.data[3:11].decode("latin1").strip()
    if {"IO.SYS", "MSDOS.SYS"} <= names:
        family = "MS-DOS"
    elif "KERNEL.SYS" in names:
        family = "FreeDOS"
    else:
        family = "no bootable DOS found on"
        return f"{family} {path} (OEM ID {oem!r})"
    return f"{family} ({path}, OEM ID {oem!r})"


class DosSource:
    """A bootable DOS to build images from: a boot sector plus system files.

    Subclasses differ only in where the bytes come from and what has to be
    written first, which is the whole reason this seam exists -- MS-DOS pins the
    first two root directory entries, FreeDOS does not.
    """

    label = "dos"

    @property
    def boot_sector(self) -> bytes:
        raise NotImplementedError

    def system_files(self) -> list[tuple[str, bytes, int]]:
        """(name, content, attr) in the exact order they must be written."""
        raise NotImplementedError

    def listing(self) -> list[tuple[str, int, str]]:
        """(name, size in bytes, which image it came from)."""
        raise NotImplementedError


class FreeDosSource(DosSource):
    """external/8086tiny/fd.img -- the shipped, redistributable default."""

    def __init__(self, path: Path):
        self.path = path
        self.reader = Fat12Reader(path)
        self.label = describe_dos(self.reader, path)

    @property
    def boot_sector(self) -> bytes:
        return self.reader.boot_sector

    def system_files(self) -> list[tuple[str, bytes, int]]:
        # KERNEL.SYS first so sequential allocation lands it at cluster 2
        # contiguously, which is what the FreeDOS boot sector wants.
        out = []
        for name in SYSTEM_FILES:
            content = self.reader.read(name)
            if content is None:
                if name in REQUIRED_SYSTEM_FILES:
                    raise DiskFullError(f"{self.path}: missing required {name}")
                continue
            out.append((name, content, ATTR_ARCHIVE))
        return out

    def listing(self):
        return [(n, -1 if is_dir else size, self.path.name)
                for n, size, is_dir in self.reader.entries()]


class MsDosSource(DosSource):
    """User-supplied MS-DOS install floppies (6.22 verified).

    Only the boot disk is mandatory -- the one carrying IO.SYS, MSDOS.SYS and
    COMMAND.COM, which on MS-DOS 6.22 is Disk 1 and carries all three
    uncompressed. Disks 2 and 3 are accepted and indexed so --util can reach
    them, but nothing on them is needed to boot.
    """

    def __init__(self, media: list[Path], utils: list[str] = (),
                 quitemu: bytes | None = None):
        if not media:
            raise DiskFullError("no MS-DOS media given (--media)")
        self.readers = {}
        self.index: dict[str, Path] = {}
        self.sizes: dict[str, int] = {}
        for p in media:
            r = Fat12Reader(p)
            self.readers[p] = r
            for n, size, is_dir in r.entries():
                if n.upper() in self.index:
                    continue
                self.index[n.upper()] = p
                self.sizes[n.upper()] = -1 if is_dir else size

        self.boot_disk = next(
            (p for p, r in self.readers.items()
             if all(r.read(n) is not None for n in MSDOS_SYSTEM_FILES)), None)
        if self.boot_disk is None:
            raise DiskFullError(
                "no disk in the given media carries all of "
                + ", ".join(MSDOS_SYSTEM_FILES)
                + " -- for MS-DOS 6.22 that is Disk 1"
            )
        self.utils = [u.upper() for u in utils]
        self.quitemu = quitemu
        self.label = describe_dos(self.readers[self.boot_disk], self.boot_disk)

    @property
    def boot_sector(self) -> bytes:
        return self.readers[self.boot_disk].boot_sector

    def _read(self, name: str) -> bytes:
        p = self.index.get(name.upper())
        if p is None:
            raise DiskFullError(
                f"{name} is not on the supplied media "
                f"(have {len(self.index)} files across {len(self.readers)} disk(s))"
            )
        content = self.readers[p].read(name)
        if content is None:
            raise DiskFullError(f"{name} is a directory, not a file")
        return content

    def system_files(self) -> list[tuple[str, bytes, int]]:
        boot = self.readers[self.boot_disk]
        out = [
            # These two, in this order, are what the boot sector name-checks at
            # root entries 0 and 1. Writing them first also makes them
            # contiguous, which MSLOAD needs.
            ("IO.SYS", boot.read("IO.SYS"), ATTR_SYSTEM_FILE),
            ("MSDOS.SYS", boot.read("MSDOS.SYS"), ATTR_SYSTEM_FILE),
            ("COMMAND.COM", boot.read("COMMAND.COM"), ATTR_ARCHIVE),
            ("CONFIG.SYS", msdos_config_sys().encode("latin1"), ATTR_ARCHIVE),
        ]
        if self.quitemu is not None:
            out.append(("QUITEMU.COM", self.quitemu, ATTR_ARCHIVE))
        for name in self.utils:
            if name in MSDOS_SYSTEM_FILES:
                continue
            if name.endswith("_"):
                print(f"  warning: {name} is SZDD/KWAJ-compressed on the install "
                      f"media and is copied as-is; run EXPAND on it inside the "
                      f"emulator (--util-set expand)", file=sys.stderr)
            out.append((name, self._read(name), ATTR_ARCHIVE))
        return out

    def listing(self):
        return sorted(
            (n, self.sizes[n], self.index[n].name) for n in self.index
        )


def msdos_config_sys() -> str:
    """CONFIG.SYS for the MS-DOS path.

    Deliberately almost empty. There is no SHELL= line: MS-DOS defaults to
    COMMAND.COM in the root of the boot drive, and hardcoding A:\\ or C:\\ would
    be a guess about how the core maps the image. HIMEM/EMM386/DOS=HIGH are all
    absent because this is an 8086 -- they need a 286/386 and would abort.
    """
    return "\r\n".join([
        "REM Generated by tools/mkdosdisk.py -- 8086, so no HIMEM/EMM386/DOS=HIGH.",
        "FILES=20",
        "BUFFERS=15",
    ]) + "\r\n"


def resolve_media(paths: list[Path] | None) -> list[Path]:
    """Turn --media (nothing / a directory / explicit images) into image paths."""
    given = list(paths) if paths else [MEDIA_DIR]
    out: list[Path] = []
    for p in given:
        if p.is_dir():
            out.extend(sorted(q for q in p.iterdir()
                              if q.is_file() and q.suffix.lower() in (".img", ".ima", ".dsk")))
        elif p.is_file():
            out.append(p)
        else:
            raise DiskFullError(f"media not found: {p}")
    if not out:
        raise DiskFullError(
            f"no floppy images in {', '.join(str(p) for p in given)} -- "
            "MS-DOS install media is user-supplied and not shipped with this repo"
        )
    sized = [p for p in out if p.stat().st_size == IMAGE_SIZE]
    if not sized:
        raise DiskFullError(
            "none of " + ", ".join(p.name for p in out)
            + f" is a {IMAGE_SIZE:,}-byte 1.44 MB floppy image"
        )
    return sized


def load_quitemu(path: Path) -> bytes | None:
    """QUITEMU.COM is an emulator escape hatch, not a FreeDOS file.

    Five bytes of `jmp far 0000:0000`, which 8086tiny traps as "power off". It
    happens to live in fd.img, so the MS-DOS path borrows it from there. Shipped
    on the disk for a user to type; not called from AUTOEXEC.BAT -- the exit path
    is retro-go's overlay, and self-quitting on program exit is wrong (see
    make_autoexec).
    """
    if not path.is_file():
        return None
    try:
        return Fat12Reader(path).read("QUITEMU.COM")
    except Exception:
        return None


# --- entry-point selection ---------------------------------------------------

def pick_entry(names: list[str]) -> str | None:
    """Choose which program AUTOEXEC.BAT should run.

    Order: .BAT first (a game shipping a launcher .BAT almost always wants it
    used), then .COM/.EXE. Within each, installer-ish names are pushed to the
    back. Ties break alphabetically so the result is deterministic.
    """
    def rank(n: str):
        stem, _, ext = n.upper().partition(".")
        ext = "." + ext
        by_ext = {".BAT": 0, ".COM": 1, ".EXE": 1}.get(ext, 9)
        deprioritised = 1 if any(stem.startswith(p) for p in ENTRY_DEPRIORITISED) else 0
        return (deprioritised, by_ext, n.upper())

    candidates = [n for n in names if n.upper().endswith(EXECUTABLE_SUFFIXES)]
    return sorted(candidates, key=rank)[0] if candidates else None


def dos_name(path: Path, taken: set[str]) -> str:
    """Coerce a filename to unique 8.3 uppercase."""
    stem = "".join(c for c in path.stem.upper() if c.isalnum() or c in "_-")[:8] or "FILE"
    ext = "".join(c for c in path.suffix[1:].upper() if c.isalnum())[:3]
    base, n = stem, 1
    while True:
        cand = f"{stem}.{ext}" if ext else stem
        if cand not in taken:
            taken.add(cand)
            return cand
        suffix = str(n)
        stem = base[: 8 - len(suffix)] + suffix
        n += 1


def make_autoexec(entry: str | None, chdir: str | None = None) -> str:
    """AUTOEXEC.BAT: run the payload, then fall back to the DOS prompt.

    It deliberately does NOT call QUITEMU.COM afterwards. That was the original
    behaviour and it was wrong: a game exiting powered the whole emulator off
    instead of returning to DOS, so quitting a program looked like a crash and
    there was no way to run anything else on the disk. Retro-go's own overlay
    (PAUSE/SET) is the exit path, so the guest needs no self-quit at all.
    QUITEMU.COM is still copied onto the disk -- 5 bytes, and occasionally handy
    to type at the prompt -- it is just not run for you.
    """
    lines = ["@echo off", "PROMPT $p$g"]
    if entry:
        lines += [
            "",
            f"REM Generated by tools/mkdosdisk.py -- runs {entry} on boot.",
        ]
        # A hard-disk image keeps the payload in a subdirectory, and DOS
        # programs of this era routinely open their data files by relative
        # path, so the CD is not cosmetic -- running \\DF\\DFDEMO.BAT from the
        # root gets you a game that cannot find its own .GOB files.
        if chdir:
            lines.append(f"CD \\{chdir.upper()}")
        lines += [
            entry.upper(),
            "",
            "REM Program exited; falling through to the DOS prompt. Use the",
            "REM retro-go overlay (PAUSE/SET) to leave the emulator.",
        ]
    else:
        lines += ["", "REM No executable to run; dropping to a prompt."]
    return "\r\n".join(lines) + "\r\n"


# --- packing -----------------------------------------------------------------

def collect_payload(src: Path,
                    reserved: set[str] | None = None
                    ) -> tuple[list[tuple[str, bytes]], list[str]]:
    """Return (files_to_write, candidate_names) for a file or directory input."""
    taken: set[str] = {f.upper() for f in (reserved or set(SYSTEM_FILES))}
    taken.update({"AUTOEXEC.BAT"})
    payload, names = [], []
    sources = sorted(p for p in src.rglob("*") if p.is_file()) if src.is_dir() else [src]
    for p in sources:
        name = dos_name(p, taken)
        payload.append((name, p.read_bytes()))
        names.append(name)
    return payload, names


def collect_payload_hdd(src: Path, reserved: set[str],
                        only: list[str] | None = None,
                        subdir: str | None = None
                        ) -> tuple[list[tuple[str, str, bytes]], list[str]]:
    """Collect a payload keeping (one level of) directory structure.

    Returns (files, entry_candidates) where each file is
    (directory or "" for the root, 8.3 name, contents), and the candidates are
    "DIR\\NAME" strings suitable for --entry.

    Why directories at all, when the floppy path happily rglob-flattens
    everything: the games that need a hard disk are the ones shipped as a
    directory tree, and at least one of them (Battle Chess) has a 33 MB
    DATA\\DATA.DAT it opens by that exact path. Flattening is not an option.

    Only one level is mirrored. Anything deeper is flattened into its top-level
    directory, which is what the DOS-era installers for these titles did anyway,
    and it keeps the directory-sizing in Fat16Image.mkdir a single decision.

    --only restricts the copy to named top-level entries. That is not a nicety:
    Battle Chess ships a 43 MB DEMOS/ tree next to the 33 MB of game data, and
    copying both would need a 94.5 MB image to hold content nobody runs.
    """
    if not src.is_dir():
        content = src.read_bytes()
        name = dos_name(src, set(reserved) | {"AUTOEXEC.BAT"})
        return [(subdir or "", name, content)], [name]

    wanted = sorted(p for p in src.iterdir() if not p.name.startswith("."))
    if only:
        keep = {o.upper().strip("\\/") for o in only}
        wanted = [p for p in wanted if p.name.upper() in keep]
        missing = keep - {p.name.upper() for p in wanted}
        if missing:
            raise DiskFullError(f"{src}: --only {', '.join(sorted(missing))} not found")

    files: list[tuple[str, str, bytes]] = []
    names: list[str] = []
    taken: dict[str, set[str]] = {}
    dir_names: set[str] = set()

    def place(directory: str, path: Path):
        # Names only have to be unique within their own directory; the root's
        # starts out already holding the system files so a payload file cannot
        # collide with IO.SYS and quietly replace it.
        seen = taken.setdefault(
            directory,
            (set(reserved) | {"AUTOEXEC.BAT"}) if directory == "" else set())
        name = dos_name(path, seen)
        files.append((directory, name, path.read_bytes()))
        names.append(f"{directory}\\{name}" if directory else name)

    for p in wanted:
        if p.is_file():
            place(subdir or "", p)
        elif p.is_dir():
            # Directory names are 8 characters, no extension -- DOS allows an
            # extension on a directory but no tool of the era expects one.
            dname = dos_name(Path(p.name.split(".")[0]), dir_names)
            if subdir:
                raise DiskFullError(
                    f"{src}: --subdir cannot be combined with a payload that "
                    f"already has directories ({p.name})")
            for q in sorted(x for x in p.rglob("*") if x.is_file()):
                place(dname, q)
    return files, names


def auto_hdd_sectors(content_bytes: int) -> int:
    """Pick an image size for a payload, erring generous.

    Free space on a DOS hard disk is not decoration: games write savegames and
    config files back, and a full disk fails those writes silently. A quarter of
    the payload plus a megabyte is the rule here, and it is then rounded up to
    something the BIOS can address, which past 31.5 MB is a big jump (see the
    dead zone). Pass --size when you want a specific number -- the three images
    in roms/dos were sized by hand and are reproduced by passing their exact
    byte counts.
    """
    want = int(content_bytes * 1.25) + (1 << 20)
    return hdd_round_up((want + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
                        + HDD_PART_LBA)


def parse_size(text: str) -> int:
    """--size, in bytes or with a K/M suffix, rounded up to a valid geometry."""
    t = text.strip().upper()
    mult = 1
    if t.endswith("K"):
        mult, t = 1 << 10, t[:-1]
    elif t.endswith("M"):
        mult, t = 1 << 20, t[:-1]
    try:
        raw = int(float(t) * mult)
    except ValueError:
        raise DiskFullError(f"--size {text!r} is not a size")
    return hdd_round_up((raw + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR)


def pack_hdd(src: Path | None, dst: Path, source: MsDosSource,
             entry_override: str | None, verbose: bool = True,
             out_name: str | None = None, size_sectors: int | None = None,
             only: list[str] | None = None, subdir: str | None = None) -> Path:
    """Write one partitioned FAT16 .dsk image.

    The write order is the whole game and is the same one the floppy path uses:
    IO.SYS and MSDOS.SYS first, so they are root entries 0 and 1 (the boot
    sector name-checks them) and so sequential allocation puts IO.SYS at cluster
    2, contiguous (MSLOAD reads it with an LBA++ loop). Directories are created
    before their contents for the same allocation reason.
    """
    if not isinstance(source, MsDosSource):
        raise DiskFullError(
            "hard-disk images need MS-DOS (--media): the FreeDOS boot sector on "
            "fd.img is a floppy one and is not transplanted here")

    system = source.system_files()
    reserved = {n for n, _, _ in system}
    if src is None:
        files, names = [], []
    else:
        files, names = collect_payload_hdd(src, reserved, only, subdir)
        if not files:
            raise DiskFullError(f"{src}: nothing to pack")

    # --entry may be given as "GAME.EXE" or "DIR\\GAME.EXE"; a bare name is
    # accepted when it is unambiguous, so --entry DFDEMO.BAT works even though
    # the file ends up in \DF.
    entry_dir, entry = "", None
    if entry_override:
        want = entry_override.upper().replace("/", "\\").lstrip("\\")
        match = [n for n in names if n.upper() == want]
        if not match:
            match = [n for n in names if n.upper().rsplit("\\", 1)[-1] == want]
        if len(match) != 1:
            raise DiskFullError(
                f"{src}: --entry {entry_override} "
                + ("is ambiguous" if match else "not found")
                + f" (have: {', '.join(names)})")
        entry_dir, _, entry = match[0].rpartition("\\")
    else:
        picked = pick_entry([n.rsplit("\\", 1)[-1] for n in names])
        if picked:
            full = next(n for n in names if n.rsplit("\\", 1)[-1] == picked)
            entry_dir, _, entry = full.rpartition("\\")

    autoexec = make_autoexec(entry, entry_dir or None).encode("latin1")

    content = (sum(len(c) for _, c, _ in system) + len(autoexec)
               + sum(len(c) for _, _, c in files))
    total = size_sectors or auto_hdd_sectors(content)

    # Retry once a size up if the first guess does not fit: cluster slack and
    # the FAT itself are not in the estimate, and a build that dies on the last
    # file after ten seconds of copying is a poor way to find that out.
    for attempt in range(4):
        img = Fat16Image(total, source.boot_sector)
        try:
            for name, data, attr in system:
                img.add(name, data, attr)
            img.add("AUTOEXEC.BAT", autoexec)

            # One directory at a time, each created immediately before its own
            # contents. Creating all the directories up front would work just as
            # well, but this keeps each directory's cluster adjacent to the
            # files in it, which is both what mformat/mcopy produce and kinder
            # to a 1990s seek pattern.
            per_dir: dict[str, list[tuple[str, bytes]]] = {}
            for directory, name, data in files:
                per_dir.setdefault(directory, []).append((name, data))
            for directory, contents in per_dir.items():
                if directory:
                    img.mkdir(directory, len(contents))
                for name, data in contents:
                    img.add(name, data, ATTR_ARCHIVE, directory)
            break
        except DiskFullError:
            if size_sectors is not None or attempt == 3:
                raise
            total = hdd_round_up(total + 1)
    _assert_hdd_boot_layout(img)

    stem = out_name if out_name is not None else (
        src.stem if src.is_file() else src.name)
    out = dst / f"{stem}.dsk"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(img.build())

    if verbose:
        used = img.total_clusters * img.cluster_bytes - img.free_bytes
        shown = ("\\" + entry_dir + "\\" if entry_dir else "") + (entry or "<prompt>")
        print(f"  {out.name}: {len(files)} file(s), entry={shown}, "
              f"{img.describe()}, {used:,} bytes used")
    return out


def _assert_hdd_boot_layout(img: Fat16Image) -> None:
    """Check the two things that make an image boot rather than mount.

    Both failures are silent -- the image mounts perfectly under mtools either
    way -- so they are asserted here rather than discovered on hardware.
    """
    root = img.dirs[""]
    names = [e[0:11].decode("latin1") for e in root[:2]]
    if names != ["IO      SYS", "MSDOS   SYS"]:
        raise DiskFullError(
            f"root entries 0/1 are {names}, must be IO.SYS then MSDOS.SYS")
    if struct.unpack_from("<H", root[0], 26)[0] != 2:
        raise DiskFullError("IO.SYS does not start at cluster 2")
    for e in root[:2]:
        first = struct.unpack_from("<H", e, 26)[0]
        c = first
        while img.fat[c] != 0xFFFF:
            if img.fat[c] != c + 1:
                raise DiskFullError(
                    f"{e[0:11].decode('latin1')} is fragmented; MSLOAD cannot load it")
            c = img.fat[c]


def pack(src: Path | None, dst: Path, source: DosSource, entry_override: str | None,
         verbose: bool = True, verify: bool = False,
         out_name: str | None = None) -> Path:
    """Write one .dsk. `src is None` builds a bare bootable disk (see --bare)."""
    system = source.system_files()
    if src is None:
        payload, names = [], []
    else:
        payload, names = collect_payload(src, {n for n, _, _ in system})
        if not payload:
            raise DiskFullError(f"{src}: nothing to pack")

    entry = entry_override.upper() if entry_override else pick_entry(names)
    if entry_override and entry not in {n.upper() for n in names}:
        raise DiskFullError(
            f"{src}: --entry {entry_override} not found (have: {', '.join(names)})"
        )

    img = Fat12Image(source.boot_sector)

    # System files before anything else. Sequential allocation then makes the
    # first of them start at cluster 2 and every one of them contiguous, which
    # is what both boot sectors need (KERNEL.SYS for FreeDOS; IO.SYS + MSDOS.SYS
    # as root entries 0 and 1 for MS-DOS).
    for name, content, attr in system:
        img.add(name, content, attr)

    autoexec = make_autoexec(entry).encode("latin1")
    img.add("AUTOEXEC.BAT", autoexec)
    for name, content in payload:
        img.add(name, content)

    if out_name is not None:
        stem = out_name
    else:
        stem = src.stem if src.is_file() else src.name
    out = dst / f"{stem}.dsk"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(img.build())

    if verbose:
        used = IMAGE_SIZE - img.free_bytes
        print(f"  {out.name}: {len(payload)} file(s), entry={entry or '<prompt>'}, "
              f"{used:,}/{IMAGE_SIZE:,} bytes used")

    if verify:
        expect = [(n, c) for n, c, _ in system]
        expect.append(("AUTOEXEC.BAT", autoexec))
        expect.extend(payload)
        for line in verify_image(out, source, expect):
            print(f"    ok: {line}")
    return out


# --- structural verification -------------------------------------------------

def _chain(reader: Fat12Reader, first: int) -> list[int]:
    out, c = [], first
    while 2 <= c < 0xFF0:
        out.append(c)
        c = reader._fat_entry(c)
    return out


def verify_image(path: Path, source: DosSource,
                 expect: list[tuple[str, bytes]]) -> list[str]:
    """Re-open a written .dsk and check every structural claim we make.

    This exists because the failure mode here is silent: a FAT12 image with the
    wrong entry order or a fragmented IO.SYS mounts perfectly and simply does
    not boot. Returns a list of human-readable results; raises on any failure.
    """
    results = []
    r = Fat12Reader(path)

    if r.boot_sector != source.boot_sector:
        raise DiskFullError(f"{path.name}: boot sector differs from the DOS source")
    results.append("boot sector byte-identical to source")

    Fat12Image(r.boot_sector)  # re-runs _assert_geometry against the written BPB
    results.append("BPB matches the layout written")

    base = r.root_start * r.bps
    order = []
    for i in range(r.root_entries):
        e = r.data[base + i * 32: base + i * 32 + 32]
        if e[0] == 0x00:
            break
        stem, ext = e[0:8].decode("latin1").strip(), e[8:11].decode("latin1").strip()
        order.append((f"{stem}.{ext}" if ext else stem, e[11],
                      struct.unpack_from("<H", e, 26)[0]))

    boot_file = "IO.SYS" if isinstance(source, MsDosSource) else "KERNEL.SYS"
    if order[0][0] != boot_file:
        raise DiskFullError(
            f"{path.name}: root entry 0 is {order[0][0]}, must be {boot_file}")
    if isinstance(source, MsDosSource):
        if order[1][0] != "MSDOS.SYS":
            raise DiskFullError(
                f"{path.name}: root entry 1 is {order[1][0]}, must be MSDOS.SYS "
                "(the boot sector name-checks it)")
        results.append("root entries 0/1 are IO.SYS, MSDOS.SYS -- "
                       f"attrs 0x{order[0][1]:02X}, 0x{order[1][1]:02X}")
    else:
        results.append(f"root entry 0 is {boot_file}")

    for name, _, first in order[:2 if isinstance(source, MsDosSource) else 1]:
        chain = _chain(r, first)
        if chain != list(range(chain[0], chain[0] + len(chain))):
            raise DiskFullError(f"{path.name}: {name} is fragmented")
        results.append(f"{name} contiguous, {len(chain)} clusters at {chain[0]}")
    if order[0][2] != 2:
        raise DiskFullError(f"{path.name}: {boot_file} starts at cluster "
                            f"{order[0][2]}, expected 2")

    for name, content in expect:
        got = r.read(name)
        if got is None or got != content:
            raise DiskFullError(f"{path.name}: {name} does not round-trip")
    results.append(f"{len(expect)} file(s) round-trip byte-exact")

    try:
        from pyfatfs.PyFatFS import PyFatFS  # noqa: PLC0415
    except ImportError:
        results.append("pyfatfs not installed -- cross-check skipped")
        return results
    fs = PyFatFS(str(path), read_only=True)
    try:
        listed = {n.upper() for n in fs.listdir("/")}
        missing = {n for n, _ in expect} - listed
        if missing:
            raise DiskFullError(
                f"{path.name}: pyfatfs cannot see {', '.join(sorted(missing))}")
        for name, content in expect:
            with fs.openbin("/" + name) as fh:
                if fh.read() != content:
                    raise DiskFullError(
                        f"{path.name}: pyfatfs read-back of {name} differs")
        results.append(f"pyfatfs mounts it and reads all {len(listed)} entries back")
    finally:
        fs.close()
    return results


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack loose DOS executables or game directories into bootable "
                    ".dsk floppy images."
    )
    parser.add_argument("inputs", nargs="*", type=Path,
                        help="Specific files/directories to pack (default: everything in --src)")
    parser.add_argument("--src", type=Path, default=Path("roms/dos"),
                        help="Directory to scan for inputs (default: roms/dos)")
    parser.add_argument("--dst", type=Path, default=None,
                        help="Where to write .dsk images (default: same as --src)")
    parser.add_argument("--template", type=Path, default=TEMPLATE,
                        help=f"Bootable FreeDOS floppy to take system files from (default: {TEMPLATE})")
    parser.add_argument("--system", choices=("freedos", "msdos"), default=None,
                        help="Which DOS to put on the image. Defaults to msdos "
                             "when --media is given, freedos otherwise.")
    parser.add_argument("--media", type=Path, nargs="*", default=None,
                        metavar="PATH",
                        help="MS-DOS install floppies: a directory of .img files "
                             f"or the images themselves (default: {MEDIA_DIR})")
    parser.add_argument("--util", action="append", default=[], metavar="NAME",
                        help="Extra file to copy off the MS-DOS media, e.g. "
                             "--util EDIT.COM (repeatable)")
    parser.add_argument("--util-set", choices=sorted(MSDOS_UTIL_SETS), default="none",
                        help="Named group of MS-DOS utilities to include "
                             "(default: none -- every cluster goes to the payload)")
    parser.add_argument("--entry", type=str, default=None,
                        help="Executable AUTOEXEC.BAT should run (only valid with a single input)")
    parser.add_argument("--bare", action="store_true",
                        help="Build one bootable disk with no payload at all, landing "
                             "at a DOS prompt. Name it with --name (default: DOS).")
    parser.add_argument("--name", type=str, default="DOS", metavar="NAME",
                        help="Output stem for --bare, i.e. NAME.dsk (default: DOS)")
    parser.add_argument("--hdd", action="store_true",
                        help="Build a partitioned FAT16 hard-disk image instead of a "
                             "1.44 MB floppy. Selected automatically when the payload "
                             "does not fit a floppy. Needs --media (MS-DOS).")
    parser.add_argument("--size", type=str, default=None, metavar="BYTES",
                        help="Hard-disk image size, e.g. 63M or 66060288. Rounded UP "
                             "to a size 8086tiny's BIOS can address -- note the "
                             "31.5 MB..63 MB dead zone. Default: payload + 25%% + 1 MB.")
    parser.add_argument("--only", action="append", default=[], metavar="NAME",
                        help="Hard-disk mode: copy only these top-level entries of the "
                             "input directory (repeatable), e.g. --only CDCHESS")
    parser.add_argument("--subdir", type=str, default=None, metavar="DIR",
                        help="Hard-disk mode: put a flat payload in \\DIR instead of "
                             "the root, e.g. --subdir DF")
    parser.add_argument("--verify", action="store_true",
                        help="Re-open each written image and check it structurally")
    parser.add_argument("--list-template", "--list-source", dest="list_template",
                        action="store_true",
                        help="List what the selected DOS source contains, and exit")
    args = parser.parse_args()

    system = args.system or ("msdos" if args.media is not None else "freedos")

    try:
        if system == "msdos":
            media = resolve_media(args.media)
            utils = list(MSDOS_UTIL_SETS[args.util_set]) + args.util
            unknown = [u.upper() for u in utils
                       if u.upper() not in MSDOS_UTILS and not u.upper().endswith("_")]
            if unknown:
                print(f"note: {', '.join(unknown)} not in the known-uncompressed "
                      f"list; copying verbatim if present", file=sys.stderr)
            # QUITEMU.COM is deliberately left off hard-disk images: it is a
            # 5-byte curiosity, the real exit path is retro-go's overlay, and
            # keeping the root directory to exactly what boots makes an image
            # comparable against the hand-built ones it replaces.
            quitemu = None if args.hdd else load_quitemu(args.template)
            source: DosSource = MsDosSource(media, utils, quitemu)
        else:
            if not args.template.is_file():
                print(f"error: template not found: {args.template}", file=sys.stderr)
                return 1
            source = FreeDosSource(args.template)
    except DiskFullError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.list_template:
        listing = source.listing()
        multi = len({where for _, _, where in listing}) > 1
        print(f"{source.label}:")
        for name, size, where in listing:
            shown = "<DIR>" if size < 0 else f"{size:,}"
            print(f"  {name:14s} {shown:>12s}" + (f"  {where}" if multi else ""))
        return 0

    dst = args.dst or args.src

    # A plain bootable disk that lands at a prompt is a legitimate product: it is
    # how you find out whether a DOS variant boots at all before blaming a game,
    # and it is what someone naming an image "msdos622.dsk" actually wants. It has
    # no input to take its name from, hence --name.
    if args.bare:
        if args.inputs:
            print("error: --bare takes no inputs", file=sys.stderr)
            return 1
        if args.entry:
            print("error: --bare has nothing to run, so --entry is meaningless",
                  file=sys.stderr)
            return 1
        if args.hdd:
            # A bootable disk with nothing on it is for checking that a DOS
            # variant boots; the floppy answers that question and is 1.4 MB
            # rather than tens of megabytes of zeroes.
            print("error: --bare builds a floppy; --hdd has no payload to size for",
                  file=sys.stderr)
            return 1
        stem = "".join(c for c in args.name if c.isalnum() or c in "._-")
        if not stem:
            print(f"error: --name {args.name!r} has no usable characters",
                  file=sys.stderr)
            return 1
        print(f"Building a bare bootable disk using {source.label}:")
        try:
            pack(None, dst, source, None, verify=args.verify, out_name=stem)
        except DiskFullError as e:
            print(f"  {e}", file=sys.stderr)
            return 1
        return 0

    if args.inputs:
        inputs = args.inputs
    else:
        if not args.src.is_dir():
            print(f"error: {args.src} is not a directory", file=sys.stderr)
            return 1
        inputs = sorted(
            p for p in args.src.iterdir()
            if (p.is_dir() and not p.name.startswith("."))
            or (p.is_file() and p.suffix.upper() in EXECUTABLE_SUFFIXES)
        )

    if not inputs:
        print(f"Nothing to pack in {args.src} "
              f"(looking for directories or {', '.join(EXECUTABLE_SUFFIXES)} files)")
        return 0
    if args.entry and len(inputs) > 1:
        print("error: --entry only makes sense with a single input", file=sys.stderr)
        return 1

    if (args.only or args.subdir or args.size) and not args.hdd:
        print("note: --only/--subdir/--size are hard-disk options; implying --hdd",
              file=sys.stderr)
        args.hdd = True

    try:
        size_sectors = parse_size(args.size) if args.size else None
    except DiskFullError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"Packing {len(inputs)} input(s) using {source.label}:")
    failures = 0
    for src in inputs:
        if not src.exists():
            print(f"  {src}: does not exist", file=sys.stderr)
            failures += 1
            continue
        try:
            # A payload that cannot fit a floppy is not a failure to report, it
            # is a hard disk: the alternative is telling the user their game is
            # too big and making them find the flag themselves.
            payload_bytes = sum(p.stat().st_size for p in src.rglob("*")
                                if p.is_file()) if src.is_dir() else src.stat().st_size
            # DOS itself is ~135 KB of the floppy, so the payload has to be
            # measured against what is left, not against the whole 1.44 MB.
            floppy_free = (TOTAL_CLUSTERS * SECTORS_PER_CLUSTER * BYTES_PER_SECTOR
                           - sum(len(c) for _, c, _ in source.system_files()))
            if args.hdd or payload_bytes > floppy_free:
                pack_hdd(src, dst, source, args.entry, out_name=None,
                         size_sectors=size_sectors, only=args.only,
                         subdir=args.subdir)
            else:
                pack(src, dst, source, args.entry, verify=args.verify)
        except DiskFullError as e:
            print(f"  {e}", file=sys.stderr)
            failures += 1

    if failures:
        print(f"\n{failures} input(s) failed.", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
