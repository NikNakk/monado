#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_cross_mode_register import estimate_scaled_centroid_matches, match_scaled_centroids  # noqa: E402


class TrackingReadoutMappingTests(unittest.TestCase):
    def test_matches_standard_two_x_pixel_centres_uniquely(self):
        mode12 = np.float32([[10.0, 20.0], [30.25, 42.75], [90.5, 100.25]])
        mode4 = 2.0 * mode12 + 0.5
        mode4 = np.vstack((mode4[[2, 0, 1]], np.float32([[400.0, 400.0]])))

        source, target = match_scaled_centroids(mode12, mode4)

        self.assertEqual(len(source), 3)
        np.testing.assert_allclose(target, 2.0 * source + 0.5)

    def test_rejects_dimension_only_mapping_with_wrong_pixel_centres(self):
        mode12 = np.float32([[10.0, 20.0], [30.0, 40.0]])
        mode4 = 2.0 * mode12 + np.float32([4.0, 0.0])

        source, target = match_scaled_centroids(mode12, mode4)

        self.assertEqual(len(source), 0)
        self.assertEqual(len(target), 0)

    def test_estimates_pixel_centre_offset_without_assuming_it(self):
        mode12 = np.float32([[10.0, 20.0], [30.25, 42.75], [90.5, 100.25]])
        mode4 = 2.0 * mode12 + 0.5

        source, target, estimated_offset = estimate_scaled_centroid_matches(mode12, mode4)

        self.assertEqual(len(source), 3)
        np.testing.assert_allclose(target, 2.0 * source + 0.5)
        np.testing.assert_allclose(estimated_offset, [0.5, 0.5])


if __name__ == "__main__":
    unittest.main()
