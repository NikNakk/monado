# SPDX-License-Identifier: BSL-1.0
"""Regression checks for interpretation of missing and differently clocked feedback."""
import csv
import pathlib
import tempfile
import unittest

from analyze import summarize


class AnalysisTests(unittest.TestCase):
    def make_trace(self, directory, mode="native", result="complete"):
        path = pathlib.Path(directory) / "trace.csv"
        pathlib.Path(str(path) + ".meta.txt").write_text(
            f"mode={mode}\npolicy=immediate\nwidth=4000\nheight=2040\nburn=0\n"
            f"nominal_refresh_hz=100\nresult={result}\n")
        rows = []
        for i in range(1, 5):
            r = {key: 0 for key in (
                "acquire_begin_s", "acquire_end_s", "reuse_wait_ms", "submit_begin_s", "submit_end_s",
                "present_begin_s", "present_end_s", "render_gpu_ms", "post_render_gpu_ms", "metal_command_gpu_ms")}
            r.update(frame_id=i, tick_id=i, status="presented", generated_s=1 + i * .01,
                     target_s=1 + i * .01, presented_s=1.02 + i * .01, feedback_received_s=1.03 + i * .01)
            rows.append(r)
        # Two frames have no trustworthy display timestamp, for different reasons.
        rows[1].update(status="superseded", presented_s=0)
        rows[2].update(status="submitted", presented_s=0)
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0])
            writer.writeheader()
            writer.writerows(rows)
        return path

    def test_missing_feedback_is_not_treated_as_confirmed_drops(self):
        with tempfile.TemporaryDirectory() as directory:
            result = summarize(self.make_trace(directory), 0, None, False)
            self.assertEqual(result["feedback_coverage_percent"], 50)
            self.assertEqual(result["statuses"], {"presented": 2, "superseded": 1, "submitted": 1})
            self.assertEqual(result["estimated_unobserved_refreshes"], 2)
            self.assertEqual(result["frame_age_ms"]["count"], 2)
            self.assertAlmostEqual(result["frame_age_ms"]["p50"], 20)

    def test_wsi_age_requires_explicit_clock_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_trace(directory, "wsi-direct")
            self.assertNotIn("frame_age_ms", summarize(path, 0, None, False))
            self.assertIn("frame_age_ms", summarize(path, 0, None, True))

    def test_implausible_clock_still_rejected_after_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_trace(directory, "wsi-copy")
            text = path.read_text().replace("1.03,1.04", "101.03,1.04")
            path.write_text(text)
            self.assertNotIn("frame_age_ms", summarize(path, 0, None, True))

    def test_failed_capture_and_empty_warmup_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_trace(directory, result="failed")
            with self.assertRaisesRegex(ValueError, "failed capture"):
                summarize(path, 0, None, False)
            path = self.make_trace(directory)
            with self.assertRaisesRegex(ValueError, "no frames"):
                summarize(path, 4, None, False)


if __name__ == "__main__":
    unittest.main()
