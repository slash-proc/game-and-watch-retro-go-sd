#!/usr/bin/env python3
"""Generate the MS-DOS core's CP437 text-mode fonts.

Emits Core/Src/porting/dos/dos_font_data.c containing two 256-glyph tables:

  dos_font_4x8[256][8]   80-column mode; low nibble used (bit3 = leftmost)
  dos_font_8x8[256][8]   40-column mode; full byte (bit7 = leftmost)

The two tables come from completely different places.

THE 8x8 FONT IS AUTHENTIC IBM CP437, extracted from:

    The Ultimate Oldschool PC Font Pack v2.2  --  https://int10h.org/oldschool-pc-fonts/
    font file: "otb - Bm (linux bitmap)/Bm437_IBM_EGA_8x8.otb"

    Copyright (c) VileR.  Licensed CC BY-SA 4.0
    https://creativecommons.org/licenses/by-sa/4.0/

    The generated glyph table in dos_font_data.c is a DERIVATIVE WORK of that
    font and therefore also CC BY-SA 4.0 (ShareAlike).

    NOTE FOR MAINTAINERS: this repository is GPLv2. CC BY-SA 4.0 has a one-way
    compatibility path to GPLv3 ONLY, not GPLv2, so there is an unresolved
    licence-compatibility question here that the project owner needs to settle.
    Flagged, not resolved. If it cannot be resolved, substitute a public-domain
    CP437 8x8 ROM dump -- the glyph bitmaps are identical either way, since every
    CP437 8x8 font is a transcription of the same IBM ROM.

    The .otb form is used because fontTools parses it reliably (EBDT format 5
    stores 8x8 1-bit glyphs as 8 row-major MSB-first bytes -- byte-for-byte what
    dos_font_8x8 needs, no transformation). The .FON in the same pack would
    require hand-rolling a Windows NE resource parser for the same result.

THE 4x8 FONT IS OURS. The pack contains nothing narrower than 8 px, and
condensing 8x8 down to 4 px algorithmically produces mush, so the 80-column
font is hand-authored here. Two classes of glyph:

  * Printable ASCII (0x20-0x7E) is hand-authored below as 3x5 pixel art, drawn
    at x=0..2 so x=3 stays an inter-character gap.

  * Box drawing, blocks and shades are generated PROCEDURALLY. Every CP437 box
    character is a combination of north/south/east/west stubs, each absent,
    single or double, so deriving them from that table guarantees the property
    that actually matters: horizontal runs reach both cell edges and vertical
    runs reach top and bottom, so adjacent cells join seamlessly. Hand-drawing
    ~40 such glyphs invites exactly the off-by-one that makes frames dashed.

Usage:
    python3 tools/gen_dos_font.py                  # write the C table
    python3 tools/gen_dos_font.py --preview        # ASCII-art preview to stdout
    python3 tools/gen_dos_font.py --png sheet.png  # also dump an editable sheet
    python3 tools/gen_dos_font.py --otb FILE.otb   # use an already-extracted font

By default the 8x8 font is extracted straight out of the zipped pack at
external/8086tiny/oldschool_pc_font_pack_v2.2_FULL.zip (gitignored, 20 MB), so
regeneration is a single reproducible command. Requires fontTools.

--png writes a 16x16 grid glyph sheet so the 4x8 shapes can be inspected in an
image editor, following the tools/png_to_logo.py and tools/img2bin.py convention
of keeping an editable artifact around.
"""

import argparse
import io
import sys
import zipfile
from pathlib import Path

OUT_C = Path("Core/Src/porting/dos/dos_font_data.c")

# Sources of the authentic CP437 glyphs. See the licence note above.
FONT_ZIP = Path("external/8086tiny/oldschool_pc_font_pack_v2.2_FULL.zip")
FONT_MEMBER = "otb - Bm (linux bitmap)/Bm437_IBM_EGA_8x8.otb"
FONT_MEMBER_4 = "otb - Bm (linux bitmap)/Bm437_EverexME_5x8.otb"
FONT_CREDIT = ("Bm437_IBM_EGA_8x8.otb and Bm437_EverexME_5x8.otb -- The Ultimate "
               "Oldschool PC Font Pack v2.2, (c) VileR, CC BY-SA 4.0, "
               "https://int10h.org/oldschool-pc-fonts/")

# CP437 0x00-0x1F and 0x7F are graphic symbols, not C0 controls, and Python's
# cp437 codec maps them to controls -- so they need an explicit table to look up
# in the font's Unicode cmap.
CP437_LOW = (" ☺☻♥♦♣♠•◘○◙♂♀"
             "♪♫☼►◄↕‼¶§▬↨"
             "↑↓→←∟↔▲▼")


def cp437_to_unicode(code):
    """CP437 byte -> the Unicode codepoint the font's cmap will be keyed on."""
    if code < 0x20:
        return CP437_LOW[code]
    if code == 0x7F:
        return "⌂"          # house
    return bytes([code]).decode("cp437")


def load_otb(otb_bytes, width, height=8):
    """Extract all 256 CP437 glyphs from an OpenType-bitmap font.

    Returns a list of 256 lists of `height` row-bitmaps, MSB = leftmost pixel,
    each row `width` bits wide.

    EBDT format 5 is BIT-aligned, not byte-aligned: a glyph is width*height bits
    packed continuously. Only when width == 8 does that coincide with one byte
    per row -- narrow fonts (the 5x8 is 5 bytes for 40 bits) need real bit
    unpacking, so do not assume one byte per row.
    """
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit("fontTools is required to extract the fonts (pip install fonttools)")

    font = TTFont(io.BytesIO(otb_bytes))
    if "EBDT" not in font or "EBLC" not in font:
        sys.exit("not a bitmap font: no EBDT/EBLC tables")

    size = font["EBLC"].strikes[0].bitmapSizeTable
    if (size.ppemY, size.bitDepth) != (height, 1):
        sys.exit(f"expected a 1-bit strike {height} px tall, got "
                 f"ppem {size.ppemX}x{size.ppemY} depth {size.bitDepth}")
    if size.hori.widthMax != width:
        sys.exit(f"expected widthMax {width}, got {size.hori.widthMax}")

    cmap = font.getBestCmap()
    strike = font["EBDT"].strikeData[0]
    need = (width * height + 7) // 8

    table, missing = [], []
    for code in range(256):
        name = cmap.get(ord(cp437_to_unicode(code)))
        glyph = strike.get(name) if name else None
        if glyph is None:
            missing.append(code)
            table.append([0] * height)
            continue
        data = glyph.imageData
        if len(data) != need:
            sys.exit(f"glyph 0x{code:02X}: {len(data)} bytes of image data, "
                     f"expected {need} for {width}x{height} -- unexpected packing")
        rows = []
        for y in range(height):
            v = 0
            for x in range(width):
                i = y * width + x
                if data[i >> 3] & (0x80 >> (i & 7)):
                    v |= 1 << (width - 1 - x)
            rows.append(v)
        table.append(rows)

    if missing:
        print(f"warning: {len(missing)} glyph(s) absent from the font: "
              f"{', '.join(f'0x{c:02X}' for c in missing)}", file=sys.stderr)
    return table


def narrow_to_4px(table5):
    """Truncate a 5-px-wide table to its leftmost 4 columns.

    Bm437_EverexME_5x8 is effectively a 4-px-ink font with one column of
    inter-character gap: of the 95 printable ASCII glyphs, only 5 put any ink in
    column 4 (# & @ Y _). Box drawing and blocks span the full cell, so dropping
    the last column leaves them spanning the full 4 px -- frames still join.
    Verified empirically, not assumed.
    """
    return [[(r >> 1) & 0x0F for r in g] for g in table5]


def read_font_bytes(zip_path, member, override=None):
    """Get .otb bytes from an explicit file, or from the zipped pack."""
    if override:
        return override.read_bytes()
    if not zip_path.exists():
        sys.exit(f"{zip_path} not found.\n"
                 f"Download the Ultimate Oldschool PC Font Pack v2.2 from\n"
                 f"  https://int10h.org/oldschool-pc-fonts/\n"
                 f"and place the FULL zip there, or pass --otb8/--otb5 with "
                 f"extracted .otb files.")
    with zipfile.ZipFile(zip_path) as z:
        try:
            return z.read(member)
        except KeyError:
            sys.exit(f"{member!r} not in {zip_path}")

# ---------------------------------------------------------------------------
# Hand-authored ASCII
# ---------------------------------------------------------------------------
# 3x5 cell, drawn into the 4x8 glyph at x=0..2 (x=3 is the inter-character gap)
# and y=1..5, giving a baseline at y=5 and one descender row at y=6.
A3x5 = {
    " ": ("...", "...", "...", "...", "..."),
    "!": (".#.", ".#.", ".#.", "...", ".#."),
    '"': ("#.#", "#.#", "...", "...", "..."),
    "#": ("#.#", "###", "#.#", "###", "#.#"),
    "$": (".##", "##.", ".##", "##.", "..."),
    "%": ("#.#", "..#", ".#.", "#..", "#.#"),
    "&": ("##.", "##.", "###", "#.#", "###"),
    "'": (".#.", ".#.", "...", "...", "..."),
    "(": ("..#", ".#.", ".#.", ".#.", "..#"),
    ")": ("#..", ".#.", ".#.", ".#.", "#.."),
    "*": ("#.#", ".#.", "#.#", "...", "..."),
    "+": ("...", ".#.", "###", ".#.", "..."),
    ",": ("...", "...", "...", ".#.", "#.."),
    "-": ("...", "...", "###", "...", "..."),
    ".": ("...", "...", "...", "...", ".#."),
    "/": ("..#", "..#", ".#.", "#..", "#.."),
    "0": ("###", "#.#", "#.#", "#.#", "###"),
    # 1 / i / l / | are the hard set at 3 px: see RAW4 below for how | is split
    # out. 1 keeps a top-left serif plus a full base; i is dot-gap-serif-base;
    # l is top-serif plus a foot kicking right. All four then differ in more
    # than one row, which is what makes them tellable apart at this size.
    "1": (".#.", "##.", ".#.", ".#.", "###"),
    "2": ("###", "..#", "###", "#..", "###"),
    "3": ("###", "..#", "###", "..#", "###"),
    "4": ("#.#", "#.#", "###", "..#", "..#"),
    "5": ("###", "#..", "###", "..#", "###"),
    "6": ("###", "#..", "###", "#.#", "###"),
    "7": ("###", "..#", "..#", "..#", "..#"),
    "8": ("###", "#.#", "###", "#.#", "###"),
    "9": ("###", "#.#", "###", "..#", "###"),
    ":": ("...", ".#.", "...", ".#.", "..."),
    ";": ("...", ".#.", "...", ".#.", "#.."),
    "<": ("..#", ".#.", "#..", ".#.", "..#"),
    "=": ("...", "###", "...", "###", "..."),
    ">": ("#..", ".#.", "..#", ".#.", "#.."),
    "?": ("###", "..#", ".##", "...", ".#."),
    "@": ("###", "#.#", "###", "#..", "###"),
    "A": ("###", "#.#", "###", "#.#", "#.#"),
    "B": ("##.", "#.#", "##.", "#.#", "##."),
    "C": ("###", "#..", "#..", "#..", "###"),
    "D": ("##.", "#.#", "#.#", "#.#", "##."),
    "E": ("###", "#..", "###", "#..", "###"),
    "F": ("###", "#..", "###", "#..", "#.."),
    "G": ("###", "#..", "#.#", "#.#", "###"),
    "H": ("#.#", "#.#", "###", "#.#", "#.#"),
    "I": ("###", ".#.", ".#.", ".#.", "###"),
    "J": ("..#", "..#", "..#", "#.#", "###"),
    "K": ("#.#", "#.#", "##.", "#.#", "#.#"),
    "L": ("#..", "#..", "#..", "#..", "###"),
    "M": ("#.#", "###", "###", "#.#", "#.#"),
    "N": ("##.", "#.#", "#.#", "#.#", "#.#"),
    "O": ("###", "#.#", "#.#", "#.#", "###"),
    "P": ("###", "#.#", "###", "#..", "#.."),
    "Q": ("###", "#.#", "#.#", "###", "..#"),
    "R": ("###", "#.#", "##.", "#.#", "#.#"),
    "S": ("###", "#..", "###", "..#", "###"),
    "T": ("###", ".#.", ".#.", ".#.", ".#."),
    "U": ("#.#", "#.#", "#.#", "#.#", "###"),
    "V": ("#.#", "#.#", "#.#", "#.#", ".#."),
    "W": ("#.#", "#.#", "###", "###", "#.#"),
    "X": ("#.#", "#.#", ".#.", "#.#", "#.#"),
    "Y": ("#.#", "#.#", "###", ".#.", ".#."),
    "Z": ("###", "..#", ".#.", "#..", "###"),
    "[": ("###", "#..", "#..", "#..", "###"),
    "\\": ("#..", "#..", ".#.", "..#", "..#"),
    "]": ("###", "..#", "..#", "..#", "###"),
    "^": (".#.", "#.#", "...", "...", "..."),
    "_": ("...", "...", "...", "...", "###"),
    "`": ("#..", ".#.", "...", "...", "..."),
    "a": ("...", "##.", "#.#", "#.#", "###"),
    "b": ("#..", "#..", "##.", "#.#", "##."),
    "c": ("...", ".##", "#..", "#..", ".##"),
    "d": ("..#", "..#", ".##", "#.#", ".##"),
    "e": ("...", ".##", "#.#", "##.", ".##"),
    "f": ("..#", ".#.", "###", ".#.", ".#."),
    "g": ("...", ".##", "#.#", ".##", "##."),
    "h": ("#..", "#..", "##.", "#.#", "#.#"),
    "i": (".#.", "...", "##.", ".#.", ".#."),
    "j": ("..#", "...", "..#", "#.#", ".#."),
    "k": ("#..", "#.#", "##.", "##.", "#.#"),
    "l": ("##.", ".#.", ".#.", ".#.", ".##"),
    "m": ("...", "##.", "###", "###", "#.#"),
    "n": ("...", "##.", "#.#", "#.#", "#.#"),
    "o": ("...", ".#.", "#.#", "#.#", ".#."),
    "p": ("...", "##.", "#.#", "##.", "#.."),
    "q": ("...", ".##", "#.#", ".##", "..#"),
    "r": ("...", ".##", "#..", "#..", "#.."),
    "s": ("...", ".##", "##.", "..#", "##."),
    "t": (".#.", "###", ".#.", ".#.", ".##"),
    "u": ("...", "#.#", "#.#", "#.#", ".##"),
    "v": ("...", "#.#", "#.#", "#.#", ".#."),
    "w": ("...", "#.#", "###", "###", "##."),
    "x": ("...", "#.#", ".#.", ".#.", "#.#"),
    "y": ("...", "#.#", "#.#", ".##", "##."),
    "z": ("...", "###", "..#", ".#.", "###"),
    "{": ("..#", ".#.", "##.", ".#.", "..#"),
    "|": (".#.", ".#.", ".#.", ".#.", ".#."),
    "}": ("#..", ".#.", ".##", ".#.", "#.."),
    "~": ("...", ".##", "##.", "...", "..."),
}

# Repairs applied AFTER truncating the authentic 5x8 down to 4 px.
#
# Only 5 printable glyphs put ink in the discarded column (# & @ Y _), and only
# one of them actually breaks:
#
#   Y  loses its entire right arm and reads as a stray vertical bar. Y is a
#      common letter, so it is redrawn here as a symmetric 4 px Y.
#
#   _  is unaffected in practice -- it spanned all 5 columns, so at 4 px it still
#      spans the full cell and consecutive underscores still join.
#   # & @  lose their rightmost column and are slightly lopsided but remain
#      readable, and all three are rare in DOS filenames and shell output. Left
#      as the authentic font truncated rather than invented here.
#
# Values are row bitmaps, bit 3 = leftmost pixel.
RAW4_FIX = {
    "Y": (0b1001, 0b1001, 0b0110, 0b0100, 0b0100, 0b0100, 0b0100, 0b0000),
}

# 5x7 cell, drawn into the 8x8 glyph at x=0..4 (x=5..7 gap) and y=0..6.
A5x7 = {
    " ": (".....",) * 7,
    "!": ("..#..", "..#..", "..#..", "..#..", ".....", "..#..", "....."),
    '"': (".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."),
    "#": (".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."),
    "$": ("..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."),
    "%": ("##..#", "##.#.", "..#..", ".#...", "#.##.", "#..##", "....."),
    "&": (".##..", "#..#.", ".##..", "#..#.", "#.#.#", ".#.#.", "..#.#"),
    "'": ("..#..", "..#..", ".....", ".....", ".....", ".....", "....."),
    "(": ("...#.", "..#..", "..#..", "..#..", "..#..", "..#..", "...#."),
    ")": (".#...", "..#..", "..#..", "..#..", "..#..", "..#..", ".#..."),
    "*": (".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."),
    "+": (".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."),
    ",": (".....", ".....", ".....", ".....", ".....", "..##.", "..#.."),
    "-": (".....", ".....", ".....", "#####", ".....", ".....", "....."),
    ".": (".....", ".....", ".....", ".....", ".....", ".##..", ".##.."),
    "/": ("....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."),
    "0": (".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": ("..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."),
    ":": (".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."),
    ";": (".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."),
    "<": ("...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."),
    "=": (".....", ".....", "#####", ".....", "#####", ".....", "....."),
    ">": (".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."),
    "?": (".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."),
    "@": (".###.", "#...#", "....#", ".####", "#.#.#", "#.#.#", ".####"),
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "B": ("####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "D": ("###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "G": (".###.", "#...#", "#....", "#..##", "#...#", "#...#", ".###."),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "I": (".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "J": ("..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "L": ("#....", "#....", "#....", "#....", "#....", "#....", "#####"),
    "M": ("#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"),
    "N": ("#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "P": ("####.", "#...#", "#...#", "####.", "#....", "#....", "#...."),
    "Q": (".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "X": ("#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"),
    "Y": ("#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."),
    "Z": ("#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"),
    "[": (".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."),
    "\\": ("#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#"),
    "]": (".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."),
    "^": ("..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."),
    "_": (".....", ".....", ".....", ".....", ".....", ".....", "#####"),
    "`": (".#...", "..#..", ".....", ".....", ".....", ".....", "....."),
    "a": (".....", ".....", ".###.", "....#", ".####", "#...#", ".####"),
    "b": ("#....", "#....", "####.", "#...#", "#...#", "#...#", "####."),
    "c": (".....", ".....", ".###.", "#....", "#....", "#...#", ".###."),
    "d": ("....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"),
    "e": (".....", ".....", ".###.", "#...#", "#####", "#....", ".###."),
    "f": ("..##.", ".#...", ".#...", "####.", ".#...", ".#...", ".#..."),
    "g": (".....", ".####", "#...#", "#...#", ".####", "....#", ".###."),
    "h": ("#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"),
    "i": ("..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."),
    "j": ("...#.", ".....", "..##.", "...#.", "...#.", "#..#.", ".##.."),
    "k": ("#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."),
    "l": (".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "m": (".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"),
    "n": (".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"),
    "o": (".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."),
    "p": (".....", ".....", "####.", "#...#", "####.", "#....", "#...."),
    "q": (".....", ".....", ".####", "#...#", ".####", "....#", "....#"),
    "r": (".....", ".....", ".####", "#....", "#....", "#....", "#...."),
    "s": (".....", ".....", ".####", "#....", ".###.", "....#", "####."),
    "t": (".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##."),
    "u": (".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####"),
    "v": (".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "w": (".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."),
    "x": (".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"),
    "y": (".....", ".....", "#...#", "#...#", ".####", "....#", ".###."),
    "z": (".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"),
    "{": ("..##.", ".#...", ".#...", "##...", ".#...", ".#...", "..##."),
    "|": ("..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "}": (".##..", "...#.", "...#.", "...##", "...#.", "...#.", ".##.."),
    "~": (".....", "..#.#", ".#.#.", ".....", ".....", ".....", "....."),
}

# ---------------------------------------------------------------------------
# Procedural box drawing
# ---------------------------------------------------------------------------
# CP437 code -> (north, south, east, west), 0 = absent, 1 = single, 2 = double.
BOX = {
    0xB3: (1, 1, 0, 0), 0xB4: (1, 1, 0, 1), 0xB5: (1, 1, 0, 2),
    0xB6: (2, 2, 0, 1), 0xB7: (0, 2, 0, 1), 0xB8: (0, 1, 0, 2),
    0xB9: (2, 2, 0, 2), 0xBA: (2, 2, 0, 0), 0xBB: (0, 2, 0, 2),
    0xBC: (2, 0, 0, 2), 0xBD: (2, 0, 0, 1), 0xBE: (1, 0, 0, 2),
    0xBF: (0, 1, 0, 1), 0xC0: (1, 0, 1, 0), 0xC1: (1, 0, 1, 1),
    0xC2: (0, 1, 1, 1), 0xC3: (1, 1, 1, 0), 0xC4: (0, 0, 1, 1),
    0xC5: (1, 1, 1, 1), 0xC6: (1, 1, 2, 0), 0xC7: (2, 2, 1, 0),
    0xC8: (2, 0, 2, 0), 0xC9: (0, 2, 2, 0), 0xCA: (2, 0, 2, 2),
    0xCB: (0, 2, 2, 2), 0xCC: (2, 2, 2, 0), 0xCD: (0, 0, 2, 2),
    0xCE: (2, 2, 2, 2), 0xCF: (1, 0, 2, 2), 0xD0: (2, 0, 1, 1),
    0xD1: (0, 1, 2, 2), 0xD2: (0, 2, 1, 1), 0xD3: (2, 0, 1, 0),
    0xD4: (1, 0, 2, 0), 0xD5: (0, 1, 2, 0), 0xD6: (0, 2, 1, 0),
    0xD7: (2, 2, 1, 1), 0xD8: (1, 1, 2, 2), 0xD9: (1, 0, 0, 1),
    0xDA: (0, 1, 1, 0),
}


def box_glyph(spec, w, h):
    """Render one box-drawing glyph as a list of h row-bitmaps (LSB = rightmost).

    Lines always run from the cell border to the perpendicular centre band, so a
    horizontal run reaches x=0 and x=w-1 and a vertical run reaches y=0 and
    y=h-1. That is what makes adjacent cells join without gaps.
    """
    n, s, e, wst = spec
    px = [[0] * w for _ in range(h)]

    vc = w // 2 - 1                      # single vertical column
    hr = h // 2 - 1                      # single horizontal row
    vcols = lambda v: [vc] if v == 1 else [vc - 1, vc + 1]
    hrows = lambda v: [hr] if v == 1 else [hr - 1, hr + 1]

    # Extent of the centre band, so perpendicular lines meet it cleanly.
    band_rows = sorted({r for v in (e, wst) if v for r in hrows(v)}) or [hr]
    band_cols = sorted({c for v in (n, s) if v for c in vcols(v)}) or [vc]

    def hline(row, x0, x1):
        for x in range(x0, x1 + 1):
            px[row][x] = 1

    def vline(col, y0, y1):
        for y in range(y0, y1 + 1):
            px[y][col] = 1

    # A pure double corner gets outer/inner turning, which is the difference
    # between a recognisable double corner and a small filled blob.
    dirs = [d for d, v in zip("NSEW", (n, s, e, wst)) if v]
    if len(dirs) == 2 and all(v == 2 for v in (n, s, e, wst) if v) and \
       set(dirs) in ({"S", "E"}, {"S", "W"}, {"N", "E"}, {"N", "W"}):
        c_out, c_in = (vc - 1, vc + 1) if "E" in dirs else (vc + 1, vc - 1)
        r_out, r_in = (hr - 1, hr + 1) if "S" in dirs else (hr + 1, hr - 1)
        xo0, xo1 = (0, w - 1) if "E" in dirs else (0, w - 1)
        if "E" in dirs:
            hline(r_out, min(c_out, c_in) if False else c_out, w - 1)
            hline(r_in, c_in, w - 1)
            hline(r_out, c_out, w - 1)
        else:
            hline(r_out, 0, c_out)
            hline(r_in, 0, c_in)
        if "S" in dirs:
            vline(c_out, r_out, h - 1)
            vline(c_in, r_in, h - 1)
        else:
            vline(c_out, 0, r_out)
            vline(c_in, 0, r_in)
        # Close the outer corner across the full edge run.
        if "E" in dirs:
            hline(r_out, c_out, w - 1)
        else:
            hline(r_out, 0, c_out)
        return [sum(px[y][x] << (w - 1 - x) for x in range(w)) for y in range(h)]

    if n:
        for c in vcols(n):
            vline(c, 0, max(band_rows))
    if s:
        for c in vcols(s):
            vline(c, min(band_rows), h - 1)
    if wst:
        for r in hrows(wst):
            hline(r, 0, max(band_cols))
    if e:
        for r in hrows(e):
            hline(r, min(band_cols), w - 1)

    return [sum(px[y][x] << (w - 1 - x) for x in range(w)) for y in range(h)]


def shade_glyph(code, w, h):
    """Blocks and shades (0xB0-0xB2, 0xDB-0xDF), also procedural."""
    rows = []
    for y in range(h):
        v = 0
        for x in range(w):
            on = 0
            if code == 0xB0:                      # light shade
                on = (x + y) % 4 == 0
            elif code == 0xB1:                    # medium shade
                on = (x + y) % 2 == 0
            elif code == 0xB2:                    # dark shade
                on = (x + y) % 4 != 0
            elif code == 0xDB:                    # full block
                on = 1
            elif code == 0xDC:                    # lower half
                on = y >= h // 2
            elif code == 0xDD:                    # left half
                on = x < w // 2
            elif code == 0xDE:                    # right half
                on = x >= w // 2
            elif code == 0xDF:                    # upper half
                on = y < h // 2
            v |= on << (w - 1 - x)
        rows.append(v)
    return rows


def place(art, w, h, ox, oy):
    """Blit a pixel-art tuple into a w x h glyph at (ox, oy)."""
    rows = [0] * h
    for dy, line in enumerate(art):
        y = oy + dy
        if not (0 <= y < h):
            continue
        v = 0
        for dx, ch in enumerate(line):
            x = ox + dx
            if 0 <= x < w and ch == "#":
                v |= 1 << (w - 1 - x)
        rows[y] |= v
    return rows


def build(w, h, ascii_art, ox, oy, raw=None):
    """Build a full 256-glyph table."""
    raw = raw or {}
    table = []
    for code in range(256):
        if 0x20 <= code <= 0x7E and chr(code) in raw:
            table.append(list(raw[chr(code)]))
        elif code in BOX:
            table.append(box_glyph(BOX[code], w, h))
        elif code in (0xB0, 0xB1, 0xB2, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF):
            table.append(shade_glyph(code, w, h))
        elif 0x20 <= code <= 0x7E and chr(code) in ascii_art:
            table.append(place(ascii_art[chr(code)], w, h, ox, oy))
        else:
            # Unmapped: a hollow box so missing glyphs are visible, not invisible.
            rows = [0] * h
            if code:
                for y in range(1, h - 1):
                    rows[y] = (1 << (w - 1)) | 1 if 1 < y < h - 2 else 0
                rows[1] = rows[h - 2] = (1 << w) - 1
            table.append(rows)
    return table


def emit_c(f4, f8):
    lines = [
        "/* GENERATED by tools/gen_dos_font.py -- do not edit by hand.",
        " *",
        " * CP437 text-mode fonts for the MS-DOS core. 8086tiny's BIOS blob has no",
        " * character generator (see",
        " * external/8086tiny/docs/video/04-text-rendering.md), so the glyphs come",
        " * from the two sources below.",
        " *",
        " * dos_font_4x8: 80-column mode. Low nibble used, bit 3 = leftmost pixel.",
        " * dos_font_8x8: 40-column mode. Full byte, bit 7 = leftmost pixel.",
        " *",
        " * ---------------------------------------------------------------------",
        " * BOTH TABLES ARE AUTHENTIC CP437 -- THIRD-PARTY, CC BY-SA 4.0",
        " * ---------------------------------------------------------------------",
        " * Extracted from The Ultimate Oldschool PC Font Pack v2.2:",
        " *",
        " *   dos_font_8x8  <-  Bm437_IBM_EGA_8x8.otb     (verbatim)",
        " *   dos_font_4x8  <-  Bm437_EverexME_5x8.otb    (leftmost 4 columns)",
        " *",
        " *   (c) VileR.  https://int10h.org/oldschool-pc-fonts/",
        " *   Licence: Creative Commons Attribution-ShareAlike 4.0 International",
        " *            https://creativecommons.org/licenses/by-sa/4.0/",
        " *",
        " * These tables are DERIVATIVE WORKS of those fonts and are therefore",
        " * themselves CC BY-SA 4.0 (ShareAlike). Keep this notice with them.",
        " *",
        " * MAINTAINERS: this repository is GPLv2, and CC BY-SA 4.0 has a one-way",
        " * compatibility path to GPLv3 ONLY. That licence-compatibility question is",
        " * unresolved and needs a decision from the project owner. If it cannot be",
        " * resolved, substitute a public-domain CP437 ROM dump for the 8x8 -- the",
        " * bitmaps are identical, since every CP437 8x8 font transcribes the same",
        " * IBM ROM. The 4x8 has no public-domain equivalent; falling back there",
        " * means the hand-drawn shapes still in tools/gen_dos_font.py (--hand-4x8).",
        " *",
        " * ---------------------------------------------------------------------",
        " * Why the 4x8 is a truncated 5x8",
        " * ---------------------------------------------------------------------",
        " * 80 columns must fit 320 px, so the cell is exactly 4 px. The Everex ME",
        " * 5x8 is a real 1980s narrow PC font and is effectively 4 px of ink plus",
        " * one column of inter-character gap: of the 95 printable ASCII glyphs only",
        " * five (# & @ Y _) put any ink in column 4. Box drawing and blocks span the",
        " * whole cell, so after truncation they still span the full 4 px and frames",
        " * join without gaps -- verified by rendering, not assumed.",
        " *",
        " * Y is the only glyph truncation genuinely breaks (it loses its right arm),",
        " * and is redrawn; see RAW4_FIX in the generator.",
        " */",
        "",
        '#include "dos_video.h"',
        "",
    ]
    for name, tbl in (("dos_font_4x8", f4), ("dos_font_8x8", f8)):
        lines.append(f"const uint8_t {name}[256][8] = {{")
        for code, rows in enumerate(tbl):
            body = ", ".join(f"0x{r:02X}" for r in rows)
            ch = chr(code) if 0x20 <= code <= 0x7E else ""
            note = f"  /* 0x{code:02X} {ch} */" if ch else f"  /* 0x{code:02X} */"
            lines.append(f"    {{ {body} }},{note}")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def preview(tbl, w, h, codes):
    for code in codes:
        ch = chr(code) if 0x20 <= code <= 0x7E else "."
        print(f"-- 0x{code:02X} {ch}")
        for r in tbl[code]:
            print("   " + "".join("#" if r & (1 << (w - 1 - x)) else "." for x in range(w)))


def main():
    ap = argparse.ArgumentParser(
        description="Generate the MS-DOS core's CP437 text-mode font tables.")
    ap.add_argument("--png", type=Path, help="also write a 16x16 editable glyph sheet")
    ap.add_argument("--preview", action="store_true", help="ASCII-art preview to stdout")
    ap.add_argument("--out", type=Path, default=OUT_C)
    ap.add_argument("--otb8", type=Path,
                    help="pre-extracted Bm437_IBM_EGA_8x8.otb (default: from --font-zip)")
    ap.add_argument("--otb5", type=Path,
                    help="pre-extracted Bm437_EverexME_5x8.otb (default: from --font-zip)")
    ap.add_argument("--font-zip", type=Path, default=FONT_ZIP,
                    help=f"Oldschool PC Font Pack zip (default: {FONT_ZIP})")
    ap.add_argument("--hand-4x8", action="store_true",
                    help="use the older hand-drawn 3x5 art for the 4x8 font instead "
                         "of the truncated authentic 5x8 (kept for comparison)")
    args = ap.parse_args()

    # Both tables are authentic CP437 now. See the licence note at the top of the
    # file: the 8x8 is IBM EGA verbatim, the 4x8 is the Everex 5x8 -- a genuine
    # narrow PC font -- truncated to its 4 ink columns.
    f8 = load_otb(read_font_bytes(args.font_zip, FONT_MEMBER, args.otb8), 8)

    if args.hand_4x8:
        f4 = build(4, 8, A3x5, ox=0, oy=1)
    else:
        f5 = load_otb(read_font_bytes(args.font_zip, FONT_MEMBER_4, args.otb5), 5)
        f4 = narrow_to_4px(f5)
        for ch, rows in RAW4_FIX.items():
            f4[ord(ch)] = list(rows)

    if args.preview:
        src4 = "hand-drawn 3x5" if args.hand_4x8 else "authentic Everex 5x8 -> 4px"
        print(f"=== 4x8 (80-column, {src4}) ===")
        preview(f4, 4, 8, [0x41, 0x61, 0x30, 0xC4, 0xB3, 0xDA, 0xC5, 0xCD, 0xBA, 0xC9, 0xCE])
        print("=== 8x8 (40-column, authentic IBM CP437) ===")
        preview(f8, 8, 8, [0x41, 0x61, 0xDA, 0xC9])
        return

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(emit_c(f4, f8))
    print(f"wrote {args.out}")

    if args.png:
        try:
            from PIL import Image
        except ImportError:
            print("PIL not available; skipping --png")
            return
        for tbl, w, suffix in ((f4, 4, "4x8"), (f8, 8, "8x8")):
            img = Image.new("1", (16 * w, 16 * 8))
            for code in range(256):
                gx, gy = (code % 16) * w, (code // 16) * 8
                for y, r in enumerate(tbl[code]):
                    for x in range(w):
                        if r & (1 << (w - 1 - x)):
                            img.putpixel((gx + x, gy + y), 1)
            p = args.png.with_name(f"{args.png.stem}_{suffix}.png")
            img.save(p)
            print(f"wrote {p}")


if __name__ == "__main__":
    main()
