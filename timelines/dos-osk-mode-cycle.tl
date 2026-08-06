# GAME's three-state cycle: off -> keyboard -> mouse -> off, one screenshot per
# state, so the whole cycle is one strip of pictures rather than a claim.
#
#   ./scripts/run_gwemu.sh --timeline timelines/dos-osk-mode-cycle.tl
#
# The state machine itself is covered without a device or an emulator by
# external/8086tiny/test286/runoskmode.sh, which compiles the real dos_osk.c and
# has the previous two-state toggle as its negative control. THIS timeline
# covers what that one cannot: that the mode reached by a real GAME press is
# rendered onto a real panel, through the real double-buffered swap. A mode that
# is right in the state machine and invisible on screen passes there and fails
# here, which is the whole reason both exist.
#
# CAVEAT -- the first eight events depend on persisted launcher state, and are
# copied verbatim from timelines/dos-osk-type-dir.tl. retro-go restores the
# last-launched ROM's game menu, so the two `press b` back out to the MS-DOS
# coverflow at 12/12 (WOLF3D) and the four `press right` wrap round to 04/12
# (freedos). If your extflash.bin has a different last-launched entry, or
# roms/dos/ holds a different set of images, re-derive that prefix with a
# screenshot -- everything from 50.0 onward is state-independent.
#
# WHAT EACH SCREENSHOT MUST SHOW. Check all four; three of them passing is the
# failure mode this is built to catch, because a two-state toggle produces the
# right picture for 1 and 4 and the WRONG one for 2.
#
#   cycle-0-off.png    Both letterbox bars black. No status text at all. This is
#                      the anti-vacuous frame: without it, a bar that never
#                      blanks would make every later shot look correct.
#   cycle-1-kbd.png    Top bar "KBD L1   SHF CTL ALT   GAME>MSE", bottom bar the
#                      two key rows starting at 'a'. The GAME>MSE hint is what
#                      replaced the MSE grid key and is the only thing that tells
#                      a user mouse mode exists, so its absence is a real bug.
#   cycle-2-mouse.png  Top bar "MOUSE <x>,<y> CUR+ CB<n>/<n>" on line 1 and
#                      "dpad move  B/A click  TIME kbd  GAME off" on line 2.
#                      BOTTOM BAR BLACK -- the key grid is not reachable in mouse
#                      mode and drawing it would offer keys the buttons cannot
#                      press. A bottom bar with letters in it here means the
#                      third press landed in the keyboard, not the mouse.
#   cycle-3-off.png    Identical in character to shot 0: both bars black, guest
#                      image untouched. This is the press that proves it is a
#                      CYCLE and not a toggle that got stuck in mouse mode.
#
# Then two more, to pin the parts a single lap cannot distinguish:
#
#   cycle-4-kbd.png    A second lap reaches the keyboard again -- i.e. OFF really
#                      is the state the cycle returns to, not a dead end.
#   cycle-5-time.png   From mouse mode, TIME steps BACK to the keyboard (the
#                      retained shortcut). Must look like cycle-1-kbd.png.
#   cycle-6-off.png    Both bars black again, ending where shot 0 started.

2.500  press b
3.500  press b
4.500  press right
5.000  press right
5.500  press right
6.000  press right
8.000  press a
10.000 press a

# Guest at a prompt, OSK never opened.
50.000 screenshot gwemu-osk-cycle-0-off.png

# ---- lap 1 -----------------------------------------------------------------
# 1.5 s after each press: dos_osk.c repaints over two frames so both
# framebuffers carry the same bars, and a screenshot taken between the two
# catches a half-updated pair. 1.5 s is far more than the two frames need.
51.000 press game
52.500 screenshot gwemu-osk-cycle-1-kbd.png

54.000 press game
55.500 screenshot gwemu-osk-cycle-2-mouse.png

57.000 press game
58.500 screenshot gwemu-osk-cycle-3-off.png

# ---- lap 2: the cycle repeats, and TIME still steps back -------------------
60.000 press game
61.500 screenshot gwemu-osk-cycle-4-kbd.png

63.000 press game
64.500 press time
66.000 screenshot gwemu-osk-cycle-5-time.png

# Back to the guest, so the run ends in the state it started in. TWO presses,
# not one: shot 5 left it in the KEYBOARD (TIME stepped back there), so the lap
# still has mouse mode to go through before it reaches off.
67.500 press game
69.000 press game
70.500 screenshot gwemu-osk-cycle-6-off.png
71.500 quit
