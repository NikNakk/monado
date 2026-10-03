#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Method checks for delivery delay that receipt-based clock mapping can hide."""
import importlib.util
import math
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/macos/analyze-tracking-freshness.py"
SPEC = importlib.util.spec_from_file_location("freshness", SCRIPT)
FRESHNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FRESHNESS)


class DeliveryMethodTests(unittest.TestCase):
    def test_renderer_pose_comparison_handles_sign_scale_and_invalid_poses(self):
        distance = FRESHNESS.quaternion_distance_deg
        self.assertAlmostEqual(distance([1, 0, 0, 0], [-2, 0, 0, 0]), 0)
        self.assertAlmostEqual(distance([1, 0, 0, 0], [math.sqrt(.5), 0, math.sqrt(.5), 0]), 90)
        self.assertIsNone(distance([0, 0, 0, 0], [1, 0, 0, 0]))
        self.assertIsNone(distance([float('nan'), 0, 0, 0], [1, 0, 0, 0]))

    def test_session_floor_retains_sustained_delay_hidden_by_short_window(self):
        device = [i * 1_000_000_000 for i in range(11)]
        offset = 100_000_000_000
        host = [t + offset + (20_000_000 if 2 <= i <= 8 else 0) for i, t in enumerate(device)]
        floor, session_floor, rolling, session = FRESHNESS.delay_series(host, device, 5_000_000_000)
        self.assertEqual(session_floor, offset)
        self.assertEqual(rolling[3], 20_000_000)
        self.assertEqual(rolling[8], 0)  # Short envelope has adapted to the stall.
        self.assertEqual(session[8], 20_000_000)
        self.assertEqual(floor[9], offset)  # Prompt delivery returns.

    def test_absorbed_delay_does_not_make_raw_source_fresh(self):
        vts, floor, delay = 1_000_000_000, 100_000_000_000, 20_000_000
        driver_offset = floor + delay
        query = vts + driver_offset + 1_000_000
        mapped_age = (query - (vts + driver_offset)) / 1e6
        self.assertEqual(mapped_age, 1)
        self.assertEqual(FRESHNESS.raw_age(query, vts, floor), 21)

    def test_distinct_equal_timestamps_are_allowed_but_clock_resets_rejected(self):
        FRESHNESS.delay_series([100, 100, 101], [10, 10, 11], 10)
        with self.assertRaises(ValueError):
            FRESHNESS.delay_series([100, 101], [11, 10], 10)
        with self.assertRaises(ValueError):
            FRESHNESS.delay_series([101, 100], [10, 11], 10)

    def test_delivery_uses_newest_sample_per_actual_callback(self):
        # Estimated sample times regress across overlapping reconstructed batches.
        rows = [(100, 110, 10, 100, 0), (110, 110, 20, 110, 0),
                (105, 115, 21, 111, 0), (115, 115, 30, 120, 0)]
        self.assertEqual(FRESHNESS.callback_batches(rows), [rows[1], rows[3]])

    def test_no_future_reference_is_used_by_trailing_lookup(self):
        self.assertIsNone(FRESHNESS.last_at([10, 20], [100, 200], 9))
        self.assertEqual(FRESHNESS.last_at([10, 20], [100, 200], 10), 100)
        self.assertEqual(FRESHNESS.last_at([10, 20], [100, 200], 19), 100)
        self.assertEqual(FRESHNESS.last_at([10, 20], [100, 200], 20), 200)


if __name__ == "__main__":
    unittest.main()
