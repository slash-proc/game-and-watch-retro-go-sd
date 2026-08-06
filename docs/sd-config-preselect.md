# Booting straight into a game: fabricating `/CONFIG` on the SD card

This is firmware-wide, not DOS-specific — `/CONFIG` is retro-go's own settings
file and the boot path lives in `Core/Src/retro-go/rg_main.c` — so it lives in
the parent `docs/`, not in `external/8086tiny/docs/`. The DOS core is only the
first consumer.

**Verified 2026-08-06** against `dos-integration` @ `95dfab6f`, under gwemu
0.0.20 headless.

## Summary

Writing a 524-byte `/CONFIG` at the root of the SD card, with `startup_file`
set to a ROM's on-card path, makes the firmware boot **directly into that
game with no input at all**. Proven; log line quoted below. Generator:
`tools/gen_retrogo_config.py`.

## 1. Where the config lives

A single file, `/CONFIG`, in the **root of the SD card** (not external flash,
not internal flash).

- Read: `odroid_settings_init()` — `Core/Src/porting/odroid_settings.c:163`.
  It `fopen("/CONFIG","rb")`s, reads exactly `sizeof(persistent_config_t)`
  bytes into `persistent_config_ram`, and rejects anything shorter.
- Write: `odroid_settings_commit()` — same file, line 207. `fopen("/CONFIG","wb")`.
  The handle is closed immediately, so it costs nothing against the 8-entry
  `MAX_OPEN_FILES` budget at runtime.
- On any rejection (`odroid_settings_reset()`, line 248) the defaults are
  copied in and **immediately committed**, i.e. a bad `/CONFIG` is overwritten
  with a good default one on the spot.

`odroid_settings_init()` is called twice per boot: once from the pre-mount
`odroid_system_init()` (which is why every boot log starts with
`Config: Magic mismatch ... got 0x00000000` — the filesystem is not mounted
yet, so nothing is read), and again after `sdcard_init()`. The second call is
the one that matters.

## 2. Binary layout

`struct persistent_config` (`odroid_settings.c:58`), little-endian, natural
ARM EABI alignment, **no packing**. Offsets below are from the DWARF of the
actual build, not from reading the source:

```
arm-none-eabi-gdb -batch build/gw_retro_go.elf \
  -ex 'ptype /o struct persistent_config'
```

| off | size | field | notes |
|----:|-----:|-------|-------|
| 0 | 4 | `uint32_t magic` | must be `0xcafef00d` |
| 4 | 1 | `uint8_t version` | must equal the build's default, currently **8** |
| 5 | 1 | `backlight` | default 6 |
| 6 | 1 | `start_action` | **dead** — set nowhere, `_get` called nowhere |
| 7 | 1 | `volume` | default 4 |
| 8 | 1 | `font_size` | default 8 |
| 9 | 1 | `theme` | default 2 |
| 10 | 1 | `colors` | |
| 11 | 1 | `turbo_buttons` | |
| 12 | 1 | `font` | |
| 13 | 1 | `lang` | index into the built-in language list |
| 14 | 1 | `startup_app` | **dead at boot** — nothing reads it in the boot path |
| 15 | 1 | `cpu_oc_level` | 0..2, applied via `SystemClock_Config()` |
| **16** | **256** | **`char startup_file[256]`** | **the field that selects the game** |
| 272 | 2 | `uint16_t main_menu_timeout_s` | default 600 |
| 274 | 2 | `uint16_t main_menu_selected_tab` | launcher UI only |
| 276 | 2 | `uint16_t main_menu_cursor` | launcher UI only |
| 278 | 96 | `char main_menu_browse_subpath[96]` | launcher UI only |
| 374 | 1 | `bool debug_clock_always_on` | +1 byte hole |
| 376 | 4 | `uint32_t welcome_prompt` | 0 = unanchored, 1 = already shown, else `YYYYMMDD` |
| 380 | 6·N | `app_config_t app[APPID_COUNT]` | 6 bytes each; +2 pad |
| **520** | 4 | `uint32_t crc32` | |
| | **524** | total | **with `APPID_COUNT == 23`** |

`app_config_t` = 6 × `uint8_t` (region, palette, disp_scaling, disp_filter,
disp_overscan, sprite_limit).

**Checksum.** `crc32_le(0, whole struct, sizeof)` with the `crc32` field zeroed
first. `Core/Src/porting/crc32.c` is the standard reflected CRC-32 (poly
`0xedb88320`), and `crc32_le(0, …)` is **bit-identical to Python's
`zlib.crc32()`** — verified by recomputing the CRC of a device-written
`/CONFIG` and matching `0xe260e96e` exactly.

**Validation order and failure behaviour** (all three fall back to
`odroid_settings_reset()`, which rewrites `/CONFIG` with defaults):

1. short read → zeroed → magic mismatch, logs `Config: Magic mismatch. …`
2. `version != 8` → logs `Config: New config version, resetting settings.`
3. CRC mismatch → logs `Config: CRC32 mismatch. Expected 0x… got 0x…`

Version is a plain equality check, so **any future field change bumps the
version and invalidates every fabricated config** — loudly, in the log.

## 3. What actually selects the game

`app_main()` — `Core/Src/retro-go/rg_main.c:1062-1088`:

```c
const bool force_launcher = ((GW_GetBootButtons() & B_TIME) != 0);
char *startup_file = odroid_settings_StartupFile_get();   // &config.startup_file
if (!force_launcher && strlen(startup_file) > 0)
    file = emulator_get_file(startup_file);
...
if (file != NULL) emulator_start(file, true, true, -1);   // else retro_loop()
```

So exactly **one** field matters: `startup_file`. Everything else
(`main_menu_selected_tab`, `main_menu_cursor`, `browse_subpath`) only
positions the launcher's UI and is irrelevant when a game is started.

- Path shape: `emulator_get_file()` (`rg_emulators.c:1754`) only matches paths
  beginning `"/roms/<emu-dirname>/"` — `RG_BASE_PATH_ROMS` is `/roms`
  (`RG_STORAGE_ROOT` is empty on SD builds). DOS's dirname is `dos`
  (`rg_emulators.c:1843`), so: `/roms/dos/TOPBENCH.dsk`.
- It then rescans that one directory and string-compares full paths. **It is
  matched by name, not by index** — adding or removing ROMs cannot make it
  select a different title.
- Holding **TIME** at boot bypasses the whole thing and forces the launcher.
  That is the manual escape hatch.
- `boot_mode` (`BOOT_MODE_COLD/WARM/HOT`) comes from the RTC backup register /
  reset cause, not from the config, and does **not** need to be forced — the
  resume path above runs in every boot mode.
- `SHARED_HIBERNATE_SAVESTATE` / savestates: `emulator_start(file, load_state=true, …)`
  means the core will load `/saves/…` if one exists. Delete the savestate if
  you want a clean start; for DOS also mind `/saves/dos/<NAME>.fbs`
  (fast-boot snapshot) and `.xipimg`.
- `emulator_start(…, start_paused=true, …)` sets `pause_after_frames`. In
  practice this did **not** stall the DOS core — the measured run below ran
  free for 120 s producing per-second `DOS: prof …` lines with no input.

## 4. Verified recipe

```bash
# 1. snapshot the images you intend to test into your OWN directory
#    (never run gwemu against a shared build/)
cp build/{qemu_bank1.bin,qemu_bank2.bin,extflash.bin,sdcard.img,gw_retro_go.elf} mydir/

# 2. fabricate the config, validating the ROM is really on the card
tools/gen_retrogo_config.py --sd-image mydir/sdcard.img \
    --rom /roms/dos/TOPBENCH.dsk --install

# 3. boot. no input, no timeline.
scripts/run_gwemu.sh --log-file run.log        # or your own port-isolated runner
```

Positive indicator from `run.log` — **no input was injected at any point**:

```
Config: Magic mismatch. Expected 0xcafef00d, got 0x00000000     <- pre-mount, always
filesytem mounted.
LCD init done.
odroid_system_init: System ready!

Retro-Go: Starting game: TOPBENCH
Initializing 8086tiny...
DOS: guest RAM 192 KB at 0x2404a534 …
DOS: prof 286 12MHz @340MHz ipf=21000 ips=1216289 cpi=154 | cpu=67% …
```

Note the *absence* of a second `Config:` line after `filesytem mounted.` —
that is the fabricated config being accepted.

## 5. Negative controls (both personally observed to fail)

**(a) Deliberately corrupted CRC**, same valid `startup_file`
(`--bad-crc`). The config is rejected and the launcher comes up:

```
filesytem mounted.
Config: CRC32 mismatch. Expected 0x7b48a3ce, got 0x84b75c31
…
Retro-Go: Initializing emulator 'Nintendo Gameboy'
```
`grep -c "Starting game" → 0`.

**(b) Structurally valid config naming a ROM that is not on the card**
(`--rom /roms/dos/NOSUCH.dsk --force`). This is the dangerous one:

```
filesytem mounted.
Retro-Go: Initializing emulator 'Nintendo Gameboy'
```
No `Config:` complaint at all — the config was accepted — and no
`Starting game`. **The firmware fails silently to the launcher.** That is why
`gen_retrogo_config.py` refuses to write a config for a ROM it cannot find on
the target image, and `--force` exists only to build this control.

## 6. What makes a fabricated config go stale

| Change | Effect | Loud or silent? |
|---|---|---|
| any field added/changed in `persistent_config_t` (version bumped past 8) | rejected, launcher | **loud** — `Config: New config version` |
| `APPID_COUNT` changes (new emulator) | struct size changes → short read → magic mismatch | **loud** — `Config: Magic mismatch` |
| the named ROM is renamed/deleted | launcher | **silent** — mitigated by the generator's presence check |
| other ROMs added/removed/reordered | none — matching is by path string | n/a |
| the game is exited to the launcher, or the device sleeps from the launcher | `odroid_system_switch_app(0)` / the sleep path call `StartupFile_set(NULL)` + commit, clearing the field | n/a, but it means **a fabricated config is one-shot** — reinstall it before every run |
| a failed validation | `odroid_settings_reset()` rewrites `/CONFIG` with defaults | so re-install after any rejected run too |

The struct-size dependency is real: this tree's `APPID_COUNT` is 23 (up from
21 before GBA and DOS were added), giving 524 bytes. `gen_retrogo_config.py`
hardcodes 524; if the count changes, re-derive it with the `ptype /o` command
above.

## 7. The alternative: `--timeline`

`scripts/run_gwemu.sh --timeline <file.tl>` replays a recorded input timeline
(`--record` to capture one). `timelines/dos-item2.tl` in full:

```
# Enter the DOS list, move down one, launch item 2.
11.0  press a
12.5  press down
13.5  press a
45.0  quit
```

That is **absolute wall-clock seconds** driving a **cursor position**. It is
therefore doubly fragile:

- *Timing:* the `11.0` assumes the launcher is up and the DOS tab is showing
  by then. Boot time varies with SD-image size, ROM count, cover art and host
  load. Under-shoot and the presses land on the splash screen; overshoot and
  the 10-minute idle timeout is irrelevant but the presses still land somewhere
  unintended.
- *List order:* "move down one, launch item 2" selects **by index**. Add a ROM
  that sorts earlier in `/roms/dos/` and this timeline silently launches a
  different game — and the log will happily print `Starting game: <wrong one>`,
  so an automated profiling run would produce plausible numbers for the wrong
  title.

It also cannot be used on hardware at all: timelines are a gwemu feature.

## 8. Recommendation

**Use the fabricated config.** It is more robust on every axis that matters
here:

| | fabricated `/CONFIG` | `--timeline` |
|---|---|---|
| selects by | **ROM path (name)** | cursor index + wall clock |
| ROM added to the folder | unaffected | **silently launches the wrong title** |
| slow/fast boot | unaffected | breaks |
| works on hardware | **yes** — same file, `gnwmanager sdpush CONFIG :/CONFIG` | no, gwemu only |
| goes stale when | struct version/size changes — **loudly, in the log** | any list or timing change — silently |
| maintenance | one 200-line script, regenerated per run | one file per scenario, re-record on every UI change |

The one failure mode the config route has — naming a ROM that is not present —
is silent in the firmware, so the generator makes it a hard error at generation
time and that is where it must stay. Do not add a "warn and continue" mode.

Timelines remain the right tool for what they are actually for: driving input
*inside* a game (menus, item selection, a benchmark's own keypresses). The two
compose well — boot into the title with the config, then use a short timeline
whose t=0 is a game that is already running, which removes the boot-latency
variance that makes timelines fragile in the first place.
