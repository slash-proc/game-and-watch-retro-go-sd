#!/usr/bin/env python3
"""Fail the link if an XIP sentinel scan would rewrite a real instruction.

WHY THIS EXISTS
---------------
Three cores ship a "cold code" blob linked at a fake address and relocated at
runtime: PICO-8 (0xBEEF0000), GBA (0xDEC00000) and MS-DOS (0xDED00000). None of
them is position-independent. Instead, a boot-time pass walks the overlay word
by word and adds an offset to every word that falls inside the sentinel window
(see patch_dos_sentinels() in Core/Src/porting/dos/main_dos.c).

That pass is a BLIND WORD SCAN OVER CODE. It cannot tell a relocatable literal
from two adjacent 16-bit Thumb instructions that happen to spell a sentinel. On
Thumb, a 4-byte word read at an odd halfword boundary is

    word = low_instruction | (high_instruction << 16)

so a false positive needs the SECOND instruction to encode exactly the
sentinel's high halfword.

It happened. DOS_CODE was 0xD05C0000, and 0xD05C is `beq.n` — one of the
commonest instructions gcc emits. In dos_cpu_init()'s hard-disk branch,
`cmp r0,#0` (0x2800) followed by `beq.n` (0xD05C) formed 0xD05C2800; the pass
relocated it; every hard-disk .dsk image executed a patched-over NULL guard and
faulted. Floppy images take a different branch and were fine, so it looked
title-specific for a while.

The fix was to move the sentinel into Thumb's permanently-undefined space
(0xDE00-0xDEFF, `udf`), which gcc never emits. This script is what keeps it
that way: it re-runs the exact scan the firmware will run, and fails if any
matching word is an INSTRUCTION rather than a literal pool entry.

A pure "is the high halfword a legal encoding" check would be weaker — it would
not catch a rodata constant, and it would not notice if someone changed the
scan window. This checks the bytes that will actually ship.

Usage:  check_xip_sentinels.py <elf>
Exit 0 = clean, 1 = a sentinel collides with code (or the ELF is unusable).
"""

import re
import subprocess
import sys

OBJDUMP = "arm-none-eabi-objdump"
NM = "arm-none-eabi-nm"

# name -> (base, section, start_symbol, end_symbol)
#
# start/end bound the region the firmware actually scans. For DOS that is
# [_DOS_MAIN_CODE_END, _OVERLAY_DOS_LOAD_END) -- main_dos.o is deliberately
# outside it because it *defines* the sentinel constant.
TARGETS = [
    ("dos", 0xDED00000, ".overlay_dos", "_DOS_MAIN_CODE_END", "_OVERLAY_DOS_LOAD_END"),
]

# The blob size the firmware compares against is the .xip blob's size. Using the
# full region length (512K) here is strictly more conservative: it flags a
# superset of what the firmware would rewrite.
WINDOW = 512 * 1024


def func_ranges(elf):
    """[(start, end, name)] for every STT_FUNC symbol.

    Sentinel-valued words OUTSIDE a function are data -- e.g.
    dos_xms_store_path / dos_ems_store_path, which are pointer variables whose
    target string lives in the XIP blob. Those are exactly the references the
    pass exists to fix, and rewriting them is correct.

    Only a word INSIDE a function body can be an instruction, and only there
    does "literal pool entry or instruction?" need deciding.
    """
    out = subprocess.run(["arm-none-eabi-readelf", "-sW", elf],
                         capture_output=True, text=True, check=True).stdout
    ranges = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC":
            try:
                addr, size = int(p[1], 16), int(p[2], 0)
            except ValueError:
                continue
            if size:
                ranges.append((addr, addr + size, p[7]))
    return ranges


def in_function(ranges, addr):
    for start, end, name in ranges:
        if start <= addr < end:
            return name
    return None


def symbols(elf):
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def disassemble(elf, section):
    """Return {addr: (raw_text, is_literal)} for one section.

    objdump renders a literal pool entry as `.word 0x...`; anything else is an
    instruction. That distinction is the whole point of this check.
    """
    out = subprocess.run(
        [OBJDUMP, "-d", "--section=" + section, elf],
        capture_output=True, text=True, check=True).stdout

    # e.g. " 240285fc:\t2800      \tcmp\tr0, #0"
    line_re = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$")
    result = {}
    for line in out.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        text = m.group(3).strip()
        result[addr] = (text, text.startswith(".word"))
    return result


def section_bytes(elf, section):
    out = subprocess.run(
        [OBJDUMP, "-s", "--section=" + section, elf],
        capture_output=True, text=True, check=True).stdout
    data = {}
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]{6,}) ((?:[0-9a-f]{2,8} ){1,4})", line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        blob = m.group(2).replace(" ", "")
        raw = bytes.fromhex(blob)
        for i, b in enumerate(raw):
            data[addr + i] = b
    return data


def check(elf):
    syms = symbols(elf)
    failures = []
    checked_any = False

    for name, base, section, start_sym, end_sym in TARGETS:
        if start_sym not in syms or end_sym not in syms:
            print(f"check_xip_sentinels: {name}: {start_sym}/{end_sym} absent "
                  f"-- core not linked in, skipping")
            continue
        checked_any = True
        start, end = syms[start_sym], syms[end_sym]
        data = section_bytes(elf, section)
        disasm = disassemble(elf, section)
        funcs = func_ranges(elf)

        hits = 0
        for addr in range(start & ~3, end, 4):
            if addr not in data or addr + 3 not in data:
                continue
            word = (data[addr] | data[addr + 1] << 8
                    | data[addr + 2] << 16 | data[addr + 3] << 24)
            if not (base <= (word & ~1) < base + WINDOW):
                continue
            hits += 1
            # Outside any function body => data, and rewriting it is the point.
            fn = in_function(funcs, addr)
            if fn is None:
                continue
            # Inside a function, a literal pool entry is also exactly what the
            # pass is meant to rewrite; objdump renders those as `.word`.
            if disasm.get(addr, ("", False))[1]:
                continue
            # Anything else inside a function is an instruction (or a pair of
            # them straddling the word), and the pass will destroy it.
            ctx = disasm.get(addr, ("<not disassembled>", False))[0]
            ctx2 = disasm.get(addr + 2, ("", False))[0]
            failures.append(
                f"  {name}: 0x{addr:08x} = 0x{word:08x} is CODE in {fn}(), "
                f"not a literal\n"
                f"      0x{addr:08x}: {ctx}\n"
                f"      0x{addr + 2:08x}: {ctx2}")
        print(f"check_xip_sentinels: {name}: base 0x{base:08x}, scanned "
              f"0x{start:08x}-0x{end:08x}, {hits} sentinel word(s), "
              f"{len(failures)} bad")

    if not checked_any:
        print("check_xip_sentinels: nothing to check")
        return 0

    if failures:
        sys.stderr.write(
            "\nERROR: an XIP sentinel matches a real instruction.\n"
            "The boot-time relocation pass will overwrite this code and the core\n"
            "will fault somewhere unrelated. Move the sentinel's HIGH halfword to\n"
            "an encoding gcc never emits (Thumb udf, 0xDE00-0xDEFF) -- see the\n"
            "DOS_CODE comment in STM32H7B0VBTx_SDCARD.ld.\n\n"
            + "\n".join(failures) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.stderr.write(__doc__)
        sys.exit(2)
    # Optional base override -- the NEGATIVE CONTROL. Pointing this at the old
    # 0xD05C0000 sentinel and an ELF built with it must FAIL, which is how we
    # know the check can fail at all:
    #   check_xip_sentinels.py old.elf 0xD05C0000   -> exit 1
    if len(sys.argv) == 3:
        override = int(sys.argv[2], 0)
        TARGETS[:] = [(n, override, s, a, b) for (n, _, s, a, b) in TARGETS]
    sys.exit(check(sys.argv[1]))
