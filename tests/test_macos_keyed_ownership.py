#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("ownership", Path(__file__).resolve().parents[1] / "scripts/macos/analyze-steamvr-keyed-ownership.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

TRACE = """call=WaitGetPoses pid=3 tid=4 begin_us=1000 end_us=6000
call=WaitGetPoses pid=3 tid=4 begin_us=2000 end_us=5000
call=Submit pid=3 tid=4 begin_us=7000 end_us=9000
"""
EVENT = "mwxr-keyed tick_us=9000 handle=0x1 op=release pid=3 owner_pid=3 held_us=8000 status=0 fence=7"


class OwnershipTests(unittest.TestCase):
    def test_nested_calls_and_duplicate_history_do_not_double_count(self):
        r = m.analyze(TRACE, EVENT + "\n" + EVENT)
        self.assertEqual(r["releases"]["samples"], 1)
        self.assertEqual(r["releases"]["median_wait_get_poses_ms"], 5)
        self.assertEqual(r["releases"]["median_wait_fraction"], 0.625)

    def test_timeout_uses_owner_not_waiter(self):
        e = EVENT.replace("op=release pid=3", "op=acquire pid=9").replace("status=0 ", "status=0x102 ")
        r = m.analyze(TRACE, e)
        self.assertEqual(r["timeouts_with_traced_owner"]["samples"], 1)
        self.assertEqual(r["releases"]["samples"], 0)

    def test_partial_and_flush_overlapped_intervals_are_excluded(self):
        self.assertEqual(m.analyze(TRACE, EVENT.replace("held_us=8000", "held_us=9000"))["releases"]["samples"], 0)
        r = m.analyze(TRACE + "trace_flush pid=3 begin_us=3000 end_us=3100\n", EVENT)
        self.assertEqual(r["excluded_flush_intervals"], 1)
        self.assertEqual(r["releases"]["samples"], 0)


if __name__ == "__main__":
    unittest.main()
