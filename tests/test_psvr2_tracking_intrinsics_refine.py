#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_tracking_affine_refine import TrainingView  # noqa: E402
from psvr2_tracking_geometry import CameraModel, project_model  # noqa: E402
from psvr2_tracking_intrinsics_refine import (  # noqa: E402
    initial_intrinsics,
    initial_parameters,
    residuals,
)


class TrackingIntrinsicsRefinementTests(unittest.TestCase):
    def test_initial_intrinsics_remove_affine_axis_rotation(self):
        K = np.float64([[200, 0, 300], [0, 180, 250], [0, 0, 1]])
        H = np.float64([[0.5, 0.1, 20], [-0.05, 0.6, 30], [0, 0, 1]])
        camera = CameraModel(K, np.zeros(4), H, np.eye(4))
        mapped = H @ K

        result = initial_intrinsics(camera)

        np.testing.assert_allclose(result[:2], [np.hypot(mapped[0, 0], mapped[0, 1]), np.hypot(mapped[1, 0], mapped[1, 1])])
        np.testing.assert_allclose(result[2:], mapped[:2, 2])

    def test_synthetic_native_camera_has_zero_residual(self):
        K = np.float64([[200, 0, 256], [0, 200, 254], [0, 0, 1]])
        cameras = [CameraModel(K, np.zeros(4), np.eye(3), np.eye(4)) for _ in range(4)]
        pose = np.eye(4)
        pose[2, 3] = 0.7
        positions = np.float64([[0.01, -0.02, 0.0]])
        normals = np.zeros_like(positions)
        observed = project_model(pose, cameras[2], positions, normals)[0][0]
        view = TrainingView(
            Path("synthetic.json"),
            pose,
            [[], [], [{"led_id": 0, "observed_mode4_px": observed.tolist()}], []],
            "accepted",
        )
        parameters = initial_parameters(cameras[2], [view])

        result, data_count = residuals(parameters, cameras, 2, positions, normals, [view], parameters)

        self.assertEqual(data_count, 2)
        np.testing.assert_allclose(result, 0.0, atol=1e-9)


if __name__ == "__main__":
    unittest.main()
