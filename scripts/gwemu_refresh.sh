#!/usr/bin/env bash
# Refresh the gwemu media IN PLACE, preserving launcher state.
#
# WHY THIS EXISTS. The obvious refresh -- make_sdcard_image.py, mformat, mcopy --
# reformats build/sdcard.img, which destroys ::/odroid. That directory holds the
# launcher's saved system tab and last-played title. Losing it means a timeline
# that does "press a twice" no longer reaches the intended core: it lands on
# whatever tab the launcher defaults to and the run looks like a clean boot with
# no guest. That is a SILENT no-op, not an error.
#
# timelines/ contains several progressively longer attempts to steer the
# launcher blind, which is what that silent no-op produces: each one encodes a
# guess about menu geometry rather than fixing the cause. Do not add another.
# Select the system once by hand; from then on it persists, and press-a-twice
# works -- provided nothing reformats the card, which is this script's job.
#
# Only reformats when the image does not exist, or when --reformat is passed
# deliberately (which costs the saved selection and should be rare).
set -e
cd "$(dirname "$0")/.."

IMG=build/sdcard.img
REFORMAT=0
[ "$1" = "--reformat" ] && REFORMAT=1

if [ ! -f "$IMG" ] || [ "$REFORMAT" = "1" ]; then
    echo "[refresh] creating $IMG from scratch (launcher selection will be lost)"
    python3 scripts/make_sdcard_image.py "$IMG" --fit sd_content --fit roms >/dev/null
    mformat -i "$IMG@@1M" -F -v RETROGO ::
else
    echo "[refresh] updating $IMG in place, preserving ::/odroid"
fi

# -o overwrites, -s recurses. Existing files are replaced; ::/odroid is untouched
# because nothing under sd_content/ or roms/ writes there.
mcopy -i "$IMG@@1M" -s -o -Q sd_content/* ::/
mmd -i "$IMG@@1M" -D s ::/roms 2>/dev/null || true
[ -d roms ] && mcopy -i "$IMG@@1M" -s -o -Q roms/* ::/roms/

# The bank images are rewritten every time: they are pure build output with no
# state in them, unlike the card.
dd if=build/gw_retro_go_intflash.bin of=build/qemu_bank1.bin bs=256k count=1 conv=sync 2>/dev/null
[ -f build/qemu_bank2.bin ] || dd if=/dev/zero of=build/qemu_bank2.bin bs=256k count=1 2>/dev/null
[ -f build/extflash.bin ] || dd if=/dev/zero of=build/extflash.bin bs=1M count=64 2>/dev/null

if mdir -i "$IMG@@1M" ::/ 2>/dev/null | grep -qi odroid; then
    echo "[refresh] ::/odroid present -- press-a-twice timelines will work"
else
    echo "[refresh] ::/odroid ABSENT -- select the system once by hand before"
    echo "[refresh] relying on any press-a-twice timeline"
fi
