# MS-DOS core (8086tiny) — porting notes

Status: **boots to a DOS prompt and takes button input.** MS-DOS 6.22 and FreeDOS both
reach `A:\>`. Text mode renders in authentic CP437 and the CGA blit now renders real
games — Alley Cat is playable. G&W buttons reach the guest keyboard
(`dos_input.c`): TOPBENCH was driven with the d-pad and A/Enter to a **SCORE of 23**.
No audio, no on-screen keyboard, so only five distinct keys are reachable.

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

Categories with docs today: `video` (10 files), `storage`, `input`, `audio`.
Still unwritten: memory map, integration, CPU & performance, BIOS & system services.

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
- `Core/Src/porting/dos/dos_input.c` — buttons → guest keystrokes. The mapping is the
  `dos_key_map[]` table; edge detection against the previous frame turns button state into
  one key-down per press and one key-up per release, encoded in the BIOS's SDL word
  format. **Change the mapping by editing the table, nothing else.** Traps live in
  `external/8086tiny/STATUS.md` (queue, word format, absence of typematic repeat).
- `APPID_DOS` in `Core/Inc/retro-go/appid.h`, and an `add_emulator("MS-DOS", "dos", ...)`
  entry in `Core/Src/retro-go/rg_emulators.c`.
- Build wiring in `Makefile` (`DOS_C_SOURCES`) and `Makefile.common` (`DOS_OBJECTS`, vpath,
  link line, `build/dos` mkdir, compile rule).

## What is missing, roughly in order

1. **On-screen keyboard.** Basic input is done (`dos_input.c` + the injection queue in
   `8086tiny.c`), but only eight physical inputs exist, so the guest can be handed five
   distinct keys — enough to play a game, not enough to type a command. The OSK in the
   reserved letterbox bars is mandatory, not optional. Design in
   `external/8086tiny/docs/input/03-onscreen-keyboard.md`.
2. **Graphics modes beyond CGA.** CGA is now confirmed end-to-end: Alley Cat's title
   screen and playfield both render, so mode detection works for a 1984 title. Mode 13h,
   EGA/Mode X and Hercules are still unbuilt — see `docs/video-roadmap.md`.
3. **Audio.** Not wired. PC speaker only for v1; see `docs/audio-roadmap.md`.
4. **Two BIOS gaps.** `INT 10h AH=0Bh` (set CGA palette/background) is not implemented at
   all, so a game setting its background that way gets defaults. And
   `int10_switch_to_cga_gfx` clears with `char=0/attr=7`, which as pixel data is a stripe
   pattern for one frame.
5. **CPU speed has a first number.** TOPBENCH scores **23**, matching an IBM PS/2 Model
   P70 / AT&T 6386 WGS (286-class) — but every one of its six sub-timings came back as
   exactly 215 µsec, which is not credible and almost certainly reflects the 55 ms
   granularity of the guest clock rather than the CPU. Treat 23 as a baseline to improve
   against, not as a measurement of anything specific, and fix the timer resolution
   before trusting the breakdown.

## The memory map — resolved, for reference

`dos_fold()` (`8086tiny.c`) folds the guest 1 MB into 780 KB: 640 KB conventional
identity-mapped, a 64 KB video aperture at guest `0xB0000`, a 64 KB BIOS+register window at
guest `0xF0000`, 4 KB windows for the BIOS's `C000:0` and `C800:0` shadows, everything else
to a scratch page. Dropping the `0xC0000`-`0xEFFFF` option-ROM hole is what makes it fit —
**that hole is not entirely dead, which is why the two 4 KB windows exist.**

`REGS_BASE` is back at a stock `0xF0000` guest address, so `0xB8000` is reachable and
`-DNO_GRAPHICS` is gone. Only two sites apply the fold: `SEGREG` and the instruction fetch.

Guest memory is a **BSS array inside the overlay**, so overflow is a link-time `ASSERT`
failure rather than silent corruption. The overlay links at `0x24025800` — the LCD bonus
area, PICO-8 pattern — and because `.lcd_pool` spans all of RAM_UC (`__lcd_pool_end__ ==
__RAM_EMU_START__`) that yields **874 KB in one unbroken run**. 780 KB guest + code/BSS
reaches 813 KB, leaving 61 KB.

Total budget, verified: 874 KB AXI (LUT8) + 120 KB AHB + 64 KB ITCM. **DTCM is not
available** — it is firmware's (~17 KB data/bss, 85 KB heap, 20 KB stack); apps reach it
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

  **But this is a dead end in method, not destination.** ITCM *is* reachable — PCE and GBA
  both get it as overlay cores via `.overlay_pce_itc` / `.overlay_gba_itc`
  (`STM32H7B0VBTx_SDCARD.ld:338`, `:773`), which have explicit LMAs and are memcpy'd in by
  `run_internal_emu()` before BSS is zeroed (`rg_emulators.c:1416`). Once DOS is an
  overlay core the same door opens, and for an instruction-dispatch interpreter that 64 KB
  is the highest-value memory on the chip. Do not re-attempt the attribute; do plan the
  overlay ITC section.
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
