# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A multi-emulator launcher for the Nintendo Game & Watch (STM32H7B0VB MCU). This is the SD-card variant of [sylverb/game-and-watch-retro-go](https://github.com/sylverb/game-and-watch-retro-go-sd) — emulator cores and ROMs live on a microSD card rather than being baked into the external flash. Each cartridge format (NES, GB, MSX, Genesis, etc.) has its own emulator core ported to STM32 with very tight memory and CPU constraints.

## Build / flash workflow

The build system is plain GNU Make. `Makefile` lists source files; `Makefile.common` contains the rules, toolchain setup, and configuration variables.

**Toolchain.** Requires `arm-none-eabi-gcc` v10+ (CI/Docker uses 15.2.rel1). Either put the toolchain on `PATH` or set `GCC_PATH=/path/to/bin` on the make command line. `gnwmanager` (Python, from `requirements.txt`) is required for any flashing target — install via `python3 -m pip install -r requirements.txt`.

**Common targets** (run from repo root, all support `-j$(nproc)`):

- `make docker` — is the proper way to check if project builds and links. If docker is not available, use `make -j8 CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 release` to use locally installed compilation environment.

- `make help` — full list of build flags with current values.

**Developing and testing on the device** — which bank to link into and why, which parameters must match between a gwemu run and a hardware run, gwemu's SD-card behaviour, and how to boot straight into a chosen game unattended: [docs/device-development.md](docs/device-development.md).

There are no automated tests. Verification is manual: build, flash, run on hardware.

**Configuration knobs that change layout (not just behavior)** — pass on the make command line:

- `GNW_TARGET={mario,zelda}` — different button mappings and default extflash size.
- `INTFLASH_BANK={1,2}` (or `INTFLASH_ADDRESS=0x08...`) — selects which 128k/256k internal-flash bank the code is linked into. Bank 2 is used with dual-boot OFW patches.
- `SD_CARD=1` (default for this repo) — enables FatFS, omits ROM compression, and uses `STM32H7B0VBTx_SDCARD.ld`. Setting `SD_CARD=0` switches to the all-in-flash variant (different link script, different feature set).
- `EXTFLASH_SIZE_MB`, `EXTFLASH_OFFSET`, `LARGE_FLASH` — external flash sizing (deprecated in favor of `EXTFLASH_SIZE_MB`).
- `CHEAT_CODES=1`, `COVERFLOW=1`, `SHARED_HIBERNATE_SAVESTATE=1`, `DISABLE_SPLASH_SCREEN=1`, `MSX_USE_BANK_2=1`, `FORCE_NOFRENDO=1` — feature toggles. The Docker release build enables `COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1`.
- `CODEPAGE`, `UICODEPAGE`, individual locale flags (`FR_FR`, `RU_RU`, …) — controls font/i18n inclusion.

**Submodule hygiene.** `external/` holds each emulator core as a git submodule. The build refuses to run if submodules are dirty or out of sync — fix with `git submodule update --init --recursive`, or pass `CHECK_DIRTY_SUBMODULE=0` to bypass (the Docker target does this).

## Architecture

### Three storage tiers, one ELF

A single ELF (`build/gw_retro_go.elf`) is partitioned by linker sections into three physical destinations:

1. **Internal flash** (`*_intflash.bin`, from sections `.isr_vector .text .rodata .data .init_array …`) — launcher UI, drivers, FatFS, the always-resident retro-go shell. Linker script: `STM32H7B0VBTx_SDCARD.ld` (or `_FLASH.ld` for non-SD builds).
2. **External flash** (`*_extflash.bin`, sections `._extflash ._itcram_hot ._ram_exec` + every `.overlay_*`) — on SD-card builds this is mostly used during build/test; at runtime cores are streamed from SD instead.
3. **SD card** — `make create_sd_data` extracts each `.overlay_<system>` section into a standalone `cores/<system>.bin` file with `objcopy --only-section`. The launcher loads exactly one core at a time into a fixed RAM region (`._ram_exec`) before running it. Each emulator therefore has the entire "free" RAM (~1 MB) to itself but cannot coexist with another core in memory.

Adding a new emulator means: add its sources to `Makefile`, give it a `.overlay_<name>` section in the linker script, and add a `--only-section=.overlay_<name>` extraction line to `create_sd_data` plus an `sdpush` line in `flash_sd`.

### Source tree layout

- `Core/Src/main.c`, `Core/Src/gw_*.c` — STM32 HAL bring-up, LCD, audio (SAI), buttons, SD driver, RTC, battery (BQ24072), flash chip access, low-level memory allocator. Headers in `Core/Inc/`.
- `Core/Src/porting/<system>/main_<system>.c` — the per-emulator porting layer. This is where most emulator-specific Game & Watch work happens: input mapping, video scaling, audio bridging, savestate hooks, ROM loading, options menus.
- `Core/Src/porting/lib/` — shared helpers used by porting code: FatFs vendor copy, LZ4/LZMA decompressors, HW JPEG decoder, HW SHA1, softspi.
- `Core/Src/porting/odroid_*.c` — the retro-go shell's portability glue (input, display, audio, overlay, sdcard, system). Names come from the original Odroid-GO Retro-Go.
- `retro-go-stm32/` — vendored snapshot of upstream retro-go (launcher UI, settings, common emulator-side helpers in `components/odroid/`, plus three emulator cores `gnuboy-go`, `nofrendo-go`, `pce-go`, `smsplusgx-go`).
- `external/` — git submodules for every other emulator core (`fceumm-go`, `blueMSX-go`, `caprice32-go`, `gwenesis`, `LCD-Game-Emulator`, `stella2014-go`, `prosystem-go`, `PokeMini-go`, `potator`, `tamalib`, `tgbdual-go`, `ccleste-go`, `zelda3`, `smw`, `o2em-go`, `firmware_update`). Each is a third-party emulator/port with its own license; we patch them via `genpatch.py`-managed `.patch` files where present.
- `tools/` — Python utilities. The user-facing ones (per README):
  - `gencovers.py` — generate `.img` cover thumbnails for ROMs (uses `requirements.txt`).
  - `fonttool/`, `png_to_logo.py`, `img2bin.py`, `pllgen.py`, `gen_fceu_palettes_table.py` — asset converters used by the build.
- `scripts/` — shell helpers invoked from the Makefile (size reporting, git tag stamping, release packaging, rom discovery). Run from the repo root.
- `assets/`, `icons/`, `smw_redefines`, `zelda3_redefines` — graphics, system icons, SNES symbol-rename headers for the homebrew SNES ports.

### Adding/modifying a core

Emulator main loop happens in `Core/Src/porting/<system>/`, a loop iteration should run the generation of a frame and to write it in the framebuffer, and to generate the audio samples for the frame. The submodule under `external/<system>` contains machine emulation logic.

The `retro-go-stm32/components/odroid/` API (`odroid_system`, `odroid_overlay`, `odroid_display`, `odroid_input`, `odroid_audio`, `odroid_sdcard`, `odroid_netplay`) is the contract between the launcher and an emulator core. New cores implement against it.

## Things that are easy to get wrong

- **Don't edit files under submodules in `external/`.** Either fix upstream or add/update a patch. The build's submodule-dirty check will reject the build.
- **`SD_CARD=1` and `SD_CARD=0` are very different builds.** Different linker scripts, different feature set, different binary layout. Default in this repo is `1`; the upstream non-SD repo defaults to `0`.
- **`INTFLASH_BANK` selects where retro-go is linked to run.** The device always boots at `0x08000000`. The project assumes bank 2, where a patched OFW in bank 1 jumps to `0x08100000` on Game+Left. For development it is quickest to build `INTFLASH_BANK=1` when testing retro-go in ways unrelated to the update path or other OFW-adjacent parts of the project, since reset then lands directly in retro-go. All 20 core binaries differ between bank builds, so use `make flash_sd` after changing bank — it flashes intflash and `sdpush`es every core in one command.

  Set by hand for most builds: `INTFLASH_BANK`, `EXTFLASH_SIZE_MB`, `EXTFLASH_OFFSET`. Typical feature flags: `COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 CHEAT_CODES=1`, plus any locale flags. See [docs/device-development.md](docs/device-development.md).
- **Each emulator must fit in the per-core RAM budget (~1 MB)**, not just compile. Memory regressions only show up at runtime on hardware.
- **`make help` is authoritative** for build flag names, defaults, and current values — prefer it over reading the Makefile.
- **Never assume a shared `build/` is yours, and `BUILD_DIR` will not save you.** `scripts/run_gwemu.sh` hardcodes `build/qemu_bank*.bin`, `build/extflash.bin`, `build/sdcard.img` and gdb port `:1234`, so two concurrent runs in one checkout corrupt each other — and the symptom is *random emulator crashes*, not an obvious resource conflict. `--reset` deletes those images while another instance may be reading them. **`BUILD_DIR` does not isolate a link**: the linker scripts hardcode `build/<core>/*.o` (96 sites in `STM32H7B0VBTx_SDCARD.ld`, 65 in `_FLASH.ld`) and those globs resolve against the linker's cwd, so an overridden `BUILD_DIR` links both trees and dies on multiple definitions. Correct approach when sharing a checkout: **serialise builds**, then **snapshot the artifacts you intend to test into your own directory** and launch the emulator against the copies on your own QMP/gdb ports. Never `rm -f build/...`. Full recipe in [docs/gwemu.md](docs/gwemu.md) under "Running more than one instance at a time".
- **Don't pass an over-aligned struct-member pointer straight into `memmove`/`memcpy`.** `arm-none-eabi-gcc 15.2` mis-marshalled the arguments of a `memmove` whose dest/src came directly from an `ABI_PTR_ALIGN` (`aligned(8)`) member inside a hot/cold–split (`.part.0`) function — it shifted the args one register over so a *pointer* landed in the size slot, producing a multi-hundred-MB copy that ran off into peripheral space and took an imprecise bus fault (EarthBound `scroll_window_up`). Materialize the pointer/size into plain locals (`uint16_t *dst = w->content_tilemap; ... memmove(dst, src, nbytes);`) and, if you suspect a codegen bug, **verify the arg registers in the disassembly**.

- **`fopen()` handles are a small, shared, system-wide budget, and running out fails *silently*.** `MAX_OPEN_FILES` (`Core/Src/syscalls.c:51`) is **8** — it was 3 until the DOS core, which holds three of them for its entire run (hard disk, floppy, BIOS blob). Once the table is full every other `fopen()` in the firmware returns `NULL`, and the code that calls it usually treats that as "asset missing" and degrades gracefully: `rg_i18n.c:245` falls back to `unknown_glyph_entry`, so the entire UI rendered as diamonds with nothing logged and nothing faulting. If a core wants long-lived handles, count them against 8 first.

- **Never make a gdb inferior call on this target.** Calling a function on the device
  (`call foo(...)`) while a breakpoint is live re-enters, gdb abandons the call, and the
  target is left **halted with a corrupted heap/stack frame** — indistinguishable from a
  hung device. Recovery: kill the stray `gnwmanager gdbserver`, then
  `gnwmanager start bank1`. `gnwmanager monitor` reads the device log with no
  breakpoints and no inferior calls; use it.
- **The device log ring is 4 KB and wraps to index 0**, not a true ring
  (`Core/Src/main.c:94`, `Core/Src/syscalls.c:142`). Any per-second log line overwrites
  it within seconds, so **output printed once at startup must be captured by attaching
  `gnwmanager monitor` before the core launches.** Connecting afterwards and reading
  what is left silently loses it.
- **`flash_intflash` does not update a core on the SD card.** Cores are streamed from
  `/cores/<system>.bin`; after changing core code run `make create_sd_data` and
  `gnwmanager sdpush`. The symptom of skipping it is `CORE: load failed`, or worse, new
  firmware silently running the previous core.
- **`<CORE>_CFLAGS_EXTRA` is not a make dependency.** Changing it recompiles nothing.
  `touch` the sources first, or you will build and flash the previous flags.

## Debugging crashes on hardware (BSOD / faults)

- **Faults self-label.** `main()` sets `SCB->SHCSR` BUSFAULTENA/USGFAULTENA/MEMFAULTENA, so the BSOD title reads "Busfault" / "Usagefault" / "Memfault" instead of a generic "Hardfault". The BSOD also prints `CFSR/HFSR/BFAR/MMFAR/ABFSR`.
- **Imprecise BusFault (`CFSR` bit10 IMPRECISERR, `CFSR=0x…400`, BFAR invalid) = a buffered store to a no-slave address; the reported PC is drain-time noise, not the culprit.** Read **`ABFSR` (`0xE000EFA8`, on the BSOD)** — it names the bus interface the wild access used: bit2 **AHBP** = peripheral space `0x40000000–0x5FFFFFFF`, bit3 **AXIM** = all RAM/flash, bit0/1 = ITCM/DTCM. This instantly tells you RAM-corruption vs a wild pointer into peripherals.
- **GDB over an ST-Link (or pico-probe).** `gnwmanager gdbserver` spawns OpenOCD (auto-detects the probe via `interface/*.cfg`) with a gdbserver on `:3333`; then `make gdb` (or `arm-none-eabi-gdb build/gw_retro_go.elf -ex 'target extended-remote :3333'`). Use **`hbreak`, not `break`, for flash addresses** (`0x08xxxxxx`) — a software breakpoint silently fails to write flash. RAM/overlay addresses (`0x24xxxxxx`) take either. This gdb build has **no Python**; use native `-ex printf`/`x`. To catch a fault with full context, `hbreak common_fault_handler_c` and read `*frame` (the stacked r0–r3/lr/PC) plus live r4–r11.
- **Overlay RAM addresses alias.** Every core's overlay links at the same RAM_EMU VMA, so `gdb`/`addr2line` resolve a `0x24xxxxxx` address to *whichever* overlay's symbol it finds first (often zelda3/SMW, not the running core). Resolve EB addresses via `build/gw_retro_go.map` filtered to `build/earthbound/*.o`, or disassemble the specific `build/<core>/<file>.o`.

## Emulator-specific notes

Detailed debugging guides live next to each porting layer (not in this file — keeps context lean when working on other cores). Cursor loads matching rules from `.cursor/rules/` when you edit files in those trees.

| System | Guide |
|--------|-------|
| PCE / PCE CD | [Core/Src/porting/pce/CLAUDE.md](Core/Src/porting/pce/CLAUDE.md) — harness `linux/Makefile.pce` |
| MS-DOS (8086tiny) | [Core/Src/porting/dos/CLAUDE.md](Core/Src/porting/dos/CLAUDE.md) — **read its "Where the documentation lives" section first.** Design docs are in the submodule: `external/8086tiny/STATUS.md` (status/budget), `docs/decisions.md` (settled calls + why), **`docs/traps.md` (things that have already cost real time — read before changing anything)**, `docs/<category>-roadmap.md` → `docs/<category>/`. `GNW_PORT.md` is a superseded pointer; `docs/sprint-first-boot.md` is a completed record, not an active plan. |

Add a `CLAUDE.md` under `Core/Src/porting/<system>/` (and optionally `.cursor/rules/<system>.mdc`) when an emulator accumulates non-obvious debug knowledge.

## Working practices that have paid for themselves

These are not style preferences. Each one has a specific failure behind it.

- **Merge, then TEST/VERIFY the merged tree, then delete the branch.** Not
  merge-and-delete. A merge is itself a change that can be wrong, and the branch is the
  only cheap way back. `fix/ega-planar-window` had its submodule half merged and its
  parent half left behind, and was reported as landed. Later, a submodule test failed on
  the merged tree purely because the parent half of the same change had not been merged
  yet — the verify caught it, a delete would have hidden it.
- **A parent change and its submodule change are one atomic change.** Both land or
  neither is done.
- **During a multi-agent run, branches stay alive; clean up as a batch at the end.**
  Accounting for what happened beats tidiness mid-flight. Agent worktrees pin their
  branches, so tear the worktrees down first or `git branch -d` fails.
- **A hot-path claim is a disassembly or it is nothing.** Shape arguments have lost
  repeatedly here. A `REJECT ON SIGHT` rule against a page-table fold survived for
  months on an argument that turned out to be based on sampling one call site; 37 of 78
  sites had the cheap arm out of line. When it was finally built and disassembled it was
  three instructions with no branch.
- **Measure before building, when the measurement is cheaper than the build.** The
  per-segment fold cache was killed by a 30-line script measuring the hit rate it would
  have had: 0.0%.
- **A test that cannot fail is not a test.** Mutation-test the check itself, and keep a
  negative control (`-DDOS_SEAM_FIX=0`, `-DDOS_FETCH_FIX=0`, `negctl.sh`). Several
  correctness "proofs" here passed while silently absorbing the very error they existed
  to catch, because the backing store was `malloc`'d rather than `mprotect(PROT_READ)`.
- **Do not repeat an agent's number without checking what it means.** `WOLF3D 0/144
  dirty pages` was reported as 589,824 bytes reclaimed; it meant the title never
  launched. A run with no input injection produces lower bounds, not results.
- **gwemu cannot produce timing numbers.** Measured: 0.58 cyc/line where hardware
  measured 223.82 — off by ~385x — and it inverts the cpu/blit ratio ~100x. It is for
  plumbing and correctness. Prove a procedure under gwemu *before* running it on
  hardware; the device can be damaged and gwemu cannot.
