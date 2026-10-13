#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("pacing", Path(__file__).resolve().parents[1] / "scripts/macos/analyze-steamvr-driver-pacing.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

OFFSET = 5_000_000  # XrTime = qpc_us * 1000 + OFFSET
CLOCK = 7_000_000_000  # Monado monotonic = XrTime + CLOCK (per-instance constant)
SHIFT = 9  # Monado counted nine frames before the driver's first
# Irregular, non-repeating frame starts, as real runs mix 2-, 3- and
# 4-refresh periods.
STEPS = [(16_667, 25_000, 33_333)[(n * 7 + n * n) % 5 % 3] for n in range(64)]


def start(n):
    return 25_000 + sum(STEPS[i % len(STEPS)] for i in range(n - 1))


def driver(frames):
    lines = [f"# pid=1 qpc_us_now=0 xr_time_ns=qpc_us*1000+{OFFSET} gpu_rows=1",
             "kind,frame,wait_begin,wait_end,predicted_display,period_us,should_render,begin_end,present_begin,"
             "prepared,acquire_end,acquired,released,end_begin,end_end,layers"]
    for n in range(1, frames + 1):
        t = start(n)
        period = 8_333 if n == 1 else 25_000
        lines.append(f"frame,{n},{t},{t + 2000},{t + 60_000},{period},1,{t + 2100},{t + 20_000},{t + 20_100},"
                     f"{t + 21_000},1,{t + 21_200},{t + 21_300},{t + 21_500},1")
        lines.append(f"gpu,{n},source,{t + 39_000},1")
        lines.append(f"gpu,{n},copies,{t + 39_100},1")
        lines.append(f"vsync,{t + 2050},-1000,16000")
    return "\n".join(lines) + "\n"


def pacing(frames):
    rows = ["event,session_id,client_frame_id,event_ns,wake_ns,predicted_display_ns,predicted_period_ns,"
            "display_time_ns,cpu_est_ns,draw_est_ns,gpu_est_ns,cpu_actual_ns,draw_actual_ns,gpu_actual_ns"]
    for n in range(1, frames + 1):
        t = start(n)
        pred = (t + 60_000) * 1000 + OFFSET + CLOCK
        done = (t + 39_600) * 1000 + OFFSET + CLOCK
        rows.append(f"predict,0,{n + SHIFT},{t * 1000 + OFFSET + CLOCK},0,{pred},25000000,0,0,0,0,0,0,0")
        rows.append(f"gpu_done,0,{n + SHIFT},{done},0,{pred},25000000,{pred},0,0,0,0,19000000,18100000")
    return "\n".join(rows) + "\n"


class DriverPacingTests(unittest.TestCase):
    def test_alignment_recovers_shift_and_clock(self):
        r = m.analyze(driver(20), pacing(20))
        self.assertEqual(r["alignment"]["monado_frame_id_minus_driver_frame"], SHIFT)
        self.assertEqual(r["alignment"]["monado_minus_xrtime_ns"], CLOCK)

    def test_stages_and_gpu_attribution(self):
        s = m.analyze(driver(20), pacing(20))["stages_ms"]
        self.assertEqual(s["wait"]["p50"], 2.0)
        self.assertEqual(s["acquire"]["p50"], 0.9)
        self.assertEqual(s["gpu_source_after_acquire"]["p50"], 18.0)
        self.assertEqual(s["gpu_copies_after_source"]["p50"], 0.1)
        self.assertEqual(s["monado_gpu"]["p50"], 18.1)
        self.assertEqual(s["monado_gpu_done_after_driver_copies"]["p50"], 0.5)

    def test_period_multiples_use_shortest_period_as_refresh(self):
        r = m.analyze(driver(20), pacing(20))
        self.assertEqual(r["pacer_period_in_refreshes"], {1: 1, 3: 19})

    def test_start_up_is_skipped(self):
        r = m.analyze(driver(20), pacing(20), skip_seconds=0.25)
        self.assertEqual(r["stages_ms"]["wait"]["n"], sum(start(n) - start(1) >= 250_000 for n in range(1, 21)))

    def test_steady_cadence_alone_is_ambiguous(self):
        global STEPS
        saved, STEPS = STEPS, [25_000]
        try:
            with self.assertRaisesRegex(ValueError, "several shifts"):
                m.analyze(driver(20), pacing(20))
        finally:
            STEPS = saved

    def test_unalignable_traces_are_rejected(self):
        with self.assertRaises(ValueError):
            m.analyze(driver(20), pacing(20).replace("predict,", "ignored,"))


if __name__ == "__main__":
    unittest.main()
