# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Synthetic check of the head_from_camera0 estimator: resting controllers seen from a moving head."""
import sys
import unittest
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import psvr2_head_from_camera0 as hfc  # noqa: E402


def make_segments(X_true, rng, noise_mm=1.0, noise_deg=0.2, rests=2, per_rest=150):
    segments = []
    for _ in range(rests):
        W = hfc.Pose(Rotation.random(random_state=rng.integers(1 << 30)), rng.normal(0, 0.3, 3) + [0, -0.3, -0.5])
        seg = []
        for _ in range(per_rest):
            # Head turns up to ~40 deg about all axes and moves a few cm.
            H = hfc.Pose(Rotation.from_rotvec(rng.normal(0, 0.35, 3)), rng.normal(0, 0.03, 3) + [0, 1.6, 0])
            C = (H * X_true).inv() * W
            C = hfc.Pose(Rotation.from_rotvec(rng.normal(0, np.radians(noise_deg), 3)) * C.R,
                         C.t + rng.normal(0, noise_mm / 1000, 3))
            seg.append((H, C))
        segments.append(seg)
    return segments


class HeadFromCamera0Test(unittest.TestCase):
    def test_recovers_transform(self):
        rng = np.random.default_rng(3)
        X_true = hfc.Pose(Rotation.from_euler("xyz", [-25, 8, 3], degrees=True), [0.035, -0.02, -0.06])
        segments = make_segments(X_true, rng)
        X = hfc.refine(segments, hfc.hand_eye_init(segments, rng))
        self.assertLess(np.linalg.norm(X.t - X_true.t) * 1000, 2.0)
        self.assertLess(np.degrees((X.R.inv() * X_true.R).magnitude()), 0.5)
        d, a = hfc.spread(segments, X)
        d0, a0 = hfc.spread(segments, hfc.identity())
        self.assertLess(np.median(d), 2.0)
        self.assertGreater(np.median(d0), 20.0)

    def test_needs_head_rotation(self):
        rng = np.random.default_rng(4)
        X_true = hfc.identity()
        W = hfc.Pose(Rotation.identity(), [0, 0, -0.5])
        seg = [(hfc.identity(), W) for _ in range(50)]
        with self.assertRaises(SystemExit):
            hfc.hand_eye_init([seg], rng)


if __name__ == "__main__":
    unittest.main()
