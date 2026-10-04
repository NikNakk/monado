#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from psvr2_tracking_calibration_assemble import assemble, xrt_pose_from_opencv_transform  # noqa: E402
from psvr2_tracking_geometry import sha256_file  # noqa: E402


class TrackingCalibrationAssembleTests(unittest.TestCase):
    def make_inputs(self, root):
        calibration = root / "calibration.json"
        cameras = {}
        for camera in range(4):
            transform = np.eye(4)
            transform[0, 3] = camera * 0.01
            cameras[f"camera{camera}"] = {
                "D": [0.1, 0.2, 0.3, 0.4],
                "transform_to_rig_T_rig_camera": transform.tolist(),
            }
        calibration.write_text(json.dumps({"schema_version": 3, "headset_serial": "test", "visible_cameras": cameras}))
        digest = sha256_file(calibration)
        paths = []
        for camera in range(4):
            path = root / f"camera{camera}.json"
            path.write_text(json.dumps({
                "format": "psvr2-tracking-intrinsics-refinement-v1", "camera": camera,
                "calibration": {"sha256": digest}, "hand": "left",
                "refined_K": [[180, 0, 256], [0, 181, 254], [0, 0, 1]],
                "fixed_D": [0.1, 0.2, 0.3, 0.4],
                "optimization": {"final_data_rms_px": 2.5},
                "validation": {"diagnostics": {"status": "bootstrap_accepted"},
                               "required_camera_validation": {"status": "passed"}},
            }))
            paths.append(path)
        return calibration, paths

    def test_assembles_four_monado_shaped_cameras(self):
        with tempfile.TemporaryDirectory() as directory:
            calibration, paths = self.make_inputs(Path(directory))
            result = assemble(calibration, paths, Path(__file__).resolve().parents[1])
        self.assertFalse(result["runtime_usable"])
        self.assertEqual([entry["camera"] for entry in result["cameras"]], [0, 1, 2, 3])
        self.assertEqual(result["cameras"][0]["calibration"]["model"], "fisheye_equidistant4")
        self.assertEqual(result["controller_models"]["right"]["led_count"], 17)

    def test_rejects_wrong_calibration_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            calibration, paths = self.make_inputs(Path(directory))
            data = json.loads(paths[2].read_text())
            data["calibration"]["sha256"] = "0" * 64
            paths[2].write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                assemble(calibration, paths, Path(__file__).resolve().parents[1])

    def test_opencv_to_xrt_pose_flips_yz_translation(self):
        transform = np.eye(4)
        transform[:3, 3] = [1, 2, 3]
        pose = xrt_pose_from_opencv_transform(transform)
        self.assertEqual(pose["position"], {"x": 1.0, "y": -2.0, "z": -3.0})
        self.assertAlmostEqual(pose["orientation"]["w"], 1.0)


if __name__ == "__main__":
    unittest.main()
