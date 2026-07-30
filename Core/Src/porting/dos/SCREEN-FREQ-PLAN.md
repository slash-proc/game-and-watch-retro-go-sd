# "Screen Freq Hz" for the MS-DOS core — design + implementation plan

Status: **design only, nothing implemented.** Read-only investigation; no source file
was modified. Every claim below cites `file:line` as of branch `dos-integration`.

Scope: add a `Screen Freq Hz` row to the MS-DOS options menu offering
`25 (50)`, `30 (60)`, `50`, `60`, `72`, `75`, reinitialise the display clock when it
changes, and keep emulated guest speed and guest wall-clock time invariant across
the change.

---

## 0. Executive summary

* Refresh switching is **already a solved, one-call operation** in this tree:
  `lcd_set_refresh_rate()` (`Core/Src/gw_lcd.c:548-591`) reprograms **PLL3 only**.
  It does **not** touch LTDC timings, the framebuffers, or the MPU. gwenesis uses it
  exactly that way (`Core/Src/porting/gwenesis/main_gwenesis.c:174`). DOS already
  calls it, hardcoded to 60 (`Core/Src/porting/dos/main_dos.c:156`).
* The panel-reachable set is **exactly {50, 60, 72, 75}** — the four branches of
  `gw_lcd.c:549-567`. There is no 25 or 30 Hz LTDC clock, so `25 (50)` / `30 (60)`
  must be **half-rate guest frame generation on a 50/60 Hz panel**.
* The real frame pacer in this codebase is **not** vsync — it is the SAI/DMA
  half-buffer counter (`common_emu_sound_sync()`, `Core/Src/porting/common.c:495-511`,
  waiting on `dma_counter`). Frame rate is therefore set by
  `audio_start_playing(AUDIO_SAMPLE_RATE / hz)` (`main_dos.c:158`), and that number
  is **bounded by `AUDIO_BUFFER_LENGTH == 1077`** (`Core/Inc/gw_audio.h:15`).
  `48000/25 = 1920 > 1077` — a naive 25 Hz would overrun the DMA buffer. This is the
  single most important constraint in the whole feature.
* Guest CPU identity must be preserved: the profile table stores **instructions per
  frame** (`dos_cpu.c:61-66`), so it must become instructions per **second** with
  `ipf = ips / hz` derived at runtime.

> **DECIDED:** the profile table changes to instructions-per-second and `ipf` is
> derived. Rationale: `ipf` alone silently redefines "286 12MHz" whenever the user
> changes the refresh rate. That is not a preference, it is the emulated machine
> changing identity behind the user's back. Section 2 gives the exact numbers.

---

## 1. How gwenesis switches refresh (the reference pattern)

Call site, in `gwenesis_system_init()`:

* `Core/Src/porting/gwenesis/main_gwenesis.c:162-168` picks
  `gwenesis_refresh_rate` = PAL or NTSC alongside the audio rate and audio buffer
  length — the three are chosen **together**, in one place.
* `Core/Src/porting/gwenesis/main_gwenesis.c:173` `odroid_audio_init(gwenesis_audio_freq)`
* `Core/Src/porting/gwenesis/main_gwenesis.c:174` `lcd_set_refresh_rate(gwenesis_refresh_rate)`
* `Core/Src/porting/gwenesis/main_gwenesis.c:180` (`gwenesis_sound_start`)
  `audio_start_playing(gwenesis_audio_buffer_lenght)` — the audio buffer length is
  what actually paces the loop.

What `lcd_set_refresh_rate()` reconfigures (`Core/Src/gw_lcd.c:548-591`):

* It selects an `(PLL3N, PLL3R)` pair per rate and calls
  `HAL_RCCEx_PeriphCLKConfig()` with `RCC_PERIPHCLK_LTDC` only
  (`gw_lcd.c:571-590`). **PLL3 is the only thing reprogrammed.**
* `PLL3M = 4`, `PLL3P = 2`, `PLL3Q = 2`, `PLL3RGE = RCC_PLL3VCIRANGE_3`,
  `PLL3VCOSEL = RCC_PLL3VCOWIDE`, `PLL3FRACN = 0` are identical to the boot-time
  configuration in `Core/Src/main.c:536-543`; only N and R differ.
* It records `last_frequency` (`gw_lcd.c:569`), readable via
  `lcd_get_last_refresh_rate()` (`gw_lcd.c:593`). `Core/Src/gw_sleep.c:46-48`
  re-applies it on wake, so **the setting survives sleep/wake for free**.
* LTDC timings are **not** touched: `AccumulatedVBP = 7`, `TotalWidth = 392`,
  `TotalHeigh = 255` are fixed at `Core/Src/main.c:764-768`. The refresh rate is
  changed purely by moving the pixel clock.

Reachability / arithmetic. With the fixed frame of 392 x 255 = 99,960 pixel clocks
per frame, 60 Hz needs ~6.00 MHz pixel clock, which is what `N=9, R=24` produces at
boot (`main.c:537-540`). The other three are exact ratios of that:

| Hz | PLL3N | PLL3R | N/R | ratio vs 60 Hz | file:line |
|----|-------|-------|-----|----------------|-----------|
| 50 | 10 | 32 | 0.31250 | 0.8333 = 50/60 | `gw_lcd.c:553-556` |
| 60 | 9  | 24 | 0.37500 | 1.0000         | `gw_lcd.c:549-552` |
| 72 | 9  | 20 | 0.45000 | 1.2000 = 72/60 | `gw_lcd.c:557-560` |
| 75 | 15 | 32 | 0.46875 | 1.2500 = 75/60 | `gw_lcd.c:561-564` |

All four are exact — no fractional-N, no drift. Any other value falls into the
`else { return; }` at `gw_lcd.c:565-568`, which **silently does nothing**: a request
for 25 or 30 leaves the panel at whatever it was.

> **DECIDED:** `25 (50)` means *panel at 50 Hz, guest frame generated every other
> panel refresh (25 guest fps)*; `30 (60)` means *panel at 60 Hz, guest frame every
> other refresh (30 guest fps)*. `50/60/72/75` are full rate: one guest frame per
> panel refresh. This is the only interpretation the hardware supports — there is no
> 25/30 Hz LTDC clock, and driving the panel that slowly would flicker badly even if
> there were.

> **OPEN:** are 72 and 75 Hz actually *pleasant* on this panel? They are electrically
> reachable (upstream shipped the branches), but nothing in this tree currently uses
> them — `grep` for `lcd_set_refresh_rate` finds only 50, 60, and per-core constants
> (`main_wsv.c:532`, `main_nes.c:529/534`, `main_tama.c:405`, `main_pkmini.c:422`,
> `main_amstrad.c:1085`, `main_msx.c:2125`, `main_gba.c:748`,
> `main_nes_fceu.c:942/946`, `main_dos.c:156`). **A human must look at the screen at
> 72 and 75** before those entries ship; if the panel smears or the backlight beats,
> drop them.

---

## 2. The `ipf` vs `ips` decision (settle this first)

Today (`Core/Src/porting/dos/dos_cpu.c:60-73`):

```
{ "XT 4.77MHz",  6700 },
{ "Turbo 8MHz", 11000 },
{ "286 12MHz",  21000 },
{ "MAX",            0 },
```

The header comment at `dos_cpu.c:36-37` already states the units explicitly:
*"insn_per_frame is at 60 fps, so instructions/second = insn_per_frame * 60"*. The
table is therefore already an instructions-per-second table with a hidden `/60`.
Making `fps` user-selectable while leaving the field as ipf turns "286 12MHz" into a
1.05 MIPS machine at 50 Hz and a 1.575 MIPS machine at 75 Hz.

**Recommended change:** store `insn_per_sec`, derive
`ipf = insn_per_sec / guest_fps` once whenever the rate changes (not per frame),
cache it in a static, and use the cached value in `dos_cpu_run_frame()`.

`ips` values that preserve today's behaviour exactly at 60 Hz:

| profile | ips (= ipf x 60) | ipf@25 | @30 | @50 | @60 | @72 | @75 |
|---------|------------------|--------|-----|-----|-----|-----|-----|
| XT 4.77MHz  |   402,000 | 16,080 | 13,400 |  8,040 |  6,700 |  5,583 |  5,360 |
| Turbo 8MHz  |   660,000 | 26,400 | 22,000 | 13,200 | 11,000 |  9,166 |  8,800 |
| 286 12MHz   | 1,260,000 | 50,400 | 42,000 | 25,200 | 21,000 | 17,500 | 16,800 |
| MAX         |         0 | deadline-driven — see below |

**Rounding drift.** Only two cells are inexact: `XT@72` (5583.33 -> 5583, error
0.006%) and `Turbo@72` (9166.67 -> 9166, error 0.007%). Worst case **0.007%**, i.e.
~28 instructions/second out of 660,000. Irrelevant next to the fact that 8086tiny
counts instructions and not cycles (`dos_cpu.c:12-19`). No compensating accumulator
is needed.

**Other consumers of `ipf`** that must follow the derived value, not the table:

* The frame budget itself, `dos_cpu.c:158` (`budget = ...insn_per_frame`) and the
  `dos_cpu_frame((int)budget)` call at `dos_cpu.c:162`.
* The MAX deadline, `dos_cpu.c:178`: `SystemCoreClock / 60u` is a **hardcoded 60**
  and must become `SystemCoreClock / guest_fps`. Note another agent is actively
  reworking `dos_cpu.c:163-186` (`DOS_MAX_FRAME_PCT`, the chunk loop) — **reference
  the mechanism, not the constants**: whatever that loop's final shape, the frame
  period it divides by must come from the selected rate.
* The reporting path is already rate-agnostic: `insn_per_sec` is derived from
  `HAL_GetTick()` deltas (`dos_cpu.c:209`), and `cpu_us_frame` divides by
  `prof_frames` (`dos_cpu.c:232`). The only stale item is the comment at
  `dos_cpu.c:227-228` that names "the 16,667 us frame period".

> **DECIDED:** half-rate modes set `guest_fps` to 25/30 for the `ipf` derivation, so
> a `25 (50)` frame executes twice the instructions of a `50` frame. Emulated MHz is
> then identical in every mode — that is the whole point.

> **OPEN:** at `25 (50)` with the 286 profile, one `dos_cpu_frame()` call runs 50,400
> instructions. At today's measured `cpi=204` on a 280 MHz core that is ~36.7 ms
> inside a single call with **no `wdog_refresh()` in it** — fixed-budget mode
> refreshes the watchdog once per frame from `main_dos.c:187` only. WWDG1's window is
> a few hundred ms (`dos_cpu.c:92-95`), so 37 ms is still safe, but the margin has
> halved. Safest resolution: run **all** profiles through the chunked
> deadline/watchdog loop (a fixed profile simply stops at its instruction budget
> instead of at a deadline), which unifies the two paths and removes the hazard
> class entirely. Coordinate with the agent currently editing that loop.

---

## 3. LCD / clock reinit path

**What must happen (full-rate change, e.g. 60 -> 50):**

1. Finish any in-flight swap. `lcd_setup_framebuffers()` ends with
   `lcd_sleep_while_swap_pending()` (`gw_lcd.c:331`) for exactly this reason; the DOS
   loop already guards its blit with `!lcd_is_swap_pending()` (`main_dos.c:275`).
2. `lcd_set_refresh_rate(panel_hz)` (`gw_lcd.c:548`). Reprograms PLL3 via
   `HAL_RCCEx_PeriphCLKConfig`, which internally stops and restarts PLL3. LTDC keeps
   scanning out of the same framebuffer; the pixel clock simply changes rate.
3. `common_emu_state.frame_time_10us = 100000 / guest_fps` — replaces the hardcoded
   `100000 / 60` at `main_dos.c:157`. `int16_t` (`common.c:92`): 25 Hz -> 4000,
   fits; but the speedup multipliers at `common.c:118-140` multiply it by up to 2
   (`SPEEDUP_0_5x`, `common.c:120`) -> 8000, still fits `int16_t`. OK.
4. `audio_start_playing(len)` where `len <= 1077` — see section 5. This restarts the
   SAI DMA and therefore the pacer.
5. `common_emu_frame_loop_reset()` (`common.c:77-88`) so `frame_integrator`,
   `last_sync_time` and the DMA marker do not carry pacing error across the change.
   Without this the first post-change frame looks like a huge stall and the
   integrator clamps into skip mode (`common.c:141-155`).

**What is NOT needed:**

* **No `lcd_init()` / `lcd_deinit()`.** gwenesis does not do it
  (`main_gwenesis.c:174` is the whole switch). `gw_sleep.c:46-49` only calls
  `lcd_init()` because it is coming out of sleep, not because of the rate.
* **No `mpu_set_lcd_pool_uncached_range()`** (`Core/Src/main.c:1348`). That function
  sizes MPU regions 3-6 to the **framebuffer footprint** and is re-called from
  `gw_lcd.c:326` only when the *pixel format* changes (RGB565 300 KB vs LUT8 154 KB,
  `gw_lcd.c:319-322`). A refresh change alters neither the footprint nor the
  addresses. Calling it would be actively harmful here: `main_dos.c:105-109` already
  warns that re-running the framebuffer setup would "re-zero the 150 KB framebuffer
  footprint and rewrite MPU regions 3-6 underneath our own code" — the DOS overlay
  lives in the LUT8 bonus pool.
* **No CLUT reprogramming, no cache maintenance.** Nothing in the memory map moves.

**Hazards:**

* PLL3 restart is not instantaneous; expect one visually imperfect frame. Do the
  switch while the pause menu is still up (the menu repaints afterwards anyway,
  `common.c:207-217`), not mid-gameplay.
* Doing it from inside the options callback means it runs while the overlay is
  drawn. `odroid_overlay_game_menu()` repaints via the core's repaint callback
  (`dos_blit`, passed at `main_dos.c:201`), so a rate change mid-menu is safe as long
  as no swap is pending.

> **DECIDED:** apply the change immediately in the option callback (like every other
> option row in this tree), not deferred to core restart. It is a PLL write plus an
> audio restart; there is nothing to defer.

---

## 4. Options-menu wiring

Existing shape (`Core/Src/porting/dos/main_dos.c:169-176`):

```c
char dos_cpu_speed_value[DOS_CPU_VALUE_LEN];
odroid_dialog_choice_t options[] = {
    ODROID_DIALOG_CHOICE_SEPARATOR,
    {200, "CPU speed", dos_cpu_speed_value, 1, &dos_cpu_speed_update_cb},
    ODROID_DIALOG_CHOICE_LAST};
dos_cpu_speed_update_cb(&options[1], ODROID_DIALOG_INIT, 0);
```

The array is a **stack local** in `app_main_dos()` and is handed to
`common_emu_input_loop(&joystick, options, &dos_blit)` (`main_dos.c:201`) every
frame. The callback contract is the `PREV`/`NEXT`/`INIT`/`ENTER` switch shown at
`dos_cpu.c:257-286`; it must write `option->value` and return
`event == ODROID_DIALOG_ENTER`.

The added row is mechanically identical:

```c
char dos_screen_freq_value[DOS_SCREEN_FREQ_VALUE_LEN];
odroid_dialog_choice_t options[] = {
    ODROID_DIALOG_CHOICE_SEPARATOR,
    {200, "CPU speed",      dos_cpu_speed_value,    1, &dos_cpu_speed_update_cb},
    {201, "Screen Freq Hz", dos_screen_freq_value,  1, &dos_screen_freq_update_cb},
    ODROID_DIALOG_CHOICE_LAST};
dos_cpu_speed_update_cb(&options[1], ODROID_DIALOG_INIT, 0);
dos_screen_freq_update_cb(&options[2], ODROID_DIALOG_INIT, 0);
```

with the rate table living next to the CPU table (new code in `dos_cpu.c`, or a
sibling `dos_video_rate.c`), shaped as
`{ const char *label; uint16_t panel_hz; uint8_t divider; }` —
`{"25 (50)", 50, 2}`, `{"30 (60)", 60, 2}`, `{"50", 50, 1}`, `{"60", 60, 1}`,
`{"72", 72, 1}`, `{"75", 75, 1}` — so `guest_fps = panel_hz / divider`. The divider
makes the two half-rate entries fall out of the same code path rather than being
special cases.

**Persistence.** The API is `odroid_settings_app_int32_get(key, default)` /
`odroid_settings_app_int32_set(key, value)`
(`retro-go-stm32/components/odroid/odroid_settings.h:104-105`) — the same mechanism
`dos_cpu.c:80-85` already documents as the intended home for per-game CPU-speed
persistence. Read the stored index in the init path (before
`lcd_set_refresh_rate()`), write it in the callback's `PREV`/`NEXT` branches.

> **DECIDED:** persist screen frequency as an app-scoped setting keyed
> `"dos_screen_hz"` (a **global DOS** preference, not per-ROM). Rationale: the panel
> rate is a display-comfort/throughput choice about the user's hardware, whereas CPU
> speed is a per-title compatibility choice. Doing per-ROM persistence for both is a
> separate piece of work that `dos_cpu.c:80-85` already scopes out.

> **OPEN:** should the default be `60` (today's hardcoded behaviour, `main_dos.c:156`)
> or `50` (the throughput win the rationale describes)? Recommendation: ship the
> default as **60** so Stage 1 is a provable no-op, and revisit only with measured
> before/after numbers from the harness.

---

## 5. Everything else that assumes 60 Hz

| site | what it assumes | fix |
|------|-----------------|-----|
| `Core/Src/porting/dos/main_dos.c:156` | `lcd_set_refresh_rate(60)` | selected `panel_hz` |
| `Core/Src/porting/dos/main_dos.c:157` | `frame_time_10us = 100000/60` | `100000 / guest_fps` |
| `Core/Src/porting/dos/main_dos.c:158` | `audio_start_playing(AUDIO_SAMPLE_RATE/60)` | `AUDIO_SAMPLE_RATE / panel_hz` (**panel**, not guest — see below) |
| `Core/Src/porting/dos/dos_cpu.c:178` | `SystemCoreClock / 60u` MAX deadline | `SystemCoreClock / guest_fps` |
| `Core/Src/porting/dos/dos_cpu.c:36-37` | comment: "insn_per_frame is at 60 fps" | rewrite for ips |
| `Core/Src/porting/dos/dos_cpu.c:227-228` | comment: "the 16,667 us frame period" | rewrite |
| `Core/Src/porting/dos/main_dos.c:226,257` | `(dbg_frames & 0x3F) == 0` "every 64 frames ~= 1s" | harmless; the window length is measured (`dos_cpu.c:203`), only the comment is wrong. At 25 Hz the window becomes ~2.5 s. |

Not affected:

* `common_emu_frame_loop()` (`common.c:90-159`) is entirely parameterised on
  `common_emu_state.frame_time_10us`; there is no literal 60 in it.
* `get_frame_time()` (`odroid_system.h:227-231`) is not used by the DOS core.

**The guest PIT / int8 55 ms tick is already independent — by construction.**
`8086tiny.c:1440-1472` fires the tick on a **wall-clock deadline**:
`dos_next_int8_ms += DOS_INT8_PERIOD_MS` where `DOS_INT8_PERIOD_MS == 55`
(`8086tiny.c:109`), compared against `dos_host_millis()`. The instruction counter
(`inst_counter % KEYBOARD_TIMER_UPDATE_DELAY`, `8086tiny.c:1440`) is *only a gate on
how often the host clock is sampled*, not the time source — the comment at
`8086tiny.c:1434-1439` says so explicitly. Likewise the guest's reported time is
quantised to whole 55 ms steps at `8086tiny.c:520-529`.

Consequence: **no change is required for the PIT, and none should be made.** The one
thing to watch is the sampling granularity: `KEYBOARD_TIMER_UPDATE_DELAY` is 2048
instructions (`8086tiny.c:94`, referenced from `dos_cpu.c:100-105`). At the XT
profile at 75 Hz, `ipf = 5360`, so the host clock is still sampled ~2.6x per frame —
fine. Nothing gets *worse* at lower fps (larger `ipf` = more samples per frame). The
`dos_int8_due` vs `dos_int8_fired` vs `dos_int8_resync` counters already reported in
the `DOS: prof` line (`dos_cpu.c:236-241`, printed at `main_dos.c:262-263`) are the
acceptance instrument: **`due` must stay at ~18.2/s at every rate**, and `resync`
must not climb.

> **DECIDED:** the PIT stays wall-clock-driven and untouched. Any temptation to
> derive it from the frame count must be rejected — that is precisely the bug that
> `8086tiny.c:1441-1443` was written to avoid.

---

## 6. Audio

The DOS core **produces no samples**: `main_dos.c:287-289` is literally
`if (drawFrame) { /* TODO: Submit audio */ }`. Confirmed — the porting notes are
right.

But it **starts the DMA anyway** (`main_dos.c:158`,
`audio_start_playing(AUDIO_SAMPLE_RATE / 60)`), because the DMA half-buffer
interrupt is the frame pacer: `common_emu_sound_sync(false)` (`main_dos.c:294`) spins
on `dma_counter` (`common.c:495-511`) and is described in-file as *"the only place
this loop deliberately waits"* (`main_dos.c:291-293`). So audio is silent but
**structurally load-bearing** for pacing.

Two consequences:

1. **Buffer-length ceiling.** `audio_start_playing(len)` calls
   `audio_start_playing_full_length(len*2)` (`gw_audio.c:70-72`) into
   `audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2]` with `AUDIO_BUFFER_LENGTH == 1077`
   (`gw_audio.h:15,24`). `48000/50 = 960` OK, `48000/25 = 1920` **overruns by 843
   samples**. Overrunning this buffer is a known hazard class in this project (it
   walks past the DMA buffer into whatever follows in `.audio`).
2. Therefore: **pace at the panel rate, always.** `audio_start_playing(48000 /
   panel_hz)` — 960 at 50 Hz, 800 at 60, 666 at 72, 640 at 75 — and implement the
   half-rate modes by consuming **two** DMA edges per guest frame (call
   `common_emu_sound_sync()` twice, or equivalently set `frame_time_10us` to the
   guest period and let the existing `pause_frames` mechanism at `common.c:155` and
   `common.c:500` do the second wait).

> **DECIDED:** the DMA buffer length is a function of **panel** Hz; the guest frame
> period is a function of **guest** fps. Conflating them either overruns the buffer
> at 25 Hz or gives up the half-rate feature.

> **OPEN:** when DOS eventually gets a PC-speaker/Adlib path, samples-per-frame
> becomes `48000 / guest_fps` and the half-rate modes must emit two buffers' worth
> per guest frame. Note it in `docs/decisions.md` now so it is not rediscovered later.

---

## 7. Risks / what could regress

1. **Buffer overrun at 25 Hz** if pacing and buffer length are conflated (section 6).
   Highest-severity item; it is memory corruption, not a glitch.
2. **Tearing / beat frequency.** Guest frames are paced by the audio DMA, not by
   LTDC vsync, so guest frame rate and panel refresh are only *nominally* equal. At
   72/75 Hz the DMA period (666/640 samples) and the panel period drift relative to
   each other exactly as they do today at 60; nothing new, but at higher rates the
   per-frame budget is smaller and `lcd_is_swap_pending()` (`main_dos.c:275`) will
   veto more blits. Expect the blit count in the prof line to drop before the frame
   count does — the same failure signature `dos_cpu.c:67-73` documents for over-tuned
   `ipf`.
3. **Watchdog margin** at 25/30 Hz with a fixed profile (section 2 OPEN). 50,400
   instructions in one un-refreshed call.
4. **Throughput does not automatically improve.** The rationale (blit 520 us x 60 =
   31.2 ms/s vs x50 = 26 ms/s) is real, but at 25/30 Hz the *blit still runs at the
   guest rate*, so 25 Hz halves blit cost again — while the per-frame CPU burst
   doubles and pushes against `DOS_MAX_FRAME_PCT`. Expect the win to be sublinear.
5. **Savestates.** If the setting is persisted in app settings (section 4) rather
   than in the savestate, savestate format is unchanged and compatibility is a
   non-issue. **Do not put it in the savestate.**
6. **Interaction with the launcher's speedup modes** (`common.c:118-140`): they scale
   `frame_time_10us` without touching `ipf`, so they already change emulated CPU
   speed. Out of scope, but note it so the two mechanisms are not confused when
   debugging.
7. **`gw_sleep.c:46-48` re-applies `lcd_get_last_refresh_rate()` on wake** — a free
   win, but it means a DOS-set rate persists into whatever runs next in the same boot
   unless that core sets its own. Every other core does call `lcd_set_refresh_rate()`
   in its init, so this is contained.

---

## 8. Staged implementation plan

Each stage is independently buildable, flashable, and testable. Build line per
project CLAUDE.md, with **`INTFLASH_BANK=1`** (this device boots bank 1).
Harness: `python3 scripts/gwharness.py replay timelines/dos-boot.tl`, reading the
`DOS: prof` line. Watch **`blits=` and `frames=N/Mms`** — `frames=` stays healthy
through drops (`dos_cpu.c:71-73`).

**Stage 1 — de-hardcode 60, change nothing observable.**
Convert the profile table to `insn_per_sec`; add a single `dos_video_hz`/`guest_fps`
pair of statics initialised to 60/60; route `main_dos.c:156-158` and
`dos_cpu.c:178` through them. No menu row yet.
*Acceptance:* prof line is statistically identical to the baseline —
`ipf≈21000`, `ips≈1.26M`, `blits=64`, `frames=64/1067ms`, `int8 due≈18/s`. Any
deviation means the ips->ipf derivation is wrong.

**Stage 2 — menu row, full-rate entries only (50/60/72/75).**
Add `Screen Freq Hz` with four entries; the callback sets panel Hz, recomputes
`ipf`, calls `lcd_set_refresh_rate()`, `audio_start_playing(48000/hz)`, and
`common_emu_frame_loop_reset()`. Not yet persisted.
*Acceptance:* at each rate, `frames=` tracks the rate (≈50, 60, 72, 75 per second),
`ips` stays ≈1.26M for the 286 profile at **every** rate (this is the whole point),
`int8 due` stays ≈18/s, `blits==frames`. **Plus a human looking at the panel at 72
and 75** — a renderer regression once measured 118 us vs 120 us and was invisible to
the profiler.

**Stage 3 — half-rate entries `25 (50)` and `30 (60)`.**
Add the divider field; pace two DMA edges per guest frame; keep the audio buffer at
panel rate.
*Acceptance:* `frames=` ≈25 or 30/s, `blits==frames`, `ips` still ≈1.26M,
`cpu_us_frame` roughly doubles while `cpu_pct` stays comparable, `int8 due` still
≈18/s, `resync` not climbing. **Human check for judder** — half-rate is a
smoothness trade and only an eye can price it.

**Stage 4 — persistence.**
`odroid_settings_app_int32_get/set("dos_screen_hz", …)`; read before the first
`lcd_set_refresh_rate()`.
*Acceptance:* set 50 Hz, exit to launcher, re-enter DOS, prof line shows ~50 fps
without touching the menu. Power-cycle and repeat.

**Stage 5 (optional) — unify the fixed and MAX execution paths** behind the chunked
watchdog loop, removing the long-un-refreshed-call hazard at 25/30 Hz.
*Acceptance:* prof line unchanged at 60 Hz vs Stage 1; no BSOD after 10 minutes at
`25 (50)` + `286 12MHz`. Coordinate: another agent owns `dos_cpu.c:163-186`.

---

## 9. Conventions note

This document is **uncommitted** by design; the requester will review and commit it
to avoid racing concurrent git operations on this branch. Once settled, the
`DECIDED:` items belong in `external/8086tiny/docs/decisions.md` and the buffer-length
ceiling (section 6) belongs in `external/8086tiny/docs/traps.md`, per the
documentation layout described in `Core/Src/porting/dos/CLAUDE.md`.
