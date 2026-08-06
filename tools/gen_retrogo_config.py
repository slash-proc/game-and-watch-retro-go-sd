#!/usr/bin/env python3
"""Fabricate retro-go's persisted /CONFIG so the firmware boots straight into a ROM.

retro-go keeps its settings in a single fixed-layout struct written to the file
`/CONFIG` in the root of the SD card (Core/Src/porting/odroid_settings.c,
`persistent_config_t`).  At boot, `app_main()` (Core/Src/retro-go/rg_main.c)
reads `startup_file` out of it and, if that path resolves to a ROM that is
actually present, calls `emulator_start()` on it instead of showing the
launcher.  Writing that field by hand is therefore enough to make an unattended
boot land in a chosen game.

The struct is validated by magic + version + CRC32, so it cannot simply be
patched byte-wise without recomputing the CRC -- that is what this script is
for.

Usage (typical, against a gwemu SD image):

    tools/gen_retrogo_config.py --sd-image build/sdcard.img \
        --rom /roms/dos/TOPBENCH.dsk --install

    # or write the blob and install it yourself
    tools/gen_retrogo_config.py --rom /roms/dos/TOPBENCH.dsk -o CONFIG

The ROM path is checked against the SD image (or against --roms-dir for a real
card mounted somewhere).  A config naming a ROM that is not present is refused,
because the firmware's failure mode for that is silent: it falls through to the
launcher and you get a menu instead of a game, with nothing in the log saying
why.  Pass --force only if you are deliberately building a negative control.
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib

# --- persistent_config_t, as laid out by arm-none-eabi-gcc -------------------
# Verified against the DWARF in build/gw_retro_go.elf:
#   arm-none-eabi-gdb -batch build/gw_retro_go.elf -ex 'ptype /o struct persistent_config'
#
#   off  size  field
#     0     4  uint32_t magic          0xcafef00d
#     4     1  uint8_t  version        8
#     5     1  uint8_t  backlight
#     6     1  uint8_t  start_action   (dead field, never read)
#     7     1  uint8_t  volume
#     8     1  uint8_t  font_size
#     9     1  uint8_t  theme
#    10     1  uint8_t  colors
#    11     1  uint8_t  turbo_buttons
#    12     1  uint8_t  font
#    13     1  uint8_t  lang
#    14     1  uint8_t  startup_app    (dead field, never read at boot)
#    15     1  uint8_t  cpu_oc_level
#    16   256  char     startup_file[256]   <-- the field that selects the game
#   272     2  uint16_t main_menu_timeout_s
#   274     2  uint16_t main_menu_selected_tab
#   276     2  uint16_t main_menu_cursor
#   278    96  char     main_menu_browse_subpath[96]
#   374     1  bool     debug_clock_always_on   (+1 byte hole)
#   376     4  uint32_t welcome_prompt
#   380   6*N  app_config_t app[APPID_COUNT]    (+padding to 4)
#   520     4  uint32_t crc32
#  total  524  (with APPID_COUNT == 23)
#
# CRC32 is zlib crc32 (reflected, poly 0xedb88320, init/final inverted) over the
# entire struct with the crc32 field zeroed -- Core/Src/porting/crc32.c
# crc32_le(0, ...) is bit-identical to zlib.crc32().

MAGIC = 0xCAFEF00D
VERSION = 8
SIZE = 524
OFF_STARTUP_FILE = 16
OFF_CRC = 520

DEFAULTS = {
    "backlight": 6,       # ODROID_BACKLIGHT_LEVEL6
    "volume": 4,          # ODROID_AUDIO_VOLUME_MAX / 2
    "font_size": 8,
    "theme": 2,
    "main_menu_timeout_s": 600,
    "welcome_prompt": 1,  # 1 = "welcome message already shown"; keeps it off screen
}

PART_OFFSET = 1048576  # partition 1 of a gwemu/gnwmanager SD image: LBA 2048


def build_config(rom_path: str, *, selected_tab: int = 0, cursor: int = 0,
                 bad_magic: bool = False, bad_crc: bool = False) -> bytes:
    buf = bytearray(SIZE)
    struct.pack_into("<I", buf, 0, 0xDEADBEEF if bad_magic else MAGIC)
    buf[4] = VERSION
    buf[5] = DEFAULTS["backlight"]
    buf[6] = 0
    buf[7] = DEFAULTS["volume"]
    buf[8] = DEFAULTS["font_size"]
    buf[9] = DEFAULTS["theme"]
    # colors/turbo/font/lang/startup_app/cpu_oc_level all stay 0

    enc = rom_path.encode()
    if len(enc) > 255:
        sys.exit("error: rom path longer than startup_file[256]")
    buf[OFF_STARTUP_FILE:OFF_STARTUP_FILE + len(enc)] = enc

    struct.pack_into("<H", buf, 272, DEFAULTS["main_menu_timeout_s"])
    struct.pack_into("<H", buf, 274, selected_tab)
    struct.pack_into("<H", buf, 276, cursor)
    struct.pack_into("<I", buf, 376, DEFAULTS["welcome_prompt"])

    crc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF
    if bad_crc:
        crc ^= 0xFFFFFFFF
    struct.pack_into("<I", buf, OFF_CRC, crc)
    return bytes(buf)


def sd_file_exists(image: str, path: str, part_offset: int) -> bool:
    """Ask mtools whether `path` exists in the FAT filesystem of `image`."""
    target = "::" + path.lstrip("/")
    # `mdir <file>` lists exactly that entry and exits non-zero with
    # 'File "..." not found' when it is absent. It matches long names, and FAT
    # lookups are case-insensitive -- so is FatFs on the device, so that is the
    # same matching the firmware will do.
    out = subprocess.run(["mdir", "-i", f"{image}@@{part_offset}", target],
                         capture_output=True, text=True)
    return out.returncode == 0 and " 1 file" in out.stdout


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True,
                    help="absolute on-card path of the ROM, e.g. /roms/dos/TOPBENCH.dsk")
    ap.add_argument("-o", "--output", help="write the 524-byte blob here")
    ap.add_argument("--sd-image", help="gwemu SD image to validate against / install into")
    ap.add_argument("--part-offset", type=int, default=PART_OFFSET,
                    help=f"byte offset of the FAT partition (default {PART_OFFSET})")
    ap.add_argument("--roms-dir",
                    help="host directory that mirrors the card root, used to validate "
                         "--rom when no --sd-image is given (e.g. a mounted card)")
    ap.add_argument("--install", action="store_true",
                    help="mcopy the result to ::/CONFIG in --sd-image")
    ap.add_argument("--selected-tab", type=int, default=0)
    ap.add_argument("--cursor", type=int, default=0)
    ap.add_argument("--force", action="store_true",
                    help="skip the ROM-presence check (only for negative controls)")
    ap.add_argument("--bad-magic", action="store_true", help="negative control: corrupt magic")
    ap.add_argument("--bad-crc", action="store_true", help="negative control: corrupt CRC32")
    args = ap.parse_args()

    if not args.rom.startswith("/roms/"):
        sys.exit("error: --rom must be an absolute card path under /roms/ "
                 "(emulator_get_file() only matches paths of that shape)")

    if not args.force:
        present = None
        if args.sd_image:
            present = sd_file_exists(args.sd_image, args.rom, args.part_offset)
        elif args.roms_dir:
            present = os.path.isfile(os.path.join(args.roms_dir,
                                                  args.rom.lstrip("/")))
        if present is None:
            sys.exit("error: pass --sd-image or --roms-dir so the ROM can be "
                     "verified present, or --force to skip the check")
        if not present:
            sys.exit(f"error: {args.rom} is not on the card. The firmware would "
                     f"silently fall back to the launcher. Refusing to write.")

    blob = build_config(args.rom, selected_tab=args.selected_tab, cursor=args.cursor,
                        bad_magic=args.bad_magic, bad_crc=args.bad_crc)

    if args.output:
        with open(args.output, "wb") as f:
            f.write(blob)
        print(f"wrote {args.output} ({len(blob)} bytes) startup_file={args.rom!r}")

    if args.install:
        if not args.sd_image:
            sys.exit("error: --install needs --sd-image")
        tmp = (args.output or "CONFIG.tmp")
        if not args.output:
            with open(tmp, "wb") as f:
                f.write(blob)
        subprocess.run(["mcopy", "-o", "-i",
                        f"{args.sd_image}@@{args.part_offset}", tmp, "::/CONFIG"],
                       check=True)
        if not args.output:
            os.unlink(tmp)
        print(f"installed ::/CONFIG into {args.sd_image} startup_file={args.rom!r}")

    if not args.output and not args.install:
        sys.stdout.buffer.write(blob)


if __name__ == "__main__":
    main()
