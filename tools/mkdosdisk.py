#!/usr/bin/env python3
"""Pack loose DOS software into bootable FAT12 floppy images (.dsk).

The MS-DOS core boots a disk image, not a bare executable, so a user with a
plain CAT.EXE has no way to launch anything. This turns either shape of input
into a bootable disk:

    roms/dos/CAT.EXE              -> roms/dos/CAT.dsk
    roms/dos/PRINCE_OF_PERSIA/    -> roms/dos/PRINCE_OF_PERSIA.dsk

The DOS system files are lifted out of external/8086tiny/fd.img, a bootable
FreeDOS floppy shipped with the emulator (MIT/GPL-clean, unlike any game).
A generated AUTOEXEC.BAT runs the target so launching drops straight into the
program instead of a C:\\> prompt.

Usage:
    python3 tools/mkdosdisk.py                     # pack everything in roms/dos
    python3 tools/mkdosdisk.py --src roms/dos --dst roms/dos
    python3 tools/mkdosdisk.py --entry GAME.EXE PRINCE_OF_PERSIA
    python3 tools/mkdosdisk.py --list-template     # show what fd.img contains

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

# Files copied out of the template to make the image bootable. KERNEL.SYS and
# COMMAND.COM are mandatory; CONFIG.SYS sets SHELL= and is what makes
# COMMAND.COM load. QUITEMU.COM is 5 bytes and exits the emulator, which is
# genuinely useful on a device with no other way out.
SYSTEM_FILES = ("KERNEL.SYS", "COMMAND.COM", "CONFIG.SYS", "QUITEMU.COM")
REQUIRED_SYSTEM_FILES = ("KERNEL.SYS", "COMMAND.COM")

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
        out = []
        for e in self._root_entries_raw():
            name, ext = e[0:8].decode("latin1").strip(), e[8:11].decode("latin1").strip()
            out.append(f"{name}.{ext}" if ext else name)
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

    def add(self, name: str, content: bytes):
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

        self.entries.append(self._dir_entry(name, first, len(content)))

    @staticmethod
    def _dir_entry(name: str, first_cluster: int, size: int) -> bytes:
        stem, _, ext = name.upper().partition(".")
        e = bytearray(32)
        e[0:8] = stem[:8].ljust(8).encode("latin1")
        e[8:11] = ext[:3].ljust(3).encode("latin1")
        e[11] = 0x20  # archive
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
    lines = ["@echo off", "PROMPT $p$g"]
    if entry:
        lines += [
            "",
            f"REM Generated by tools/mkdosdisk.py -- runs {entry} on boot.",
            entry.upper(),
            "",
            "REM Program exited. QUITEMU powers the emulator down; without it",
            "REM there is no way off this screen on a Game & Watch.",
            "QUITEMU.COM",
        ]
    else:
        lines += ["", "REM No executable found; dropping to a prompt."]
    return "\r\n".join(lines) + "\r\n"


# --- packing -----------------------------------------------------------------

def collect_payload(src: Path) -> tuple[list[tuple[str, bytes]], list[str]]:
    """Return (files_to_write, candidate_names) for a file or directory input."""
    taken: set[str] = {f.upper() for f in SYSTEM_FILES}
    taken.update({"AUTOEXEC.BAT"})
    payload, names = [], []
    sources = sorted(p for p in src.rglob("*") if p.is_file()) if src.is_dir() else [src]
    for p in sources:
        name = dos_name(p, taken)
        payload.append((name, p.read_bytes()))
        names.append(name)
    return payload, names


def pack(src: Path, dst: Path, template: Fat12Reader, entry_override: str | None,
         verbose: bool = True) -> Path:
    payload, names = collect_payload(src)
    if not payload:
        raise DiskFullError(f"{src}: nothing to pack")

    entry = entry_override.upper() if entry_override else pick_entry(names)
    if entry_override and entry not in {n.upper() for n in names}:
        raise DiskFullError(
            f"{src}: --entry {entry_override} not found (have: {', '.join(names)})"
        )

    img = Fat12Image(template.boot_sector)

    # KERNEL.SYS first: written before anything else so it lands contiguously at
    # the start of the data area, satisfying boot sectors that require it.
    for name in SYSTEM_FILES:
        content = template.read(name)
        if content is None:
            if name in REQUIRED_SYSTEM_FILES:
                raise DiskFullError(f"{TEMPLATE}: missing required {name}")
            continue
        img.add(name, content)

    img.add("AUTOEXEC.BAT", make_autoexec(entry).encode("latin1"))
    for name, content in payload:
        img.add(name, content)

    out = dst / f"{src.stem if src.is_file() else src.name}.dsk"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(img.build())

    if verbose:
        used = IMAGE_SIZE - img.free_bytes
        print(f"  {out.name}: {len(payload)} file(s), entry={entry or '<prompt>'}, "
              f"{used:,}/{IMAGE_SIZE:,} bytes used")
    return out


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
    parser.add_argument("--entry", type=str, default=None,
                        help="Executable AUTOEXEC.BAT should run (only valid with a single input)")
    parser.add_argument("--list-template", action="store_true",
                        help="List the template's root directory and exit")
    args = parser.parse_args()

    if not args.template.is_file():
        print(f"error: template not found: {args.template}", file=sys.stderr)
        return 1
    template = Fat12Reader(args.template)

    if args.list_template:
        print(f"{args.template}:")
        for n in template.listdir():
            c = template.read(n)
            print(f"  {n:14s} {'<dir>' if c is None else f'{len(c):,} bytes'}")
        return 0

    dst = args.dst or args.src

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

    print(f"Packing {len(inputs)} input(s) using {args.template}:")
    failures = 0
    for src in inputs:
        if not src.exists():
            print(f"  {src}: does not exist", file=sys.stderr)
            failures += 1
            continue
        try:
            pack(src, dst, template, args.entry)
        except DiskFullError as e:
            print(f"  {e}", file=sys.stderr)
            failures += 1

    if failures:
        print(f"\n{failures} input(s) failed.", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
