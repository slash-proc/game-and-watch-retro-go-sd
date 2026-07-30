#!/usr/bin/env python3
"""Pack loose DOS software into bootable FAT12 floppy images (.dsk).

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


def make_autoexec(entry: str | None) -> str:
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
            source: DosSource = MsDosSource(media, utils, load_quitemu(args.template))
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

    print(f"Packing {len(inputs)} input(s) using {source.label}:")
    failures = 0
    for src in inputs:
        if not src.exists():
            print(f"  {src}: does not exist", file=sys.stderr)
            failures += 1
            continue
        try:
            pack(src, dst, source, args.entry, verify=args.verify)
        except DiskFullError as e:
            print(f"  {e}", file=sys.stderr)
            failures += 1

    if failures:
        print(f"\n{failures} input(s) failed.", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
