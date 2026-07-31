# OSK regression test: boot FreeDOS, open the on-screen keyboard, type "dir",
# and assert the guest echoed it and produced a directory listing.
#
#   ./scripts/run_gwemu.sh --timeline timelines/dos-osk-type-dir.tl
#
# CAVEAT -- the first six events depend on persisted launcher state. retro-go
# restores the last-launched ROM's game menu, so the two `press b` back out to
# the MS-DOS coverflow at 12/12 (WOLF3D) and the four `press right` wrap round to
# 04/12 (freedos). If your extflash.bin has a different last-launched entry, or
# roms/dos/ holds a different set of images, re-derive that prefix with a
# screenshot -- everything from 50.0 onward is state-independent.
#
# From 52.0 the grid selection starts at layer 1 row 1 cell 0 ('a') and is walked
# to 'd' (+3), 'i' (+5), 'r' (row 2, index 4) and ENT (row 1, index 14).
# Expected in osk-result.png: "C:\>dir" (or "A:\>dir") echoed by the guest,
# a directory listing, "KBD L1" and the echo "dir" in the top bar, and the two
# key rows in the bottom bar.

2.500  press b
3.500  press b
4.500  press right
5.000  press right
5.500  press right
6.000  press right
8.000  press a
10.000 press a

50.000 press game

52.000 press right
52.400 press right
52.800 press right
53.200 press a
53.600 press right
54.000 press right
54.400 press right
54.800 press right
55.200 press right
55.600 press a
56.000 press down
56.400 press left
56.800 press left
57.200 press left
57.600 press left
58.000 press a
58.400 screenshot gwemu-osk-typed.png

59.000 press up
59.400 press right
59.800 press right
60.200 press right
60.600 press right
61.000 press right
61.400 press right
61.800 press right
62.200 press right
62.600 press right
63.000 press right
63.400 press a
64.900 screenshot gwemu-osk-result.png

66.000 press game
67.500 screenshot gwemu-osk-closed.png
68.500 quit
