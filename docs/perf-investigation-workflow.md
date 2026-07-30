# Performance investigation workflow (real hardware)

The loop we use to find and fix performance problems in this repo, on a real
Game & Watch over a debug probe. Adapted from the qemu-gnw fork's
`docs/perf-investigation-workflow.md`, rewritten around *silicon* rather than an
emulator — different hazards, different instruments, same discipline.

Written after a session that produced **+22% guest throughput** on the MS-DOS core
(three banked wins) and **seven documented refutations**, at low context cost.

Not specific to the DOS core. Anything with a repeatable scene and a trustworthy number
fits. Companions: `external/8086tiny/docs/cpu/05-perf-handover.md` (current state),
`06-opcode-histogram.md` (what this loop found), `docs/gwemu.md` (the emulator, which is
**not** a perf harness — see *Metric discipline*).

## The loop

1. **Hypothesis** — from a profile or from source, never from intuition alone.
2. **Delegate** one tightly-scoped experiment to one agent.
3. **Measure** — median of ≥15 samples against a known noise floor.
4. **Verify** before believing it (see *Verification*).
5. **Bank or refute** — commit the win, or commit the negative *with its data*.
6. **Re-profile.** The next hypothesis comes from the *new* profile, not the old plan.

Step 6 is what makes it converge. Four consecutive interpreter micro-optimisations failed
here because nobody had profiled *which opcodes actually run*. Building an instruction
histogram overturned the working assumption in one run and produced the session's only
large win. **When experiments keep failing, the plan is wrong, not the experiments.**

## Orchestrator / agent split

The orchestrator (main session) holds the *thread of reasoning*. Agents hold the *work*.
That is why context barely moves across many experiments.

**Orchestrator:** forms hypotheses, writes briefs, reads diffs on correctness-critical
changes, decides bank-vs-refute, commits, keeps the state docs current, and **owns the
one thing agents cannot do — asking the human to look at the screen.**

**Agents:** builds, flashes, measurement runs, source spelunking, instrumentation, sweeps.

**Agents return numbers and file:line citations, not narrative.** A good report is a table
plus a verdict.

Practical:
- **Never read an agent's raw transcript file.** It will overflow context. Final report only.
- **ONE agent at a time on the device.** There is a single probe and a single board.
  Two agents flashing concurrently corrupt each other's runs, and the symptom looks like
  random emulator misbehaviour, not a resource conflict. Parallelise **read-only** work
  (source surveys, doc reading) freely — that is safe and cheap.
- An agent that finishes can be **resumed with `SendMessage`** and keeps its context. Prefer
  that over spawning a fresh agent to fix a bug in work it just did.

## The agent brief

Every line below exists because omitting it cost a run:

- **Established facts marked "do not re-derive"**, plus what was already refuted.
- **The exact build, flash and measure command lines.** Copy-paste, not description.
- **The metric, and what makes a run invalid** ("no `prof` lines means the run FAILED —
  report it, never invent a number").
- **Explicit permission to fail.** "Concluding this is unsafe is a SUCCESSFUL outcome",
  "if it is within noise, say so plainly." Without this, agents find a win.
- **Machine hazards** (below) — every brief, every time.
- **Required end state**: commit to a branch, leave the device flashed with X, confirm
  with `git status`.
- **A report format.**

**Split research from implementation when a change touches a correctness invariant.**
Phase 1 establishes *why* the code is the way it is; only a "safe" verdict unlocks phase 2.
This is what caught that the 8086 register file lives *inside* the guest memory array, so
"just move the registers to DTCM" would have been silently wrong.

## Device workflow

### Build

```
make -j$(nproc) CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
     DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=1 CHEAT_CODES=1 REMOTE_INPUT=1 release
make ... same flags ... create_sd_data
```

- **`REMOTE_INPUT=1` is required for unattended runs** and defaults to **0**. It compiles a
  read of a DTCM shadow cell (`0x2001FFF4`, `Core/Src/gw_buttons.c`) that a probe write ORs
  into the live button state. **Measured cost: none** (identical `cpi` with and without).
- **`INTFLASH_BANK=1`** for a standalone install; bank 2 is for dual-boot. Must match how
  the bootloader was installed.

### Flash — intflash + one core, not `flash_sd`

```
gnwmanager flash 0x08000000 build/gw_retro_go_intflash.bin \
  -- sdpush --file sd_content/cores/dos.bin --dest-path "/cores/" \
  -- start 0x08000000
```

`flash_sd` pushes all 18 cores and is far slower. **Flashing intflash alone tests stale
code** — the core lives on the SD card.

### Measure

```
timeout 140 python3 scripts/gwharness.py replay timelines/dos-boot.tl \
        --capture 35 --boot --out build/run.log
```

`scripts/gwharness.py` (ours) does reset-halt → resume → gwemu-format timeline replay →
log capture → parsed median, **all over one OpenOCD connection**. Give the Bash call a
180000–200000 ms timeout.

- **`gnwmanager monitor` and `scripts/remote_input.py` cannot run concurrently.** Each
  spawns its own OpenOCD and the second evicts the first
  (`Disconnected from openocd. Response so far: b''`). `gwharness.py` exists to solve this.
- **`--boot` reset-halts and starts the clock at the resume instant**, so `t=0` is the same
  event as gwemu's machine start. With the `.tl` format's absolute timestamps, the same
  timeline replays on emulator and silicon and the logs are directly comparable.
- **Timelines are the gwemu `.tl` format**: `press` / `down` / `hold <secs>` / `release` /
  `quit`, `+`-joined chords, `[MM:]SS[.fff]` absolute times, `@N` frames. Buttons match
  **by name** — gwemu's bit order differs from `remote_input.py`'s, so index mapping
  presses the wrong buttons.
- **Reaching a scene:** retro-go remembers the last-launched ROM, so once a human has
  selected it, boot + two `A` presses returns to it (`timelines/dos-boot.tl`). Navigating
  to a *different* ROM blind does not reliably work — **ask the human to select it once.**
- `@frame` addressing cannot be exact on hardware (no vblank hook); it is translated at
  nominal 60 Hz with a warning. `screenshot` is skipped — `fastcap.py` lives in sibling
  repos, and `gnwmanager screenshot` is broken on this fork (`framebuffer1`/`2` are
  *pointers*, not the 153,600-byte buffer).

### Reading the numbers

The `prof` line, once per second:

```
DOS: prof 286 12MHz @280MHz ipf=20000 ips=1199625 cpi=208 | cpu=89% (14894us/f)
     blit=3% (520us x64) idle=7% other=1% putc=0 tick=20/20 rs=0 frames=64/1067ms
```

- **Discard the first sample** — it spans the SD core load.
- **Use the median**, not the mean; early samples skew high.
- `tick=fired/due` and `rs=` are the guest-clock health check; `putc` is guest output rate.
- **The frame-drop signal is `blit=…x<N>`, NOT `frames=N/Mms`.** This is counter-intuitive
  and cost a sweep to discover. `common_emu_frame_loop()` skips the actual screen draw when
  it is falling behind, so **`blit` count out of 64 is what collapses** (60–64 healthy,
  33–35 = half the visual frames dropped). Meanwhile `frames=64/1066ms` stays *perfectly
  healthy* through the drops, because loop pacing holds even as draws are skipped.
  Neither field is in the parsed summary — grep the raw log whenever sweeping anything
  that costs frame time.

## Metric discipline

**Pick a metric that can see the thing you are changing.** Traps that have each cost a run:

- **`ips` is `ipf × fps` — constant by construction.** It cannot show a throughput change,
  ever. Do not use it as one.
- **`cpi` is integer-quantised with sd ≈ 4.** It cannot resolve a change under ~5%. For
  small effects use **µs/frame**, which is what `cpi` is derived from and far more precise.
  A 2% win was nearly discarded because the brief said "judge by `cpi`".
- **Any change that alters *what counts as an instruction* makes `cpi` RISE while the guest
  gets FASTER.** The segment-prefix fast path took `cpi` 194 → 212 and was a **+19.3%
  speedup**. For those changes, normalise to *cycles per real guest instruction*.
- **A win on a scene with idle headroom buys headroom, not speed.** It converts to speed
  only by raising `ipf` — which changes the *emulated machine's identity*, not just its
  performance, and is a product decision.
- **gwemu is not a performance harness for this core.** It inverts cpu-vs-blit ~100× and
  has no cache model. Hardware only. See `external/8086tiny/docs/gwemu-performance.md`.
- **Timing cannot establish renderer correctness.** A visibly flickering text cache measured
  118 µs and its fixed version 120 µs. **A human must look at the screen.**

Establish the noise floor first, and never report a delta smaller than it.

## Verification

Assume every result is wrong until it survives:

- **Re-measure the baseline on the current tree.** A baseline from before an unrelated
  change is not a baseline.
- **Reproduce independently before committing** — a separate rebuild and reflash, ideally
  by the orchestrator rather than the agent that found it.
- **A green build proves nothing.** `--gc-sections` once discarded an entire core. Verify
  with `nm`.
- **Health signature after every change**: DOS reaches a clean prompt, `putc=0`,
  `tick` fired==due, `rs=0`, stable across the whole capture. A silent wrong-answer bug
  (bad segment override, corrupted stack) shows as a hang or garbage, not a fault.
- **Anything touching the display needs human eyes.** Four checks: no flicker, cursor
  blinks, scrolling clean, overlay open/close leaves no remnants.
- **Check the workload can even exercise the thing.** A text-mode renderer optimisation was
  measured, landed, and then shelved on realising games use a *different* blit path
  entirely — it optimised the idle prompt, the scene where speed matters least.

## Git usage

- **One branch per experiment, kept even when refuted**: `perf/<short-name>` in the
  superproject, and in the `external/8086tiny` submodule.
- **Commit the experiment to its branch before reverting it.** Two refuted experiments were
  reverted by agents before this rule existed; only their numbers survive, the code is gone.
- **Commit messages carry the data** — both arms' numbers, the method, the residual risks.
  The commit is the durable record; a chat log is not.
- **Commit early and often, by explicit path**, while agents are running. Staging named
  paths avoids capturing an agent's in-flight work.
- **NEVER `git checkout .` inside `external/8086tiny`.** It reverts the whole submodule
  including documentation edits made while an agent ran. This destroyed a set of handover
  notes. Always name the specific file.
- Submodule commits need a **gitlink bump** in the superproject (`git add external/8086tiny`)
  — and run it **from the repo root**, not from inside `external/`.
- **Default-off for anything whose safety net is untested.** An opt-in build flag with a
  documented gating condition is a legitimate deliverable; silently enabling it is not.

## Machine hazards

Put these in every brief.

- **Every Bash call needs `dangerouslyDisableSandbox: true`** in this environment, or it
  fails on `apply-seccomp`.
- **Use absolute paths, always from the repo root.** `cd`-ing into `external/8086tiny` and
  then running `gnwmanager` gives "No ELF files found!"; running `git add external/8086tiny`
  from `external/` gives "pathspec did not match".
- **Never `pkill -f <pattern>`** — it matches the agent's own command line and kills it.
  Use `pkill -x <name>`, or kill by PID. (The gwemu binary is `gwemu_bin`, not `gwemu`.)
- **`/tmp` is RAM.** Never fill it. Logs and captures go in `build/` (gitignored).
- **Flag changes are not make dependencies.** `DOS_CFLAGS_EXTRA` **and** `C_DEFS` both
  silently reuse stale objects — `rm -f build/dos/*.o` (or the specific object) or you will
  measure the previous build. This has produced a false "no effect" *and* a false "the hook
  isn't compiled in".
- **`BUILD_DIR` does not isolate a build** — 96 hardcoded paths in the linker script.
  Serialise builds; snapshot artifacts you intend to test.
- **One heavy job at a time.** Concurrent builds invalidate timings and fight for the probe.
- Verifying a symbol by grepping the disassembly for its address gives **false negatives** —
  it compiles to base+offset (`ldr.w r3, [r3, #4084]`, not `2001fff4`).

## When an excursion ends

An excursion is done when it is either **committed with its measurements**, or **recorded
as refuted with the data that refutes it**, or **parked on a branch with the measured
reason it was parked**.

"Tried it, didn't seem to help" is not a conclusion. It is an unfinished excursion, and
someone will retry it in six months.
