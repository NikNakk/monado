#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_tracking_affine_refine import (  # noqa: E402
    TrainingView,
    fixed_residuals,
    initial_parameters,
    required_camera_validation,
    robust_cost,
)
from psvr2_tracking_geometry import CameraModel, project_model  # noqa: E402


class TrackingAffineRefinementTests(unittest.TestCase):
    def test_required_camera_must_contribute_three_matches(self):
        diagnostics = {
            "per_camera": [
                {"camera": 0, "matches": [1, 2, 3]},
                {"camera": 1, "matches": []},
                {"camera": 2, "matches": [1, 2]},
                {"camera": 3, "matches": [1, 2, 3, 4]},
            ]
        }
        self.assertEqual(required_camera_validation(diagnostics, [3])["status"], "passed")
        self.assertEqual(required_camera_validation(diagnostics, [2])["status"], "failed")

    def test_robust_cost_limits_outlier_influence(self):
        residuals = np.float64([1.0, 10.0, 2.0])
        self.assertAlmostEqual(robust_cost(residuals, 2), 34.5)

    def test_fixed_residuals_are_zero_for_synthetic_observation(self):
        intrinsics = np.float64([[200, 0, 256], [0, 200, 254], [0, 0, 1]])
        cameras = [CameraModel(intrinsics, np.zeros(4), np.eye(3), np.eye(4)) for _ in range(4)]
        pose = np.eye(4)
        pose[2, 3] = 0.7
        positions = np.float64([[0.01, -0.02, 0.0]])
        normals = np.zeros_like(positions)
        observed = project_model(pose, cameras[0], positions, normals)[0][0]
        matches = [[{"led_id": 0, "observed_mode4_px": observed.tolist()}], [], [], []]
        view = TrainingView(Path("synthetic.json"), pose, matches, "accepted")
        parameters = initial_parameters(cameras, [view])

        residuals, data_count = fixed_residuals(
            parameters, cameras, positions, normals, [view], parameters[:24]
        )

        self.assertEqual(data_count, 2)
        np.testing.assert_allclose(residuals, 0.0, atol=1e-9)


if __name__ == "__main__":
    unittest.main()
