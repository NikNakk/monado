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
    compose, fixed_board_residuals, handeye_once, invert,
    matrix_to_quat_xyzw, quat_xyzw_to_matrix, transform,
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
