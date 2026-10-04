#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Survey recorded sessions for the Sense always-lit fault and the conditions around each onset.

A scan step is "lit out of window" when it scores >= 1 camera while its pulse lies well outside the lit window every
healthy lock has found (fudge ~15.0-16.7 ms, wrapping to ~0.3 ms). The fault persists, so the onset is the first of
three consecutive such steps; single lit steps are the other ring or a scan edge. For every scan
(faulted or not) it prints the conditions that might matter: controller uptime (its IMU clock, 1/3 us ticks since
power-on), LED sequence number, how many LED settings the controller had latched so far, the other controller's state,
and what changed at the onset step.

    psvr2_sense_stuck_lit_survey.py ~/Code/psvr2-datasets/sessions/2026092*
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

STEP = re.compile(r"LED_BOOTSTRAP side=(.) event=step stage=(\w+) step=(\d+)/(\d+) fudge_us=([\d.]+) pulse_us=([\d.]+) "
                  r"score=([\d.]+) mean_blobs=([\d.]+)")
START = re.compile(r"LED_BOOTSTRAP side=(.) event=scan_start stage=(\w+)")
BASE = re.compile(r"LED_BOOTSTRAP side=(.) event=baseline blobs=([\d,]+)")
LOCK = re.compile(r"LED_BOOTSTRAP side=(.) event=locked ")
TIMING = re.compile(r"PSSENSE_TIMING side=(.) .*host_now_ns=(\d+) .*phase=(\d+) led_seq=(\d+) period_id=(\d+) .*"
                    r"cycle_position=(\d+) .*device_ticks_est=(\d+)")


def should_be_dark(stage: str, fudge_us: float) -> bool:
    if stage == "wide":  # 2.1 ms pulses starting at fudge; healthy peaks at 13000-16000
        return 1000 <= fudge_us <= 10000
    return 1500 <= fudge_us <= 13500  # 450 us pulses; healthy windows ~15000-16500 and wrap to ~300


def survey(session: Path) -> None:
    log = session / "run.log"
    if not log.exists():
        return
    t0 = None
    last = {}  # side -> (host_ns, phase, led_seq, period_id, device_ticks)
    seqs_latched = {"L": set(), "R": set()}
    state = {"L": "idle", "R": "idle"}
    scans = {"L": 0, "R": 0}
    onset = {}
    candidate = {}  # side -> [consecutive lit-out-of-window steps, conditions at the first]
    for line in log.open(errors="replace"):
        m = TIMING.search(line)
        if m:
            side, host, phase, seq, period, pos, ticks = m.groups()
            host = int(host)
            t0 = t0 or host
            last[side] = (host, int(phase), int(seq), int(period), int(ticks))
            seqs_latched[side].add(int(seq))
            continue
        m = START.search(line)
        if m:
            state[m.group(1)] = m.group(2)
            if m.group(2) == "wide":
                scans[m.group(1)] += 1
            continue
        m = LOCK.search(line)
        if m:
            state[m.group(1)] = "locked"
            continue
        m = BASE.search(line)
        if m:
            continue
        m = STEP.search(line)
        if not m:
            continue
        side, stage, step, n, fudge, pulse, score, blobs = m.groups()
        fudge, score = float(fudge), float(score)
        other = "R" if side == "L" else "L"
        if side in onset or not should_be_dark(stage, fudge):
            continue
        if score < 1.0:
            candidate.pop(side, None)
            continue
        host, phase, seq, period, ticks = last.get(side, (t0 or 0, -1, -1, -1, 0))
        c = candidate.setdefault(side, [0, None])
        c[0] += 1
        if c[1] is None:
            c[1] = dict(t=(host - (t0 or host)) / 1e9, stage=stage, step=f"{step}/{n}", fudge=fudge, pulse=float(pulse),
                           score=score, scan=scans[side], uptime_s=ticks / 3e6, led_seq=seq,
                           latched=len(seqs_latched[side]), other=state[other], period=period)
        if c[0] >= 3:
            onset[side] = c[1]
    for side in "LR":
        if scans[side] == 0:
            continue
        host, phase, seq, period, ticks = last.get(side, (0, -1, -1, -1, 0))
        o = onset.get(side)
        if o:
            print(f"{session.name:42s} {side} FAULT at {o['t']:5.1f}s scan {o['scan']} {o['stage']} step {o['step']:6s} "
                  f"fudge {o['fudge']:7.0f} pulse {o['pulse']:6.0f}  uptime {o['uptime_s']:6.0f}s  led_seq {o['led_seq']:3d}  "
                  f"latched {o['latched']:3d}  other {o['other']}")
        else:
            print(f"{session.name:42s} {side} ok     scans {scans[side]}  uptime at end {ticks / 3e6:6.0f}s  "
                  f"latched {len(seqs_latched[side]):3d}")


def main() -> int:
    for arg in sys.argv[1:]:
        survey(Path(arg).expanduser())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
