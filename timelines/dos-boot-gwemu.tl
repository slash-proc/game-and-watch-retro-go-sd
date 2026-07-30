# Same as dos-boot.tl but timed for gwemu, which boots much faster than hardware.
# Hardware needs ~11s to reach the launcher; gwemu reaches it in ~2s.
# Keep the two separate -- gwharness.py (hardware) and run_gwemu.sh share dos-boot.tl.
2.0   press a
4.5   press a
40.0  quit
