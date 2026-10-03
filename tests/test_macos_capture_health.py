#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Capture must reject inactive sensors and trace writes inside the window."""
import importlib.util
import json
import pathlib
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("capture", pathlib.Path(__file__).resolve().parents[1] / "scripts/macos/capture-tracking-freshness.py")
CAPTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAPTURE)


class CaptureHealthTests(unittest.TestCase):
    def test_current_sensor_streams_are_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            (out / "server").mkdir()
            (out / "server/monado_psvr2_7_imu.csv").write_text("host_callback_ns\n990000000\n")
            (out / "server/monado_psvr2_7_slam.csv").write_text("host_received_ns\n980000000\n")
            CAPTURE.check_sensor_health(out, 7, 1_000_000_000)
            self.assertTrue((out / "sensor-health.json").exists())

    def test_empty_or_stale_slam_prevents_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            (out / "server").mkdir()
            (out / "server/monado_psvr2_7_imu.csv").write_text("host_callback_ns\n990000000\n")
            slam = out / "server/monado_psvr2_7_slam.csv"
            for text in ["host_received_ns\n", "host_received_ns\n100000000\n"]:
                slam.write_text(text)
                with self.assertRaises(RuntimeError):
                    CAPTURE.check_sensor_health(out, 7, 1_000_000_000)
                health = json.loads((out / "sensor-health.json").read_text())
                self.assertTrue(health['imu']['active'])
                self.assertFalse(health['slam']['active'])

    def test_both_missing_streams_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory)
            (out / "server").mkdir()
            (out / "server/monado_psvr2_7_imu.csv").write_text("host_callback_ns\n")
            (out / "server/monado_psvr2_7_slam.csv").write_text("host_received_ns\n")
            with self.assertRaisesRegex(RuntimeError, "IMU and SLAM"):
                CAPTURE.check_sensor_health(out, 7, 1_000_000_000)

    def test_warmup_bytes_allowed_but_window_growth_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "trace.csv"
            path.write_text("warmup\n")
            baseline = {str(path): path.stat().st_size}
            CAPTURE.check_buffered([path], baseline)
            path.write_text("warmup\nnew row\n")
            with self.assertRaises(RuntimeError):
                CAPTURE.check_buffered([path], baseline)


if __name__ == "__main__":
    unittest.main()
