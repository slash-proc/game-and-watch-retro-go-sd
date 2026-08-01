# MS-DOS core (8086tiny) — porting notes

Status: **boots to a DOS prompt and can be typed at.** MS-DOS 6.22 and FreeDOS both
reach `A:\>`. Text mode renders in authentic CP437 and the CGA blit now renders real
games — Alley Cat is playable. PC-speaker audio is wired but unheard.
G&W buttons reach the guest keyboard
(`dos_input.c`): TOPBENCH was driven with the d-pad and A/Enter to a **SCORE of 23**.
The on-screen keyboard (`dos_osk.c`) makes the rest of the keyboard reachable —
`dir` has been typed at a FreeDOS prompt under gwemu and echoed back. PC-speaker
audio is built but has never been heard.

**This file is a summary and a list of traps. `external/8086tiny/STATUS.md` is the
authority on current state** — it is maintained per-change and this one is not.
`external/8086tiny/GNW_PORT.md` is the original plan document and is **stale**; several
things it proposes were tried and abandoned.

## Where the documentation lives — read this first

**All design documentation is in the submodule**, and it is more current and more
detailed than this file. This file is only a summary and a list of traps.

```
external/8086tiny/
  STATUS.md                    <- START HERE. Status, blockers, memory budget, traps. ~100 lines.
  docs/
    decisions.md               <- Settled calls + WHY. Read before relitigating anything.
    sprint-first-boot.md       <- The active work plan.
    <category>-roadmap.md      <- Feature checklist per category, links into the subdir below
    <category>/                <- Design detail, numbered 01.., 02..
    upstream/manual.md         <- Upstream's own manual. Describes SDL/terminal behaviour
                                  that does NOT apply here — see its caveats section.
  GNW_PORT.md                  <- STALE. Superseded. Do not trust it.
```

Categories with docs today: `video` (10 files), `cpu` (2 files), `storage`, `input`, `audio`.
Still unwritten: memory map, integration, BIOS & system services.

### The reading path

1. **`STATUS.md`** — what works, what blocks, the verified memory budget.
2. **`docs/decisions.md`** — twelve-plus settled calls with the reasoning. Most have a
   concrete failure mode behind them, not a preference. If you are about to change one,
   find out why it was made first.
3. **`docs/sprint-first-boot.md`** — the current milestone and its ordered steps.
4. **`docs/<category>-roadmap.md`** — then drill into `docs/<category>/` as needed.

### Conventions

- `> **OPEN:**` marks an unresolved decision. Do not silently resolve one — raise it.
- `> **DECIDED:**` marks a resolved one, usually with a one-line rationale.
- Every claim about existing code cites `file:line`. Match that; unsourced assertions
  about this codebase have been wrong more than once.
- Roadmaps are checklists and stay brief. Detail belongs in the child documents.
- `STATUS.md` is intentionally capped at ~100 lines. Growing content goes to `docs/`.

### When adding a category

Create `docs/<name>-roadmap.md` following the exact shape of `docs/video-roadmap.md`,
plus a `docs/<name>/` subdirectory. Add a row to the category table in `STATUS.md`.

## What exists

- `external/8086tiny/8086tiny.c` — the emulator, patched (see *Submodule hygiene*).
  `main()` was split into `dos_cpu_init(argc, argv)` and `dos_cpu_frame(cycles)` so the
  launcher can run it a frame at a time instead of owning the main loop.
- Disk I/O converted from raw `open`/`read`/`write`/`lseek` to `FILE*`, so FatFS can serve
  it. The original used an IOCCC-style `(int(*)())` function-pointer cast that GCC 15
  rejects; that is gone.
- `Core/Src/porting/dos/main_dos.c` — `app_main_dos()`. Sets up the frame loop, audio
  timing and `common_emu_*` bookkeeping, and calls `dos_input_update()` once per frame.
- `Core/Src/porting/dos/dos_cpu.c` — guest CPU speed profiles (`dos_cpu_profiles[]`, a
  data table like `dos_key_map[]`), MAX mode's deadline chunk loop, the retro-go options
  row, and the cpu/blit/idle profiler. **Change the speeds by editing the table.**
  Design in `external/8086tiny/docs/cpu/`. Two things to know before touching it:
  MAX mode refreshes the watchdog *inside* the chunk loop and must keep doing so, and
  the `DOS: prof …` line is intentionally not behind a debug flag.
- `Core/Src/porting/dos/dos_input.c` — buttons → guest keystrokes. The mapping is the
  `dos_key_map[]` table; edge detection against the previous frame turns button state into
  one key-down per press and one key-up per release, encoded in the BIOS's SDL word
  format. **Change the mapping by editing the table, nothing else.** Traps live in
  `external/8086tiny/STATUS.md` (queue, word format, absence of typematic repeat).
  GAME is no longer a table row — it toggles the on-screen keyboard.
- `Core/Src/porting/dos/dos_osk.c` — the on-screen keyboard, drawn into the two 20-row
  letterbox bars that `dos_video.c` reserves and never writes. Three layers (letters /
  digits+punctuation / function keys), sticky Shift/Ctrl/Alt, d-pad to move and A to
  press; GAME toggles it, TIME cycles the layer, B is Backspace. **Two things here fail
  silently if changed carelessly:** every state change repaints into *both*
  framebuffers, because a single paint looks correct in a screenshot and flickers at
  the swap rate on hardware; and a Shifted key must set `0x1000` itself, because the
  SDL decode path never consults `a2shift_tbl` (only the ASCII path does,
  `bios.asm:694`) — so keysym `!` alone types `1`. Design and the full trap list:
  `external/8086tiny/docs/input/03-onscreen-keyboard.md`, `docs/traps.md` under *Input*.
- `APPID_DOS` in `Core/Inc/retro-go/appid.h`, and an `add_emulator("MS-DOS", "dos", ...)`
  entry in `Core/Src/retro-go/rg_emulators.c`.
- Build wiring in `Makefile` (`DOS_C_SOURCES`) and `Makefile.common` (`DOS_OBJECTS`, vpath,
  link line, `build/dos` mkdir, compile rule).

## What is missing, roughly in order

1. **Graphics modes beyond CGA.** CGA, **VGA mode 13h** (320x200x256 linear) and **EGA
   mode 0Dh** (320x200x16 planar) all render, each host-verified pixel-exact
   (`test286/run13h.sh`, `test286/run_ega.sh`). Planar reuses the existing 64 KB A-segment
   aperture — 16 KB CPU read shadow at the bottom, four interleaved planes above — and
   allocates no new guest RAM. **EGA 0Eh/10h, page flipping, Mode X and Hercules are not
   built, and 0Eh/10h/page-flipping are blocked on RAM, not effort** (the arithmetic is in
   `external/8086tiny/docs/video/11-ega-planar.md`).
2. **Audio.** PC speaker is **built** (`dos_audio.c`, `dos_audio_wave.h`,
   `dos_spkr_take()` in `8086tiny.c`) but has never been heard — no gwemu run and
   no hardware run. Two things to know before touching it: the `spkr_en` latch is
   accumulate-on-write / clear-once-per-fill and `dos_spkr_take()` must keep
   exactly one caller, and the 1077-sample DMA ceiling is enforced at the write in
   `dos_audio_submit()` because the firmware itself bounds-checks nothing. Host
   tests: `external/8086tiny/test286/runaudio.sh` (pitch) and `runspkr.sh`
   (latch/gate). Design in `docs/audio-roadmap.md`.
3. **Two BIOS gaps.** `INT 10h AH=0Bh` (set CGA palette/background) is not implemented at
   all, so a game setting its background that way gets defaults. And
   `int10_switch_to_cga_gfx` clears with `char=0/attr=7`, which as pixel data is a stripe
   pattern for one frame.
4. **CPU speed is selectable and instrumented; the numbers are not yet explained.**
   Profiles (XT/Turbo/286/MAX) live in `dos_cpu.c` and a per-second `DOS: prof …` line
   reports achieved instructions/frame, instructions/second, **ARM cycles per guest
   instruction**, and the cpu/blit/idle split. TOPBENCH scores **34/35 on the device**
   at the default 20,000 and **23 under gwemu** (gwemu is the slower of the two). Both
   runs returned six *identical* 215 µsec sub-timings, which is not credible — prime
   suspect is the guest clock's 55 ms quantisation, so the aggregate may be usable but
   the breakdown is not. At 280 MHz the default profile costs ~233 ARM cycles per 8086
   instruction against 20–50 for a decent interpreter, so the ceiling looks like an
   efficiency problem. `external/8086tiny/docs/cpu-roadmap.md` lists the unpulled levers. It
   used to name `.overlay_dos_itc` first; **that ordering is obsolete** — ITCM has since been
   measured and refuted (see *Things already ruled out*). The one remaining double-digit
   lever the docs endorse is the 4K decode cache (`docs/cpu/07-decode-cache-design.md`).

## The memory map — resolved, for reference

`dos_fold()` (`8086tiny.c`) maps the guest 1 MB onto **two physical regions**, and it
returns a **pointer**, not an index — an index into one array is what used to force every
guest-addressable byte into one AXI allocation. AXI `mem[]` holds 640 KB conventional plus
the 64 KB aperture at guest `0xA0000` (identity, one compare for both) and the 64 KB
BIOS+register window at guest `0xF0000`. AHB SRAM holds the 32 KB colour-text window at
`0xB8000`, the BIOS's `C000:0`/`C800:0` shadows and the scratch page — all cold, all behind
the two hot tests in `dos_fold_cold()`. Dropping most of the `0xC0000`-`0xEFFFF` option-ROM
hole is what makes it fit — **that hole is not entirely dead, which is why the two 4 KB
windows exist.** Full accounting: `external/8086tiny/docs/memory/01-two-region-fold.md`.

**Three ways to break this silently** (all compile): using a folded address as a *number*
(LEA), bounding a transfer against `RAM_SIZE` alone, and adding a region test in front of
the two hot compares. See `docs/traps.md` under *Memory and layout*.

`REGS_BASE` is back at a stock `0xF0000` guest address, so `0xB8000` is reachable and
`-DNO_GRAPHICS` is gone. Only two sites apply the fold: `SEGREG` and the instruction fetch.

Guest memory is a **BSS array inside the overlay**, so overflow is a link-time `ASSERT`
failure rather than silent corruption. The overlay links at `0x24025800` — the LCD bonus
area, PICO-8 pattern — and because `.lcd_pool` spans all of RAM_UC (`__lcd_pool_end__ ==
__RAM_EMU_START__`) that yields **874 KB in one unbroken run**. 780 KB guest + code/BSS
reaches 813 KB, leaving 61 KB.

Total budget, verified against the linked ELF (`arm-none-eabi-nm build/gw_retro_go.elf`):
874 KB AXI (LUT8) + **122,880 B AHB** + 64 KB ITCM.

The AHB figure used to read 87,904 B, because `.gba_ahbram` statically reserved 34,976 B
whichever core was resident. **That reservation is gone** — commit `4252a648` gave the
section an explicit VMA of `ORIGIN(AHBRAM)` so it is *overlaid* on the AHB heap instead of
charged to the region (`STM32H7B0VBTx_SDCARD.ld:669`). Verified: `__ahbram_heap_start__ ==
__gba_ahb_start__ == 0x30000000`, and `__ahbram_audio_start__ = 0x3001e000`, so the pool is
`0x1e000` = 122,880 B (128 KB less the 8 KB `.audio` DMA reserve). GBA is unharmed because
`main_gba.c:766` claims those 34,976 B back with an `ahb_only_malloc()` before its first
allocation, guarded by both a runtime check and the link-time ASSERT at `ld:1104`.

**AXI headroom is 16,520 bytes**, measured: `__RAM_EMU_END__` (`0x24100000`) −
`_OVERLAY_DOS_BSS_END` (`0x240fbf78`). The older 49,368/49,352 figure predates the HMA and
the 186/286 opcode work and is **stale — do not quote it**.

**DTCM is not available** — it is firmware's (~17 KB data/bss, 85 KB heap, 20 KB stack); apps reach it
only through `malloc`. See the RAM ownership contract.

## Submodule hygiene

`external/8086tiny` points at **`git@github.com:slash-proc/8086tiny.git` — our own fork**,
already carrying a `Port to Game and Watch (Retro-Go)` commit.

An earlier revision of this file said the dirty tree had to become a managed patch, as
third-party cores do. **That was wrong** — it is not a third-party submodule. Port work is
committed to the fork and the submodule pointer is bumped, which is also what makes
`docs/` a legitimate home for the design documentation.

The tree is currently dirty (`8086tiny.c`, `GNW_PORT.md`), so the build needs
`CHECK_DIRTY_SUBMODULE=0` until those changes are committed upstream in the fork.

## Building and running

```bash
make -j$(nproc) CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
     DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=1 CHEAT_CODES=1 release gwemu_release
./scripts/run_gwemu.sh
```

See `docs/gwemu.md`. The harness forwards retro-go's log to stdout and traps exceptions
automatically.

## Things already ruled out — do not re-litigate

- **ITCM via `__attribute__((section(".itcram")))` on a non-overlay build.**
  `GNW_PORT.md` proposes tagging the interpreter loop this way. It does not work and was
  removed: the SD linker script collects no `.itcram*` input sections, so the attribute
  produced an orphan section with no copy-to-ITCM step. `._itcram` and `._itcram_hot` are
  both size 0 in this build.

  An earlier revision of this file called that "a dead end in method, not destination", and
  said the overlay ITC section would open the door because "for an instruction-dispatch
  interpreter that 64 KB is the highest-value memory on the chip".

  **That prediction was tested and is wrong. ITCM is closed for DOS — all three TCM
  variants were measured and all three LOST** (`external/8086tiny/docs/cpu/05-perf-handover.md:646-692`):

  | variant | what moved | result |
  |---|---|---|
  | `DOS_ITC_DATA=1` | `bios_table_lookup` (5 KB) + decode state → ITCM | **−1.4%** (cpi 207→210) |
  | DTCM | hottest small data → DTCM | **−2%** |
  | `DOS_ITC_CODE=1` | `dos_cpu_frame` (21 KB) → ITCM, via exactly the overlay ITC section recommended above | **−1.1%** (11,958 → 12,090 µs/f) |

  Branches: superproject `perf/dos-itcm-code` @ `c96d49a8`, submodule `perf/itcm-code` @
  `6abed58`. The reasoning error is recorded at `05-perf-handover.md:666`: **21 KB is the
  static size of `dos_cpu_frame`, not its hot working set** — a ~120-case switch is large on
  disk but only a fraction runs per guest instruction, so the I-cache was never thrashing.
  And ITCM is not free: it forfeits I-cache prefetch/line-fill and adds a veneer indirection
  on every call out (`:670`).

  The DWT instrumentation closes both halves directly (`:466-501`): `LSUCNT` is **6.4 of 228
  cycles = 2.5%**, so the interpreter is not data-latency bound, and no phase of the loop
  exceeds 29%, so there is no hot spot to relocate. `05-perf-handover.md:674` states it
  flatly: **"Do not propose a fourth TCM experiment."**

  So `.overlay_dos_itc` stays declared and **empty on purpose**. The plumbing is complete
  and correct — `rg_emulators.c:1706-1712` copies the image, guarded by `if (dos_itc_size)`,
  with `__DSB()/__ISB()` before any fetch — so seeding it later is still a one-line change.
  There is just no evidence that anything should go in it. Do not re-attempt the attribute,
  and do not re-attempt the overlay ITC section either.
- **A boot-time MemManage fault in `rg_get_logo`.** A long investigation concluded this was
  a gwemu bug, not firmware: an `UNPREDICTABLE` `MPU_RBAR` write that QEMU resolved
  differently from silicon. **Fixed in gwemu 0.0.19.** It reproduced only under emulation
  and never on hardware. If you see it again, check your gwemu version first. The full
  write-up and reproduction images live in the gwemu project's `backup/memfault/`.
- **`REGS_BASE` relocation as a long-term answer.** It was what forced `NO_GRAPHICS`.
  Resolved: `REGS_BASE` is back at a stock `0xF0000` guest address, split from `REGS_SEG`
  so it is not folded twice. Do not move it again.
- **`gettimeofday()` as a millisecond source.** It resolves to `GW_GetCurrentMillis()`
  (`Core/Src/retro-go/rg_rtc.c:251`), whose sub-second part comes from the RTC SubSeconds
  register — **which gwemu does not model** (`tv_usec` measured pinned at 996000 forever).
  So it works on hardware and silently freezes guest time under emulation. The guest clock
  is driven off `HAL_GetTick()` instead, anchored once to the RTC for the wall date.

> **A repeated lesson, worth internalising: unverified claims about this codebase have been
> wrong far more often than not** — the bonus pool's cacheability, `BUILD_DIR` isolation,
> BDA `0x484`'s meaning, the CRTC wrap granularity, whether the font pack had narrow fonts,
> which script assembles `roms/`, and the detection method for a contaminated build were all
> asserted confidently and all wrong. Check against source and cite `file:line`.
