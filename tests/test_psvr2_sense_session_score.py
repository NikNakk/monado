#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_sense_session_score import parse_log, render_text, score_session  # noqa: E402

LOG = """\
 INFO [pssense_create] LED phase bootstrap enabled (replaces pose-driven LED sync refinement)
 INFO [finish_baseline] LED_BOOTSTRAP side=L event=baseline blobs=7,0,8,1 reported=8,8,8,8
\x1b[32m INFO \x1b[0m[begin_scan] LED_BOOTSTRAP side=L event=scan_start stage=wide steps=17 start_us=0.0 step_us=1000.0 pulse_us=2100.0 period_us=16683.0
 INFO [finish_step] LED_BOOTSTRAP side=L event=step stage=wide step=1/17 fudge_us=0.0 pulse_us=2100.0 score=0.000 mean_blobs=0.50 lit=0/8,0/8,0/8,0/8
 INFO [finish_step] LED_BOOTSTRAP side=L event=step stage=wide step=2/17 fudge_us=1000.0 pulse_us=2100.0 score=3.750 mean_blobs=19.00 lit=8/8,8/8,7/8,7/8
 INFO [finish_wide_scan] LED_BOOTSTRAP side=L event=wide_result best_step=1 fudge_us=1000.0 peak=3.750 median=0.000
 INFO [begin_scan] LED_BOOTSTRAP side=L event=scan_start stage=narrow steps=21 start_us=-500.0 step_us=250.0 pulse_us=450.0 period_us=16683.0
 INFO [finish_step] LED_BOOTSTRAP side=L event=step stage=narrow step=1/21 fudge_us=16183.0 pulse_us=450.0 score=0.000 mean_blobs=0.00 lit=0/8,0/8,0/8,0/8
 INFO [finish_step] LED_BOOTSTRAP side=L event=step stage=narrow step=2/21 fudge_us=16433.0 pulse_us=450.0 score=4.000 mean_blobs=20.00 lit=8/8,8/8,8/8,8/8
 INFO [finish_narrow_scan] LED_BOOTSTRAP side=L event=locked lit_start_us=16433.0 lit_end_us=16683.0 window_us=700.0 centre_us=16783.0 lock_fudge_us=16283.0 lock_pulse_us=1000.0 peak=4.000
 INFO [pssense_led_bootstrap_update_locked] LED_BOOTSTRAP side=L event=locked_status fudge_us=16283.0 pulse_us=1000.0 lit_reports=900/1200 frames_since_lit=0
 WARN [fail_scan] LED_BOOTSTRAP side=R event=scan_failed stage=wide reason=wide_peak_below_minimum
 INFO [pssense_push_constellation_tracker_sample] CONSTELLATION_CANDIDATE side=L ts=1 cam=0 pos=(0.1,0.2,0.3) quat=(0,0,0,1) matched=5 visible=6 reproj=0.5 brightness=1.0 imu_valid=1 imu_quat=(0,0,0,1) imu_delta_deg=3.00 imu_aligned_valid=1 imu_aligned_delta_deg=1.50 imu_alignment_age_ms=10.0
 INFO [pssense_push_constellation_tracker_sample] CONSTELLATION_CANDIDATE side=L ts=1 cam=2 pos=(0.1,0.2,0.3) quat=(0,0,0,1) matched=4 visible=6 reproj=0.7 brightness=1.0 imu_valid=1 imu_quat=(0,0,0,1) imu_delta_deg=3.00 imu_aligned_valid=0 imu_aligned_delta_deg=nan imu_alignment_age_ms=nan
 INFO [pssense_push_constellation_tracker_sample] CONSTELLATION_FUSED_ACCEPT side=L ts=1 cameras=2 matched=5 reproj=0.5 reacquired=0 imu_alignment_valid=1
 INFO [pssense_push_constellation_tracker_sample] CONSTELLATION_REACQUIRE side=L ts=2 gap_ms=300.0 cameras=3 pos_delta_mm=10.0 orientation_delta_deg=2.0
 WARN [deferSampleToSlowThread] Dropping slow sample 12 at ts 34. Tracker is likely running slow.
"""

HEADER = (
    "timestamp_ns,hand,relation_flags,px,py,pz,qx,qy,qz,qw,pose_age_ns,fused_pose_count,fused_camera_count,"
    "candidate_count,disagreement_count,jump_rejection_count,led_bootstrap_state,led_bootstrap_fudge_us,"
    "led_bootstrap_pulse_us,led_bootstrap_scans,led_bootstrap_locks\n"
)


def write_pgm(path: Path, image: np.ndarray):
    path.write_bytes(b"P5\n%d %d\n255\n" % (image.shape[1], image.shape[0]) + image.tobytes())


class SessionScoreTest(unittest.TestCase):
    def test_parse_log_extracts_bootstrap_and_pipeline(self):
        result = parse_log(LOG.splitlines(True))
        left = result["sides"]["L"]
        self.assertEqual(left["bootstrap"]["scans_started"], 1)
        self.assertEqual(left["bootstrap"]["baselines"], ["7,0,8,1"])
        self.assertEqual(len(left["bootstrap"]["locks"]), 1)
        self.assertAlmostEqual(left["bootstrap"]["locks"][0]["lock_fudge_us"], 16283.0)
        self.assertEqual(len(left["bootstrap"]["last_scan_steps"]["wide"]), 2)
        self.assertEqual(len(left["bootstrap"]["last_scan_steps"]["narrow"]), 2)
        self.assertAlmostEqual(left["bootstrap"]["locked_lit_fraction"]["median"], 0.75)
        self.assertEqual(left["candidates_by_camera"], {"0": 1, "2": 1})
        self.assertEqual(left["fused_by_camera_count"], {"2": 1})
        self.assertEqual(left["reacquisitions"], 1)
        self.assertEqual(left["imu_aligned_delta_deg"]["n"], 1)
        self.assertEqual(result["sides"]["R"]["bootstrap"]["scans_failed"], ["wide_peak_below_minimum"])
        self.assertEqual(result["counts"]["dropped_slow_samples"], 1)

    def test_parse_log_measures_clock_offset_creep(self):
        # Offset creeps 2.5 us per 16.7 ms frame for 1 s, then holds: the slew-limited startup seen on 24 Sep.
        lines = []
        for i in range(180):
            now = 1_000_000_000_000 + i * 16_683_000
            offset = 50_000_000 + min(i, 60) * 2_500
            lines.append(
                f" INFO [x] LED_SCHEDULE side=L now={now} raw_exposure={now - 20_000_000 + (3_000_000 if i == 90 else 0)} "
                f"period=16683000 controller_now={now + offset} period_id=20\n"
            )
        lines.append(" INFO [x] CLOCK_OFFSET side=L event=snap delta_us=5400.0\n")
        clock = parse_log(lines)["sides"]["L"]["clock_offset"]
        self.assertAlmostEqual(clock["creep_us"], 150.0)
        self.assertAlmostEqual(clock["range_us"], 150.0)
        self.assertAlmostEqual(clock["settled_s"], 20 * 0.016683, places=3)
        self.assertEqual(clock["snaps"], 1)
        jitter = parse_log(lines)["sides"]["L"]["exposure_jitter_us"]
        self.assertAlmostEqual(jitter["median"], 0.0, delta=20.0)
        self.assertAlmostEqual(jitter["max"], 3000.0, delta=20.0)

    def test_parse_log_flags_candidates_while_commanded_off(self):
        lines = []
        for i in range(120):  # 2 s of output reports: on for 1 s, then LED_ALL_OFF
            host = 1_000_000_000_000 + i * 16_683_000
            phase = 1 if i < 60 else 5
            lines.append(f"os_hid_iokit: PSSENSE_TIMING side=R force_ir=0 host_now_ns={host} phase={phase}\n")
            lines.append(f" INFO [x] CONSTELLATION_CANDIDATE side=R ts={host} cam=0 matched=5\n")
        off = parse_log(lines)["sides"]["R"]["lit_while_off"]
        # Off from report 60; candidates count once they are 400 ms (24 reports) into the off run.
        self.assertEqual(off["candidates"], 120 - 60 - 24)
        self.assertEqual(off["seconds"], [1])

    def test_score_session_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            (session / "run.log").write_text(LOG)

            rows = [HEADER]
            for i in range(40):
                t = 1_000_000_000 + i * 100_000_000
                state = 1 if i < 10 else 3
                tracked = i >= 10
                flags = 0x33 if tracked else 0x11
                jitter = 0.0005 * (i % 2)
                rows.append(
                    f"{t},left,0x{flags:x},{0.1 + jitter:.6f},0.2,0.3,0,0,0,1,{20_000_000 if tracked else -1},"
                    f"{i},2,{i},0,0,{state},16283.0,1000.0,1,{1 if tracked else 0}\n"
                )
            (session / "poses.csv").write_text("".join(rows))

            capture = session / "capture"
            (capture / "frames").mkdir(parents=True)
            lines = ["camera,source_sequence,exposure_monotonic_ns,exposure_vts_ns,width,height,stride,file\n"]
            for i, (t, lit) in enumerate([(1_500_000_000, False), (3_500_000_000, True)]):
                image = np.zeros((508, 512), dtype=np.uint8)
                if lit:
                    for k in range(4):
                        image[100 + 20 * k : 104 + 20 * k, 200:204] = 220
                rel = f"frames/camera0-sequence-{i:010d}.pgm"
                write_pgm(capture / rel, image)
                lines.append(f"0,{i},{t},{t},512,508,512,{rel}\n")
            (capture / "camera0.csv").write_text("".join(lines))

            result = score_session(session)
            left = result["poses"]["left"]
            self.assertAlmostEqual(left["position_tracked_fraction"], 0.75)
            self.assertAlmostEqual(left["first_lock_s"], 1.0)
            self.assertAlmostEqual(left["pose_age_ms"]["median"], 20.0)
            self.assertLess(left["static_jitter_mm"]["median"], 1.0)
            self.assertEqual(result["capture"]["camera0"]["wide"], {"frames": 1, "lit_frames": 0, "lit_fraction": 0.0})
            self.assertEqual(result["capture"]["camera0"]["locked"], {"frames": 1, "lit_frames": 1, "lit_fraction": 1.0})

            text = render_text(result)
            self.assertIn("first LED lock at 1.0 s", text)
            self.assertIn("lock: lit window", text)


if __name__ == "__main__":
    unittest.main()
