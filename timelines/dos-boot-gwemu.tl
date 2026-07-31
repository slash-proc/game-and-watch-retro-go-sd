# Same as dos-boot.tl but timed for gwemu, which boots much faster than hardware.
# Hardware needs ~11s to reach the launcher; gwemu reaches it in ~2s.
# Keep the two separate -- gwharness.py (hardware) and run_gwemu.sh share dos-boot.tl.
#
# IMPORTANT, learned 2026-07-31: "press a twice" only reaches DOS when MS-DOS is
# already the selected system, and that selection lives in the launcher's saved
# config on the SD card (::/odroid). Rebuilding build/sdcard.img from scratch --
# `rm -f build/sdcard.img` then `make gwemu_release` -- wipes it, after which this
# timeline lands on whatever tab the launcher defaults to and the run looks like a
# clean boot with no guest. That is a silent no-op, not an error, and it cost real
# time to diagnose: check for ::/odroid on the card before believing a DOS run.
#
# Timings were also relaxed from 2.0/4.5: the DOS core grew (EGA planar + XMS took
# dos.bin from 39 KB to 47 KB) so the launcher needs longer before it will accept
# input.
6.0   press a
10.0  press a
50.0  quit
