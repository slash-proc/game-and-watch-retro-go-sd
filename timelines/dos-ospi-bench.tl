# Unattended cold-start -> DOS launch, for the OSPI benchmark run.
# Absolute seconds from machine start; same clock on gwemu and on
# scripts/gwharness.py --boot, so this one file drives both.
#
# It only presses A twice: open the MS-DOS system list, launch the highlighted
# ROM. That is enough because the launcher comes up on the MS-DOS tab in this
# tree even with a blank config ("Config: Magic mismatch ... got 0x00000000") --
# verified under gwemu 2026-07-31, log shows "Retro-Go: Initializing emulator
# 'MS-DOS'" then "Starting game: BATTLECHESS" with no navigation.
# If the launcher ever comes up on a different tab, add left/right presses here
# rather than changing the press times.
#
# Timings are the hardware ones (launcher ready ~11 s). gwemu reaches the
# launcher in ~2 s, so a press at 11.0 is simply late, not wrong -- verified
# working under gwemu, which is why this file is not split per target.
#
# The benchmark prints its OSPIBENCH lines at DOS-core entry, within a second of
# the second press; the quit is generous so the DOS: prof / DWT lines that follow
# are captured too.
11.0  press a
12.5  press a
60.0  quit
