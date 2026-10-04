#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_tracking_charuco_native_origin import build  # noqa: E402


def direct_fixture():
    rig = {}
    for i in range(4):
        t = np.eye(4)
        t[0, 3] = 0.08 * (i % 2)
        t[1, 3] = 0.06 * (i // 2)
        rig[str(i)] = t.tolist()
    cameras = {
        str(i): {
            "calibration": {
                "resolution": {"width": 508, "height": 508},
                "model": "fisheye_equidistant4",
                "intrinsics": {"fx": 189.0, "fy": 189.0, "cx": 254.0, "cy": 254.0},
                "distortion": {"k1": 0.02, "k2": 0.0, "k3": 0.0, "k4": 0.0},
            },
            "fit": {"rms_px": 0.4},
            "leave_one_pose_out": None,
        }
        for i in range(4)
    }
    return {
        "format": "psvr2-mode4-charuco-direct-calibration-v1",
        "capture_root": "x",
        "capture_images": {"image_count": 1},
        "board": {"squares": [7, 5]},
        "cameras": cameras,
        "native_relative_rig": {"transforms_T_camera0_camera": rig, "fit": {"rms_px": 0.5, "median_px": 0.3, "p95_px": 0.9}},
    }


class NativeOriginTest(unittest.TestCase):
    def test_build_produces_cli_loadable_shape(self):
        template = {
            "format": "psvr2-mode4-constellation-calibration-v1",
            "base_calibration": {"path": "/tmp/lost.json"},
            "cameras": [{"camera": i} for i in range(4)],
            "controller_models": {"left": {"led_count": 17}, "right": {"led_count": 17}},
        }
        with tempfile.TemporaryDirectory() as tmp:
            paths = {"direct": Path(tmp) / "d.json", "provisional": Path(tmp) / "p.json"}
            for p in paths.values():
                p.write_text("{}")
            result = build(direct_fixture(), template, paths)
        self.assertEqual(result["format"], "psvr2-mode4-constellation-calibration-v1")
        self.assertNotIn("base_calibration", result)
        self.assertFalse(result["runtime_usable"])
        self.assertEqual([c["camera"] for c in result["cameras"]], [0, 1, 2, 3])
        cam3 = result["cameras"][3]
        self.assertEqual(cam3["calibration"]["resolution"], {"width": 512, "height": 508})
        # OpenCV +y down becomes XRT +y up.
        self.assertAlmostEqual(cam3["pose_in_tracking_origin_xrt"]["position"]["x"], 0.08)
        self.assertAlmostEqual(cam3["pose_in_tracking_origin_xrt"]["position"]["y"], -0.06)
        self.assertAlmostEqual(result["candidate_provenance"]["camera_centre_distances_mm"]["0-1"], 80.0)


if __name__ == "__main__":
    unittest.main()
