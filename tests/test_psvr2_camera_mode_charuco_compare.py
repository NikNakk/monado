# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from psvr2_camera_mode_charuco_compare import fit_affine


class CorrespondenceTests(unittest.TestCase):
    def test_native_coordinate_scale_reflection_and_offset(self):
        x = np.array([[0, 0], [10, 0], [0, 20], [10, 20], [4, 7], [9, 3]], float)
        expected = np.array([[2, 0], [0, -2], [.5, 1015.5]])
        y = np.column_stack([x, np.ones(len(x))]) @ expected
        result, errors = fit_affine(x, y)
        np.testing.assert_allclose(result, expected, atol=1e-10)
        self.assertLess(errors.max(), 1e-10)

    def test_collinear_board_observations_rejected(self):
        x = np.column_stack([np.arange(8), np.arange(8)])
        with self.assertRaises(ValueError):
            fit_affine(x, x * 2)


if __name__ == '__main__':
    unittest.main()
