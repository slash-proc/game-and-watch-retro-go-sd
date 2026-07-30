#!/usr/bin/env python3
"""Drive real hardware from a gwemu timeline and capture the log — in one probe session.

Why this exists
---------------
`gnwmanager monitor` and `scripts/remote_input.py` each spawn their **own** OpenOCD and
evict one another ("Disconnected from openocd"), so you can either press buttons or read
logs, never both at once. That makes timeline replay impossible, because a timeline is by
definition presses interleaved with a running capture.

This module opens **one** `OpenOCDBackend` and interleaves both jobs in a single loop:
the shadow-cell write that `remote_input` uses for buttons, and the `logbuf`/`log_idx`
ring-buffer poll that `gnwmanager monitor` uses for stdout. No threads, no locking — the
loop simply services whichever is due.

Timelines are the **same `.tl` format gwemu records and replays**, so a recording made
against the emulator runs on silicon unchanged, and the two logs can be diffed directly.

Requires firmware built with `REMOTE_INPUT=1` (it defaults to 0). Costs nothing at
runtime — measured identical `cpi` with and without.

Usage
-----
    gwharness.py replay TIMELINE [--capture SECS] [--out FILE] [--json] [--boot]
    gwharness.py run --keys a,a [--capture SECS] [--out FILE] [--json] [--boot]
    gwharness.py parse LOGFILE [--json]

`--boot` reset-halts the device and starts the clock at the instant of resume, so `t=0`
is the same event as gwemu's machine start.  Combined with the absolute-time `.tl`
format, that makes a run reproducible across emulator and silicon — feed both the same
timeline and diff the two logs.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))

import remote_input as ri  # noqa: E402

INTFLASH_ADDR = 0x08000000
POLL_S = 0.05
PRESS_HOLD_S = 0.1  # gwemu's `press` = down + 100ms + up


# --------------------------------------------------------------------------- timeline

def parse_time(tok: str) -> tuple[float, bool]:
    """Return (seconds, is_frame).  Accepts '@N', 'SS[.fff]', 'MM:SS[.fff]', trailing 's'."""
    tok = tok.strip()
    if tok.startswith("@"):
        # gwemu counts LTDC vblanks; hardware has no such hook, so translate at nominal 60Hz.
        return int(tok[1:]) / 60.0, True
    tok = tok.rstrip("s")
    if ":" in tok:
        mm, ss = tok.split(":", 1)
        return int(mm) * 60 + float(ss), False
    return float(tok), False


def buttons_of(spec: str) -> int:
    """'game+left' -> bitmask, matched by NAME (gwemu's bit order differs from ours)."""
    mask = 0
    for name in spec.split("+"):
        key = name.strip().upper()
        if key not in ri.NAMES:
            raise ValueError(f"unknown button {name!r}")
        mask |= 1 << ri.NAMES[key]
    return mask


def load_timeline(path: Path) -> tuple[list[tuple[float, str, int]], float | None]:
    """Parse a .tl into sorted (time, 'down'|'up', mask) events, plus a quit time."""
    events: list[tuple[float, str, int]] = []
    quit_at: float | None = None
    saw_frame = False

    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            raise ValueError(f"{path}:{lineno}: expected '<time> <action> [args]'")
        t, is_frame = parse_time(parts[0])
        saw_frame |= is_frame
        action = parts[1].lower()

        if action == "quit":
            quit_at = t if quit_at is None else min(quit_at, t)
        elif action == "screenshot":
            # fastcap lives in sibling repos, not here; gnwmanager's screenshot is broken
            # on this fork (framebuffer1/2 are pointers, not the 153,600-byte buffer).
            print(f"warning: {path}:{lineno}: screenshot not supported on hardware, skipped",
                  file=sys.stderr)
        elif action == "press":
            mask = buttons_of(parts[2])
            events.append((t, "down", mask))
            events.append((t + PRESS_HOLD_S, "up", mask))
        elif action == "hold":
            mask = buttons_of(parts[2])
            dur = float(parts[3]) if len(parts) > 3 else PRESS_HOLD_S
            events.append((t, "down", mask))
            events.append((t + dur, "up", mask))
        elif action == "down":
            events.append((t, "down", buttons_of(parts[2])))
        elif action == "release":
            events.append((t, "up", buttons_of(parts[2])))
        else:
            raise ValueError(f"{path}:{lineno}: unknown action {action!r}")

    if saw_frame:
        print("warning: timeline uses @frame addressing; translated at nominal 60Hz, "
              "so hardware timing will drift from gwemu", file=sys.stderr)
    events.sort(key=lambda e: e[0])
    return events, quit_at


# ------------------------------------------------------------------------- log reader

class LogReader:
    """The `logbuf`/`log_idx` ring-buffer protocol, on a caller-supplied backend."""

    def __init__(self, backend, elf: Path | None = None):
        from gnwmanager.elf import SymTab
        with (SymTab(elf) if elf else SymTab.find()) as symtab:
            buf = symtab["logbuf"]
            self.addr = buf.entry.st_value
            self.size = buf.entry.st_size
            self.idx_addr = symtab["log_idx"].entry.st_value
        self.backend = backend
        self.last = backend.read_uint32(self.idx_addr)

    def _decode(self, addr: int, n: int) -> str:
        if n <= 0:
            return ""
        data = self.backend.read_memory(addr, n)
        end = data.find(0)
        if end != -1:
            data = data[:end]
        return data.decode("utf-8", errors="replace")

    def poll(self) -> str:
        idx = self.backend.read_uint32(self.idx_addr)
        out = ""
        if idx > self.last:
            out = self._decode(self.addr + self.last, idx - self.last)
        elif 0 < idx < self.last:  # wrapped
            out = self._decode(self.addr + self.last, self.size - self.last)
            out += self._decode(self.addr, idx)
        self.last = idx
        return out


# ----------------------------------------------------------------------- prof parsing

PROF_RE = re.compile(
    r"cpi=(?P<cpi>\d+).*?cpu=(?P<cpu>\d+)%\s*\((?P<cpu_us>\d+)us/f\)"
    r".*?blit=(?P<blit>\d+)%\s*\((?P<blit_us>\d+)us"
    r".*?idle=(?P<idle>\d+)%.*?putc=(?P<putc>\d+)"
)
IPS_RE = re.compile(r"ips=(\d+)")


def parse_prof(text: str) -> list[dict]:
    rows = []
    for line in text.splitlines():
        m = PROF_RE.search(line)
        if not m:
            continue
        row = {k: int(v) for k, v in m.groupdict().items()}
        ips = IPS_RE.search(line)
        if ips:
            row["ips"] = int(ips.group(1))
        rows.append(row)
    return rows


def summarise(rows: list[dict], drop: int = 1) -> dict:
    """Median-based summary.  The first sample spans the SD core load and is garbage."""
    kept = rows[drop:]
    if not kept:
        return {"n": 0, "error": "no prof samples"}
    cpis = [r["cpi"] for r in kept]
    out = {
        "n": len(kept),
        "cpi_median": statistics.median(cpis),
        "cpi_mean": round(statistics.fmean(cpis), 2),
        "cpi_min": min(cpis),
        "cpi_max": max(cpis),
        "cpi_sd": round(statistics.pstdev(cpis), 2) if len(cpis) > 1 else 0.0,
    }
    for key in ("cpu", "cpu_us", "blit_us", "idle", "putc"):
        out[key + "_median"] = statistics.median(r[key] for r in kept)
    return out


# ------------------------------------------------------------------------------ driver

def drive(events, quit_at, capture_s, out_path, do_boot, elf=None):
    deadline = capture_s if quit_at is None else min(capture_s, quit_at)
    captured: list[str] = []

    with ri.session() as dev:
        backend = dev.backend
        reader = LogReader(backend, elf)
        held = 0
        i = 0
        next_poll = 0.0

        if do_boot:
            # Reset-halt, then start the clock at the instant of resume.  This makes t=0
            # the same event as gwemu's machine start, so an absolute-time timeline lines
            # up on both targets and the two logs are directly comparable.
            dev.transport.write_mask(0)
            backend.reset_and_halt()
            backend.resume()
            t0 = time.monotonic()
            reader.last = 0  # firmware zeroes log_idx during init; capture from boot
        else:
            t0 = time.monotonic()

        while True:
            now = time.monotonic() - t0
            if now >= deadline:
                break
            while i < len(events) and events[i][0] <= now:
                _, kind, mask = events[i]
                held = (held | mask) if kind == "down" else (held & ~mask)
                dev.transport.write_mask(held)
                i += 1
            if now >= next_poll:
                chunk = reader.poll()
                if chunk:
                    captured.append(chunk)
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
                next_poll = now + POLL_S
            nxt = events[i][0] if i < len(events) else deadline
            time.sleep(max(0.0, min(next_poll, nxt, deadline) - (time.monotonic() - t0)))

        dev.transport.write_mask(0)

    text = "".join(captured)
    if out_path:
        Path(out_path).write_text(text)
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--capture", type=float, default=25.0, help="seconds to capture")
        p.add_argument("--out", default="build/gwharness.log", help="write raw log here")
        p.add_argument("--json", action="store_true", help="emit the summary as JSON")
        p.add_argument("--boot", action="store_true", help="reset-halt then resume; t=0 is the resume instant (matches gwemu machine start)")
        p.add_argument("--elf", default=None, help="ELF for logbuf symbols")

    p = sub.add_parser("replay", help="replay a gwemu .tl timeline")
    p.add_argument("timeline")
    common(p)

    p = sub.add_parser("run", help="tap a comma-separated key list, then capture")
    p.add_argument("--keys", default="a,a")
    p.add_argument("--gap", type=float, default=1.5, help="seconds between taps")
    p.add_argument("--start", type=float, default=0.5, help="delay before the first tap")
    common(p)

    p = sub.add_parser("parse", help="summarise prof lines in an existing log")
    p.add_argument("logfile")
    p.add_argument("--json", action="store_true")

    a = ap.parse_args()

    if a.cmd == "parse":
        rows = parse_prof(Path(a.logfile).read_text())
    else:
        if a.cmd == "replay":
            events, quit_at = load_timeline(Path(a.timeline))
        else:
            events, quit_at = [], None
            t = a.start
            for key in [k for k in a.keys.split(",") if k.strip()]:
                mask = buttons_of(key)
                events.append((t, "down", mask))
                events.append((t + PRESS_HOLD_S, "up", mask))
                t += a.gap
        text = drive(events, quit_at, a.capture, a.out, a.boot, a.elf)
        rows = parse_prof(text)

    summary = summarise(rows)
    if a.json:
        print(json.dumps(summary, indent=2))
    else:
        print("\n=== summary (first sample dropped) ===")
        if summary.get("n"):
            print(f"n={summary['n']}  cpi median={summary['cpi_median']} "
                  f"mean={summary['cpi_mean']} sd={summary['cpi_sd']} "
                  f"min={summary['cpi_min']} max={summary['cpi_max']}")
            print(f"cpu={summary['cpu_median']}% ({summary['cpu_us_median']}us/f)  "
                  f"blit={summary['blit_us_median']}us  idle={summary['idle_median']}%  "
                  f"putc={summary['putc_median']}")
        else:
            print("no prof samples")
    return 0 if summary.get("n") else 1


if __name__ == "__main__":
    sys.exit(main())
