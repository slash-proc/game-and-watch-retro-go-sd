# Developing on the device — what bank, what parameters

Orientation for anyone (human or agent) about to build firmware for testing, run
it under gwemu, or flash it. Two questions decide everything else: **what bank**
and **what parameters**.

For depth on gwemu itself — dependencies, the two build variants, multi-instance
rules, timelines and video — see [gwemu.md](gwemu.md). This file is the
thirty-second version plus the reasoning, so the answers are not buried.

---

## TL;DR

```sh
# Build firmware AND the gwemu images in ONE invocation. Never two.
make -j8 CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
     DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=1 CHEAT_CODES=1 \
     ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 \
     release gwemu_release gwemu_roms

# Flash the device. flash_sd, NOT flash_intflash.
make -j8 <same variables> flash_sd
```

If `build/sdcard.img` already exists, `gwemu_release` **skips image prep** and you
will test yesterday's SD card with today's firmware. Move the four images aside
first — see [gwemu SD-card behaviour](#gwemu-sd-card-behaviour).

---

## What bank?

**The device always boots at `0x08000000`.** That is hardware, not configuration.

The usual end-user arrangement is stock OFW in bank 1, patched so that
**Game + Left** jumps to `0x08100000` where retro-go lives in bank 2. Sometimes a
bootloader also sits in bank 1, to help update retro-go and to diagnose SD-card
problems.

**For development we build `INTFLASH_BANK=1` to cut out the middle-man.**
retro-go links to run at `0x08000000`, so reset lands directly in it: no OFW
patch, no key combo, no bootloader. Simplicity and speed while testing.

The cost: we overwrite whatever was in bank 1.

`INTFLASH_BANK` is about **where retro-go is linked to run**, not about matching
some component that happens to be installed.

### Why this makes gwemu comparable to the device

With `INTFLASH_BANK=1`:

| image | contents |
|---|---|
| `build/qemu_bank1.bin` | **our `intflash.bin`** |
| `build/qemu_bank2.bin` | zeros |

So the emulator and the device run the **identical binary**. That is the entire
point of keeping the parameters symmetric — see the next section.

(With `INTFLASH_BANK=2`, `qemu_bank1.bin` is instead taken from
`backup/internal_flash_backup_mario.bin` or `..._zelda.bin`, i.e. the OFW, and
our image goes in bank 2.)

### Cores are bank-dependent — `flash_sd`, never `flash_intflash`

**Measured 2026-08-05: all 20 core binaries differ between a bank-1 and a bank-2
build**, not just the one you are working on. Cores are streamed from
`/cores/<system>.bin` on the SD card, so after a bank change the card's cores no
longer match the flashed firmware.

The symptom is vague rather than a clean error: "the cores aren't correct",
`CORE: load failed`, or — worst — **new firmware silently running the previous
core**.

`make flash_sd` does intflash over USB **plus** `sdpush` of every core, in one
command, with the card still in the device. Use it.

Related trap: **`update_bank2.bin` is named, not verified.** The Makefile gives
that filename to `build/gw_retro_go_intflash.bin` whatever bank it was linked
for, so a bank-1 tree produces a bank-1 image called `update_bank2.bin`. Check
the reset PC (second word of the image); below `0x08100000` means bank 1.

---

## What parameters?

**One invocation, one set of variables.** `Makefile.common` enforces this
structurally:

```make
GWEMU_PREREQS := release
```

`release` is a real prerequisite of `gwemu_release`, so the two cannot be built
with different flags. Build them separately — `make release COVERFLOW=1` today,
`make gwemu_release` tomorrow — and gwemu boots *different firmware* than the
device while looking completely fine. Every comparison between them is then
worthless.

### Checking parity rather than assuming it

`build/gwemu_build_info.txt` is written on every `gwemu_release` and records the
parameters plus md5s of all four images:

```
GNW_TARGET                = mario
SD_CARD                   = 1
INTFLASH_BANK             = 1
...
intflash.bin md5          = 391217b05c0494c21aa40f6ee79db430
qemu_bank1.bin md5 = b413cf503c852f170d8fe88f72a56815
qemu_bank2.bin md5 = ec87a838931d4d5d2e94a04644788a55
extflash.bin md5 = 7f614da9329cd3aebf59b91aadc30bf0
sdcard.img md5 = 497e3c358da0ae2b824c1b5bf46f7841
```

When gwemu and hardware disagree, the first question is always *"was it actually
the same firmware?"* — compare the `intflash.bin md5` against what `gnwmanager`
flashed. That file answers it without re-deriving anything.

### Which variables matter

`make help` is authoritative for names, defaults and current values. The ones
that change **layout** rather than behaviour, and therefore must match between
your gwemu and hardware runs:

`GNW_TARGET`, `INTFLASH_BANK` (or `INTFLASH_ADDRESS`), `SD_CARD`,
`EXTFLASH_SIZE_MB` / `EXTFLASH_OFFSET`, and the locale/codepage flags.

---

## gwemu SD-card behaviour

Three things about the SD image surprise people, in roughly this order.

### 1. Prep is skipped if the image already exists

`gwemu_release` checks for `build/sdcard.img` and, if present, prints:

```
QEMU images already exist (skipping prep). Pass --reset to wipe state.
```

…and keeps **yesterday's firmware, cores and games**. This is the single most
common way to test something that is not what you just built.

Move the four images aside before rebuilding — do **not** `rm -f build/...`,
another instance may be reading them:

```sh
mkdir -p build/_stale
for f in sdcard.img extflash.bin qemu_bank1.bin qemu_bank2.bin; do
    [ -f build/$f ] && mv build/$f build/_stale/$f
done
```

`Core/Src/porting/dos/CLAUDE.md` documents an alternative used during the DOS
work: `scripts/gwemu_refresh.sh`, which rebuilds bank 1 in place, re-`mcopy`s
`sd_content` and `roms/dos`, and **preserves `::/CONFIG`**. Note it is
**hardcoded for bank 1** and will silently produce an unbootable machine on a
bank-2 tree.

### 2. `gwemu_roms` is a separate target

`gwemu_release` **sizes** the image to fit `roms/` (via
`make_sdcard_image.py --fit sd_content --fit roms`) but then copies only
`sd_content/*`. Omit `gwemu_roms` and you get a correctly-sized card with **no
games on it**.

The documented invocation is therefore
`make ... release gwemu_release gwemu_roms`, in that order.

### 3. What media prep actually does

64 MB blank `extflash.bin`; `make_sdcard_image.py` sizes and creates a FAT32
image; `mformat`; `mcopy -s sd_content/* ::/`. Then `gwemu_roms` `mmd`s `::/roms`
and `mcopy`s `roms/*` into it, after a free-space check with 5 % FAT slack.

---

## Unattended launch: boot straight into a game

Fully documented, with a verified recipe and negative controls, in
**[sd-config-preselect.md](sd-config-preselect.md)**. Summary:

retro-go persists its settings as a single file **`/CONFIG` in the SD-card
root** — `struct persistent_config`, little-endian, **524 bytes** at this tree's
`APPID_COUNT == 23`. Magic `0xcafef00d`, version `8`, trailing CRC32 computed
over the whole struct with the CRC field zeroed (bit-identical to
`zlib.crc32()`).

**Exactly one field selects the game:** `startup_file` at offset 16, matched **by
full path string** — `/roms/dos/TOPBENCH.dsk` — not by index. Generator:
`tools/gen_retrogo_config.py`.

Three things to know before relying on it:

- **A config naming a ROM that is not on the card fails SILENTLY.** No complaint,
  no log line, straight to the launcher. That is why the generator hard-errors
  when it cannot find the ROM in the target image.
- **A fabricated config is one-shot.** Exiting a game to the launcher, and the
  launcher's sleep path, both clear `startup_file` and commit. Reinstall before
  each run.
- **`Config: Magic mismatch. Expected 0xcafef00d, got 0x00000000` appears at the
  top of EVERY boot and is noise, not a fault** — it is a pre-mount read, before
  the filesystem is available.

**Prefer this to `--timeline` for automation.** A timeline drives a *cursor
index*; add a ROM that sorts earlier in `/roms/dos/` and it silently launches a
different title while printing a perfectly plausible `Starting game: <wrong
one>`. A config selects by name, is immune to list changes, goes stale loudly,
and the identical file works on hardware via `gnwmanager sdpush CONFIG :/CONFIG`.
The strongest combination is a config to reach the title, then a short timeline
whose t=0 is an already-running game — which removes the boot-latency variance
that makes timelines fragile.

---

## Traps that have each cost real time

- **`DOS_CFLAGS_EXTRA` (and `<CORE>_CFLAGS_EXTRA`) is NOT a make dependency.**
  Changing it recompiles nothing. `touch` the sources first or you build, flash
  and measure the *previous* flags. Two DOS features shipped completely inert
  because of this class of mistake.
- **Never assume a shared `build/` is yours, and `BUILD_DIR` will not save you.**
  `scripts/run_gwemu.sh` hardcodes `build/qemu_bank*.bin`, `build/extflash.bin`,
  `build/sdcard.img` and gdb `:1234`. Two concurrent runs corrupt each other and
  **the symptom is random emulator crashes**, not an obvious resource conflict.
  `BUILD_DIR` does not isolate a *link*: the linker scripts hardcode
  `build/<core>/*.o` globs that resolve against the linker's cwd. Serialise
  builds, snapshot the artifacts you intend to test into your own directory, and
  use your own QMP/gdb ports. Full recipe in
  [gwemu.md](gwemu.md#running-more-than-one-instance-at-a-time).
- **Never `rm -f build/...`** — `--reset` deletes images another instance may be
  reading.
- **The device log ring is 4 KB and wraps to index 0** (`Core/Src/main.c:94`), so
  anything printed once at startup is lost unless `gnwmanager monitor` is
  attached *before* the core launches. Under gwemu use `--log-file`.
- **gwemu cannot produce timing numbers.** Measured: 0.58 cycles/line where
  hardware measured 223.82 — off by ~385× — and it inverts the cpu/blit ratio by
  ~100×. gwemu is for plumbing and correctness. **Prove a procedure under gwemu
  before running it on hardware**; the device can be damaged and gwemu cannot.

---

## Where to go deeper

| topic | document |
|---|---|
| gwemu in full — deps, variants, multi-instance, timelines, video | [gwemu.md](gwemu.md) |
| Booting straight into a game | [sd-config-preselect.md](sd-config-preselect.md) |
| Profiling workflow | [perf-investigation-workflow.md](perf-investigation-workflow.md) |
| MS-DOS core: rules, traps, docs map | [../Core/Src/porting/dos/CLAUDE.md](../Core/Src/porting/dos/CLAUDE.md) |
| MS-DOS design docs, glossary, traps | `external/8086tiny/docs/` |
