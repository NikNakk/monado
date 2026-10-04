#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_tracking_geometry import (  # noqa: E402
    CameraModel,
    load_led_model,
    pose_matrix,
    project_model,
    refine_pose,
    rotation_distance,
    score_pose,
    unique_nearest,
)


class TrackingGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo_root = Path(__file__).resolve().parents[1]

    def test_parses_mirrored_controller_models_from_driver_source(self):
        left_positions, left_normals = load_led_model(self.repo_root, "left")
        right_positions, right_normals = load_led_model(self.repo_root, "right")

        self.assertEqual(left_positions.shape, (17, 3))
        np.testing.assert_allclose(right_positions[:, 0], -left_positions[:, 0])
        np.testing.assert_allclose(right_positions[:, 1:], left_positions[:, 1:])
        np.testing.assert_allclose(right_normals[:, 0], -left_normals[:, 0])
        np.testing.assert_allclose(right_normals[:, 1:], left_normals[:, 1:])

    def test_nearest_matching_is_one_to_one(self):
        projected = np.float64([[10, 10], [12, 10], [30, 30]])
        observed = np.float64([[10.5, 10], [30, 30]])
        matches = unique_nearest(projected, observed, np.ones(3, dtype=bool), 3.0)

        self.assertEqual([(led, blob) for _, led, blob in matches], [(2, 1), (0, 0)])

    def test_joint_refinement_reduces_synthetic_reprojection_error(self):
        positions, normals = load_led_model(self.repo_root, "left")
        normals = np.zeros_like(normals)  # All model points face the synthetic cameras.
        intrinsics = np.float64([[220, 0, 256], [0, 220, 254], [0, 0, 1]])
        cameras = []
        for x in (0.0, 0.08):
            camera_to_rig = np.eye(4)
            camera_to_rig[0, 3] = x
            cameras.append(CameraModel(intrinsics, np.zeros(4), np.eye(3), camera_to_rig))

        expected = pose_matrix(np.float64([[0.05], [-0.08], [0.03]]), np.float64([[0.02], [0.01], [0.7]]))
        observations = [project_model(expected, camera, positions, normals)[0] for camera in cameras]
        initial = pose_matrix(np.float64([[0.06], [-0.07], [0.02]]), np.float64([[0.025], [0.005], [0.71]]))
        before = score_pose(initial, cameras, positions, normals, observations)
        refined = refine_pose(initial, cameras, positions, normals, observations)
        after = score_pose(refined, cameras, positions, normals, observations)

        self.assertEqual(after["matched"], 34)
        self.assertLess(after["rms_px"], before["rms_px"])
        self.assertLess(after["rms_px"], 0.01)
        self.assertLess(rotation_distance(expected, refined), 1e-3)


if __name__ == "__main__":
    unittest.main()
