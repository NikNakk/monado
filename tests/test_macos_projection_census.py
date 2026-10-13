#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("census", Path(__file__).resolve().parents[1] / "scripts/macos/analyze-projection-census.py")
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


def frame(count=1, mask=0, kind=0, flags=0):
    shared = dict(compositor_id="0x1", event_ns="1000000000", frame_id="7", layer_count=str(count),
                  fast_path=str(int(mask == 0)), rejection_mask=str(mask), use_compute="1", passthrough="0",
                  peek="0", debug_mirror="0", debug_disable="0", layer_type="-1", flags="0", view_count="0",
                  chroma_curve="0", foveated_views="0", source_width="0", source_height="0")
    for axis in "rgba":
        shared[f"color_scale_{axis}"] = "1"
        shared[f"color_bias_{axis}"] = "0"
    rows = [dict(shared, layer_index="-1")]
    for index in range(count):
        rows.append(dict(shared, layer_index=str(index), layer_type=str(kind), flags=str(flags),
                         view_count="2", source_width="2800", source_height="2856"))
    return rows


class CensusTests(unittest.TestCase):
    def summary(self, rows, trim=0):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "composition.csv"
            with path.open("w", newline="") as file:
                writer = csv.DictWriter(file, fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
            return census.summarize(path, trim)

    def test_identity_flag_is_still_a_real_policy_blocker(self):
        result = self.summary(frame(mask=64, flags=16))
        self.assertEqual(result["rejections"], {"color_scale_bias": 1})
        self.assertEqual(result["frames_with_identity_color_scale_bias_flag"], 1)
        self.assertEqual(result["fast_path_frames"], 0)

    def test_multiple_projections_are_not_quads(self):
        result = self.summary(frame(count=2, mask=1))
        self.assertEqual(result["layer_patterns"], {"projection+projection": 1})

    def test_missing_layer_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Incomplete layer list"):
            self.summary(frame(count=2, mask=1)[:-1])

    def test_wrong_fast_path_status_is_rejected(self):
        rows = frame(mask=64, flags=16)
        for row in rows:row["fast_path"] = "1"
        with self.assertRaisesRegex(ValueError, "does not match policy"):
            self.summary(rows)

    def test_short_startup_trace_is_explicitly_unmeasured(self):
        result = self.summary(frame(count=0, mask=1), trim=2)
        self.assertEqual(result["status"], "insufficient_duration")
        self.assertEqual(result["steady_frames"], 0)


if __name__ == "__main__":
    unittest.main()
