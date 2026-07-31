set pagination off
set confirm off
set backtrace past-main off

target extended-remote :1234

# ---------------------------------------------------------------------------
# Log forwarding.
#
# retro-go's stdout/stderr funnel through _write() in Core/Src/syscalls.c.
# We break there and print the buffer the caller handed us, then resume.
#
# This is deterministic: every write is observed exactly once, in order, with
# no dependence on wall-clock timing. The previous poll-the-log-buffer designs
# (both the blocking `continue` + while-loop, which never reached the loop, and
# the `interrupt`-every-second async variant) raced the target and dropped
# output.
#
# gdb's printf has no "%.*s", and `ptr` is not NUL-terminated, so emit exactly
# `len` bytes by hand.
# ---------------------------------------------------------------------------
break _write
commands
  silent
  if file == 1 || file == 2
    set $n = len
    set $i = 0
    while $i < $n
      printf "%c", ptr[$i]
      set $i = $i + 1
    end
  end
  continue
end

# ---------------------------------------------------------------------------
# Fault trap.
#
# Every fault (hard/bus/usage/mem) funnels through common_fault_handler_c in
# Core/Src/stm32h7xx_it.c before BSOD() paints the screen. Breaking here gets
# the stacked exception frame with full context, and puts the whole report on
# stdout so it is readable without squinting at the emulated LCD.
#
# `type` matches the BSOD_* enum; `frame` is sContextStateFrame.
# ABFSR (0xE000EFA8) names the bus interface for imprecise faults:
#   bit0/1 = ITCM/DTCM, bit2 = AHBP (peripheral space), bit3 = AXIM (RAM/flash).
# ---------------------------------------------------------------------------
break common_fault_handler_c
commands
  silent
  printf "\n\n=== !!! FAULT CAUGHT (type=%d) !!! ===\n", type
  printf "--- stacked frame ---\n"
  printf "  PC  =0x%08x   LR  =0x%08x   xPSR=0x%08x\n", frame->return_address, frame->lr, frame->xpsr
  printf "  r0  =0x%08x   r1  =0x%08x   r2  =0x%08x   r3  =0x%08x   r12 =0x%08x\n", frame->r0, frame->r1, frame->r2, frame->r3, frame->r12
  printf "--- fault status registers ---\n"
  printf "  CFSR =0x%08x\n", *(unsigned int *)0xE000ED28
  printf "  HFSR =0x%08x\n", *(unsigned int *)0xE000ED2C
  printf "  MMFAR=0x%08x\n", *(unsigned int *)0xE000ED34
  printf "  BFAR =0x%08x\n", *(unsigned int *)0xE000ED38
  printf "  ABFSR=0x%08x\n", *(unsigned int *)0xE000EFA8
  printf "--- faulting PC ---\n"
  info symbol frame->return_address
  printf "--- backtrace ---\n"
  bt
  printf "--- registers ---\n"
  info registers
  printf "=== END FAULT REPORT ===\n"
  # $fault_keep is always set by run_gwemu.sh (0 or 1) before this script is
  # sourced, so it is never void here.
  #
  # 0 (default): quit non-zero. The exit code is how a caller or CI detects a
  #   crashed run without parsing the log, and stopping here means the report
  #   above is the last thing that happened -- nothing overwrites it.
  #
  # 1 (--keep-on-fault): resume instead. common_fault_handler_c goes on to call
  #   BSOD(), so the firmware paints its own blue screen and the emulator stays
  #   up -- which is the only way to see, photograph or drive the BSOD, and the
  #   BSOD is firmware that needs testing too. The trade is that the exit code
  #   is NO LONGER a dependable fault signal in this mode -- measured: a resumed
  #   run whose inferior then exited normally still returned 1 -- so callers
  #   must grep the log for "FAULT CAUGHT" rather than trusting $?.
  if $fault_keep == 1
    printf "[gwemu_log] --keep-on-fault: resuming so the firmware BSOD can paint.\n"
    printf "[gwemu_log] exit code is not a fault signal here; grep for 'FAULT CAUGHT'.\n"
    continue
  else
    quit 1
  end
end

# Assertions are the other common abort path; catch them the same way.
break __assert_func
commands
  silent
  printf "\n\n=== !!! ASSERT FAILED !!! ===\n"
  bt
  printf "=== END ASSERT REPORT ===\n"
  if $fault_keep == 1
    printf "[gwemu_log] --keep-on-fault: resuming.\n"
    continue
  else
    quit 1
  end
end

continue
