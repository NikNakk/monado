#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Coordinate the existing C LED probe and camera survey, then latch/capture OFF.

Experimental orchestration only. Keep both devices fixed. Use caffeinate -dims.
The C probe's known force-IR recipe is preserved; this is not a Sony BROAD vector.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import re
import csv
import cv2
from pssense_sony_wire_prepare import phase_reference


def settings_batches(args, repo, out, mark):
    """Refresh with the existing phase diagnostic; never overlap HID owners."""
    plan = json.loads(args.settings_plan.read_text())
    side = args.hand[0].upper()
    if plan['side'] != side or args.calibration is None or args.off_roi is None:
        raise ValueError('settings batches require matching side, calibration and ring ROI')
    env = os.environ | {
        'PSSENSE_LED_BOOTSTRAP_HINT_US': '16350', 'PSSENSE_LED_BOOTSTRAP_STRICT': '1',
        'PSSENSE_LED_BOOTSTRAP_LED_BLOBS': '1', 'PSSENSE_LED_BOOTSTRAP_FIRST': side,
        'PSSENSE_LEDS_OFF_ON_EXIT': '1', 'PSSENSE_LED_BOOTSTRAP_WIDE_PERIOD_ID': '32',
        'PSSENSE_RAW_REPORTS': '1',
        'PSSENSE_LED_BOOTSTRAP_TRACK': '1', 'PSSENSE_LED_BOOTSTRAP_TRACK_COVERAGE': '0', 'PSSENSE_FORCE_IR': '0',
    }
    count = len(plan['plan'])
    if args.start_step < 0 or args.start_step % 6:
        raise ValueError('resume offset must be a complete two-trial batch boundary')
    for start in range(args.start_step, count, 6):
        batch = start // 6
        mark('phase_refresh_start', batch=batch, plan_start=start)
        phase_log = out / f'{batch:02d}-phase.log'
        with phase_log.open('x') as f:
            status = subprocess.run([str(repo / 'scripts/psvr2_sense_session.sh'),
                f'wire-phase-{out.name[-30:]}-{batch:02d}', str(args.calibration), str(args.phase_seconds),
                f'Stationary {side}; phase refresh for captured settings steps {start}:{min(start+6,count)}'],
                env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        text = phase_log.read_text()
        saved = re.findall(r'Session saved: (.+)', text)
        if status not in (0, 2) or not saved:
            raise RuntimeError('phase diagnostic failed; inspect batch phase log')
        session = Path(saved[-1])
        reference_log = session / 'run.log'
        reference = phase_reference(reference_log.read_text(), side)
        with (session / 'capture/camera0.csv').open() as f:
            frames = list(csv.DictReader(f))[-20:]
        x0, y0, x1, y1 = args.off_roi
        lit = []
        for row in frames:
            image = cv2.imread(str(session / 'capture' / row['file']), 0)
            lit.append(int((image[y0:y1, x0:x1] > 100).sum()) > 10)
        fraction = sum(lit) / len(lit) if lit else 0
        mark('phase_tail_gate', batch=batch, frame_count=len(lit), populated_fraction=fraction)
        if fraction < 0.8:
            raise RuntimeError('phase calibration tail is not reliably lit; no captured trial started')
        mark('phase_refresh_end', batch=batch, status=status, session=str(session), reference=reference)
        # Prefer the selected side's actual last TX, including exit OFF. The
        # separate relinked diagnostic supports this; retain the documented
        # timing margin for the original cached binary and verify OFF optically.
        phase_text = reference_log.read_text()
        if re.search(rf'PSSENSE_RAW side={side}[^\n]*direction=tx[^\n]*hex=', phase_text):
            seed = last_probe_led_sequence(phase_text, side)
            latch_source = 'last_actual_tx'
        else:
            seqs = re.findall(rf'PSSENSE_TIMING side={side} .*?led_seq=(\d+)', phase_text)
            if not seqs:
                raise RuntimeError('phase diagnostic lacks sequence evidence')
            seed = (int(seqs[-1]) + 16) & 255
            latch_source = 'timing_plus_16'
        command = [sys.executable, str(repo / 'scripts/pssense_led_poke.py'),
            str(out / f'{batch:02d}-settings'), '--side', side, '--settings-plan', str(args.settings_plan),
            '--start-step', str(start), '--limit-steps', str(min(6, count-start)),
            '--initial-led-sequence', str(seed), '--phase-reference-log', str(reference_log),
            '--off-roi', *map(str, args.off_roi), '--prescan-gate', '--compact-examples']
        if args.raw_if8:
            command += ['--raw-if8']
        mark('settings_batch_start', batch=batch, command=command, latch_seed=seed, latch_source=latch_source,
             phase_cli=env.get('MONADO_CLI', 'session-script-default'))
        with (out / f'{batch:02d}-settings.log').open('x') as f:
            subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, check=True)
        mark('settings_batch_end', batch=batch)
        print(f'batch {batch+1}/{(count+5)//6}: plan steps {start}-{min(start+6,count)-1} passed controls', flush=True)


def last_probe_led_sequence(text, side=None):
    """Use actual post-override TX bytes, never the caller's pre-override report."""
    prefix = rf'PSSENSE_RAW side={side}[^\n]*' if side else ''
    reports = re.findall(prefix + r"direction=tx[^\n]*hex=([0-9a-f]+)", text)
    for report in reversed(reports):
        data = bytes.fromhex(report)
        if len(data) == 78 and data[0] == 0x31:
            return data[23]
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("--hand", choices=["left", "right"], required=True)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--sequence", default="4")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--sample", type=float, default=2)
    parser.add_argument("--bc4-layout", choices=["sbs", "stacked", "planar"])
    parser.add_argument("--raw-if8", action="store_true")
    parser.add_argument("--settings-plan", type=Path, help="run captured trials in two-trial batches with fresh existing phase diagnostics")
    parser.add_argument("--calibration", type=Path, help="existing phase diagnostic calibration")
    parser.add_argument("--off-roi", type=int, nargs=4)
    parser.add_argument("--start-step", type=int, default=0, help="resume the immutable plan at a six-stage batch boundary")
    parser.add_argument("--phase-seconds", type=int, default=30, help="existing phase diagnostic duration; allow clock startup to converge")
    args = parser.parse_args()
    out = args.out.expanduser().resolve()
    if out == Path("/private/tmp") or Path("/private/tmp") in out.parents:
        parser.error("recordings must be persistent, never /tmp")
    if args.cycles < 1 or args.repeat < 1 or args.sample <= 0:
        parser.error("invalid capture duration or count")
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error("output directory must be empty")
    repo = Path(__file__).resolve().parents[1]
    probe = repo / "build-sense-rel/src/xrt/auxiliary/os/pssense_hid_probe"
    # Include settling, mode synchronisation and startup margin in the LED hold.
    seconds = min(600, int(8 + (args.sample + 4) * len(args.sequence.split(",")) * args.repeat))
    env = os.environ | {"PSSENSE_TIMING_DIAG": "1", "PSSENSE_RAW_REPORTS": "1"}
    with (out / "markers.jsonl").open("x") as markers:
        def mark(event, **fields):
            markers.write(json.dumps(dict(event=event, host_monotonic_ns=time.monotonic_ns(),
                                          clock_monotonic_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC),
                                          host_realtime_ns=time.time_ns(), **fields)) + "\n")
            markers.flush()
        if args.settings_plan:
            settings_batches(args, repo, out, mark)
            return 0
        for cycle in range(args.cycles):
            mark("probe_start", cycle=cycle, hand=args.hand, seconds=seconds)
            with (out / f"{cycle:02d}-probe.log").open("x") as log:
                process = subprocess.Popen([str(probe), "--hand", args.hand, "--force-ir-seconds", str(seconds)],
                                           stdout=log, stderr=subprocess.STDOUT, env=env)
                try:
                    time.sleep(4)
                    if process.poll() is not None:
                        raise RuntimeError(f"LED probe exited early ({process.returncode}); inspect probe log")
                    command = [sys.executable, str(repo / "scripts/psvr2_camera_mode_survey.py"),
                               str(out / f"{cycle:02d}-on"), "--sequence", args.sequence, "--repeat", str(args.repeat),
                               "--settle", "0.5", "--sample", str(args.sample), "--examples", "500",
                               "--save-every", "2", "--no-contact-sheet"]
                    if args.bc4_layout:
                        command += ["--bc4-layout", args.bc4_layout]
                    if args.raw_if8:
                        command += ["--raw-if8"]
                    mark("capture_start", cycle=cycle, command=command)
                    with (out / f"{cycle:02d}-camera.log").open("x") as camera_log:
                        subprocess.run(command, stdout=camera_log, stderr=subprocess.STDOUT, check=True)
                    mark("capture_end", cycle=cycle)
                    if process.wait(timeout=seconds + 10) != 0:
                        raise RuntimeError("LED probe failed")
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)
                    mark("probe_end", cycle=cycle, returncode=process.returncode)
                    # The two senders never own HID concurrently. OFF also runs on failure.
                    log.flush()
                    last_sequence = last_probe_led_sequence((out / f"{cycle:02d}-probe.log").read_text())
                    mark("off_start", cycle=cycle, previous_led_sequence=last_sequence)
                    with (out / f"{cycle:02d}-off.log").open("x") as off_log:
                        subprocess.run([sys.executable, str(repo / "scripts/pssense_led_poke.py"),
                                        str(out / f"{cycle:02d}-off"), "--side", args.hand[0].upper(),
                                        "--initial-led-sequence", str(last_sequence),
                                        "--steps", "off", "--hold", "4", "--sample", "2"],
                                       stdout=off_log, stderr=subprocess.STDOUT, check=True)
                    mark("off_end", cycle=cycle)
            print(f"cycle {cycle + 1}/{args.cycles} recorded", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
