#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check clock-domain conversion used to separate GPU work and display delay."""
import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/macos/analyze-presentation-stalls.py"
SPEC = importlib.util.spec_from_file_location("stalls", SCRIPT)
STALLS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STALLS)


class PresentationClockTests(unittest.TestCase):
    def test_physical_clock_offset_does_not_use_delayed_handler(self):
        row = {"presented_monotonic_ns": "12000000000", "presented_time_host_s": "10",
               "presented_handler_ns": "12500000000"}
        self.assertEqual(STALLS.host_seconds_to_monotonic(9.9, row), 11_900_000_000)

    def test_not_presented_does_not_produce_gpu_mapping(self):
        self.assertIsNone(STALLS.host_seconds_to_monotonic(10, {"presented_monotonic_ns": "0", "presented_time_host_s": "0"}))

    def test_invalid_gpu_clock_is_excluded(self):
        row = {"presented_monotonic_ns": "12000000000", "presented_time_host_s": "10"}
        self.assertIsNone(STALLS.gpu_end_ns({"gpu_end_time_s": "50", "completion_handler_ns": "11901000000"}, row, 11_800_000_000))
        self.assertIsNone(STALLS.gpu_end_ns({"gpu_end_time_s": "9", "completion_handler_ns": "11901000000"}, row, 11_800_000_000))

    def test_valid_gpu_end_can_precede_delayed_completion_callback(self):
        row = {"presented_monotonic_ns": "12000000000", "presented_time_host_s": "10"}
        self.assertEqual(STALLS.gpu_end_ns({"gpu_end_time_s": "9.9", "completion_handler_ns": "12100000000"}, row, 11_800_000_000), 11_900_000_000)

    def test_empty_optional_trace_summary_is_explicit(self):
        self.assertEqual(STALLS.stats([])["n"], 0)
        self.assertIsNone(STALLS.stats([])["max"])


if __name__ == "__main__":
    unittest.main()
