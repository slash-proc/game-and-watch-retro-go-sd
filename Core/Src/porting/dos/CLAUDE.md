# MS-DOS core (8086tiny) — porting notes

Status: **early**. It builds and links, the launcher shows an MS-DOS tab, and the CPU core
is split into init/step so it can be driven per frame. Nothing is displayed, no input is
wired, and selecting a ROM does nothing yet.

`external/8086tiny/GNW_PORT.md` is the original plan document and is **out of date** —
several things it proposes were tried and abandoned. Trust this file where they disagree.

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
  timing and `common_emu_*` bookkeeping. `dos_blit()` is an empty TODO.
- `APPID_DOS` in `Core/Inc/retro-go/appid.h`, and an `add_emulator("MS-DOS", "dos", ...)`
  entry in `Core/Src/retro-go/rg_emulators.c`.
- Build wiring in `Makefile` (`DOS_C_SOURCES`) and `Makefile.common` (`DOS_OBJECTS`, vpath,
  link line, `build/dos` mkdir, compile rule).

## What is missing, roughly in order

1. **It is not an overlay core.** Every other emulator links into a `.overlay_<name>`
   section that is streamed from SD into `._ram_exec` at launch; DOS currently links
   straight into internal flash. Follow the pattern at
   `STM32H7B0VBTx_SDCARD.ld:918-938` (`.overlay_tama` / `.overlay_tama_bss`), plus the
   `EXTRACT_INTERNAL_CORE_BIN_WITH_HEADER` line in `Makefile.common:1573` and the `sdpush`
   line near `:1669`. Until then it burns scarce internal flash and cannot get a sensible
   RAM budget.
2. **Nothing dispatches to it.** `rg_emulators.c` has no `strcmp(system_name, "MS-DOS")`
   branch, so picking a ROM falls through the chain harmlessly and returns. Dispatch is
   now table-driven (`emu_dispatch_t` + `run_internal_emu()`, `rg_emulators.c:1394`), so
   adding DOS is one struct line alongside `emu_tama` (`:1517`) and one `else if` (`:1673`)
   — cheaper than it used to be. Add both once the overlay exists.
3. **The memory map is unresolved** — see below.
4. **Display.** `dos_blit()` is empty. See `external/8086tiny/docs/` for the full design.
   The headline finding: **upstream 8086tiny has no graphical text mode at all** — it
   renders only in graphics mode (`8086tiny.c:733`) and sends text mode to stdout via
   termios. DOS boots to text mode, so the *primary* display path does not exist upstream
   and must be written from scratch. The video aperture is 64 KB at `0xB0000` (not 32 KB
   at `0xB8000`): `8086tiny.c:754` uses Hercules bank 1 at `B000:0` and CGA/bank 2 at
   `B800:0`.
5. **Input and audio.** Neither is wired.

## The memory map problem (the real design decision)

8086tiny wants a flat 1 MB physical space: conventional RAM up to `0xA0000`, CGA video at
`0xB8000`, BIOS and its register block at `0xF0000`. That does not fit anywhere on this
part.

The current code is a stopgap and should not be built on:

- `RAM_SIZE` reduced to 256 KB, addresses masked with `& 0x3FFFF`.
- `REGS_BASE` moved from `0xF0000` to `0x30000` to fit, which puts the BIOS somewhere the
  standard layout does not expect and makes `0xB8000` unreachable — hence `-DNO_GRAPHICS`
  and the empty blit.
- `mem` points at the LCD bonus pool via `lcd_get_bonus_pool()`, which is only ~146 KB in
  LUT8 mode while the mask allows 256 KB. **Anything above 146 KB runs off the end of the
  pool into `RAM_EMU`.**

  (An earlier revision of this file called the bonus pool uncached and therefore slow.
  **That was wrong.** In LUT8 mode `lcd_setup_framebuffers` calls
  `mpu_set_lcd_pool_uncached_range(fb_footprint)`, shrinking the uncached window to just
  the two framebuffers; everything above them is Normal cacheable. See `gw_lcd.c:317-325`.)

The bonus pool is **contiguous with `RAM_EMU`** — `.lcd_pool` spans all of RAM_UC, so
`__lcd_pool_end__ == __RAM_EMU_START__`. Linking the overlay at `0x24026800` (the PICO-8
pattern, `STM32H7B0VBTx_SDCARD.ld:887`) therefore yields **870 KB in one unbroken run**,
not 724 KB. That is enough for a translated map (640 KB conventional + a video window + a
BIOS window) but not for a flat 1 MB, so address translation replacing the `& 0x3FFFF`
mask is required either way.

Total budget, verified: 870 KB AXI (LUT8) + 120 KB AHB + 64 KB ITCM. **DTCM is not
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
- **`REGS_BASE` relocation as a long-term answer.** It is what forces `NO_GRAPHICS`.
  Solving the memory map properly removes the need for it.
