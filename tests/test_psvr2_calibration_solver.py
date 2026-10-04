#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_calibration_solver import (  # noqa: E402
    Observation, board_points, build_rig, compose, fixed_board_residuals,
    handeye_once, invert, matrix_to_quat_xyzw, pair_extrinsic,
    quat_xyzw_to_matrix, refine_joint_board_pose, rotation_angle_deg,
    runtime_pose, transform,
)


def pose(rvec, t):
    R, _ = cv2.Rodrigues(np.asarray(rvec, np.float64))
    return transform(R, t)


class TransformTests(unittest.TestCase):
    def test_inversion_and_composition(self):
        T_A_B = pose([.2, -.1, .3], [1, 2, 3])
        np.testing.assert_allclose(compose(T_A_B, invert(T_A_B)), np.eye(4), atol=1e-12)

    def test_quaternion_matrix_roundtrip(self):
        q = np.array([.2, -.3, .1, .9]); q /= np.linalg.norm(q)
        recovered = matrix_to_quat_xyzw(quat_xyzw_to_matrix(q))
        self.assertAlmostEqual(abs(float(np.dot(q, recovered))), 1.0, places=12)

    def test_runtime_pose_matches_monado_order(self):
        raw = pose([.2, -.1, .3], [1., 2., 3.])
        Rz = quat_xyzw_to_matrix([0., 0., np.sqrt(.5), np.sqrt(.5)])
        tracker = runtime_pose(raw, "runtime_tracker")
        np.testing.assert_allclose(tracker[:3, :3], Rz @ raw[:3, :3], atol=1e-12)
        np.testing.assert_allclose(tracker[:3, 3], raw[:3, 3], atol=1e-12)
        head = runtime_pose(raw, "runtime_head")
        expected = tracker @ transform(t=[.000247, -.000273, .104826])
        np.testing.assert_allclose(head, expected, atol=1e-12)
        self.assertFalse(np.allclose(runtime_pose(raw, "rigid_z_correction")[:3, 3], raw[:3, 3]))

    def test_synthetic_stereo_and_rig_direction(self):
        corners = board_points(7, 5, .04)
        K = np.array([[300., 0., 320.], [0., 302., 318.], [0., 0., 1.]])
        intrinsics = {c: {"K": K.copy(), "D": np.zeros(4)} for c in range(4)}
        T_C1_C0 = pose([.01, -.02, .005], [-.08, .002, .001])
        observations, board_truth = {}, {}
        ids = np.arange(len(corners), dtype=np.int32)
        for i in range(8):
            T_C0_B = pose([.03*i, -.015*i, .01*i], [-.1 + .025*i, -.06 + .012*i, .8 + .02*i])
            board_truth[i] = T_C0_B
            for camera, T_C_B in ((0, T_C0_B), (1, T_C1_C0 @ T_C0_B)):
                rvec, _ = cv2.Rodrigues(T_C_B[:3, :3])
                image, _ = cv2.fisheye.projectPoints(
                    corners.reshape(1, -1, 3), rvec, T_C_B[:3, 3], K, np.zeros(4))
                observations[(0, i, camera)] = Observation(0, i, i, camera, ids, image.reshape(-1, 2))
        edge = pair_extrinsic(0, 1, observations, corners, intrinsics, (640, 640), 6)
        self.assertIsNotNone(edge)
        np.testing.assert_allclose(edge["T_camera_b_camera_a"], T_C1_C0, atol=2e-4)
        pairwise = [edge]
        for camera, offset in ((2, -.07), (3, .11)):
            pairwise.append({"camera_a": 0, "camera_b": camera,
                             "T_camera_b_camera_a": transform(t=[offset, 0, 0]),
                             "rms_px": .1, "frames": 8})
        T_R_C, _closures = build_rig(pairwise)
        np.testing.assert_allclose(T_R_C[1], invert(T_C1_C0), atol=2e-4)
        frame = 3
        candidates = [(c, observations[(0, frame, c)], board_truth[frame], 0.) for c in (0, 1)]
        initial = pose([.02, -.01, .015], [.01, -.005, .012]) @ board_truth[frame]
        refined, residual = refine_joint_board_pose(
            initial, candidates, corners, intrinsics, T_R_C)
        self.assertLess(rotation_angle_deg(refined[:3, :3].T @ board_truth[frame][:3, :3]), .01)
        self.assertLess(np.linalg.norm(refined[:3, 3] - board_truth[frame][:3, 3]), 1e-4)
        self.assertLess(np.sqrt(np.mean(residual**2)), .01)

    def test_handeye_direction_recovers_tracker_from_rig(self):
        # Fixed board equation: T_S_T(i) T_T_R T_R_B(i) = T_S_B.
        T_T_R = pose([.12, -.08, .05], [.04, -.03, .09])
        T_S_B = pose([-.3, .1, .2], [1.0, .2, 1.6])
        slam, board = [], []
        for i in range(18):
            T_S_T = pose([.04*i, -.025*i + .002*i*i, .017*i],
                         [.08*np.sin(i*.4), .05*np.cos(i*.31), .015*i])
            T_R_B = invert(T_T_R) @ invert(T_S_T) @ T_S_B
            slam.append(T_S_T); board.append(T_R_B)
        recovered = handeye_once(slam, board)
        np.testing.assert_allclose(recovered, T_T_R, atol=2e-6)
        _, rr, tr, _ = fixed_board_residuals(slam, board, recovered)
        self.assertLess(rr["max"], 1e-4); self.assertLess(tr["max"], 1e-6)

        wrong = handeye_once([invert(x) for x in slam], board)
        _, _rr, wrong_tr, _ = fixed_board_residuals(slam, board, wrong)
        self.assertGreater(wrong_tr["median"], .01)

    def test_handeye_keeps_independent_slam_sessions_separate(self):
        T_T_R = pose([.12, -.08, .05], [.04, -.03, .09])
        slam, board, groups = [], [], []
        for group, T_S_B in enumerate((pose([-.3,.1,.2],[1,.2,1.6]),
                                        pose([.2,.25,-.1],[-2,1,3]))):
            for i in range(12):
                T_S_T = pose([.05*i, -.02*i + .003*i*i, .019*i],
                             [.06*np.sin(i*.5), .04*np.cos(i*.3), .02*i])
                slam.append(T_S_T); board.append(invert(T_T_R) @ invert(T_S_T) @ T_S_B)
                groups.append(group)
        recovered = handeye_once(slam, board, groups=groups)
        np.testing.assert_allclose(recovered, T_T_R, atol=2e-6)
        _, rr, tr, centers = fixed_board_residuals(slam, board, recovered, groups)
        self.assertEqual(set(centers), {"0", "1"})
        self.assertLess(rr["max"], 1e-4); self.assertLess(tr["max"], 1e-6)


if __name__ == "__main__":
    unittest.main()
