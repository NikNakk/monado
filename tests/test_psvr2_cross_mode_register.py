#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_cross_mode_register import (  # noqa: E402
    analyze_mode12_bridge,
    estimate_scaled_centroid_matches,
    hull_fraction,
    match_scaled_centroids,
)


class TrackingReadoutMappingTests(unittest.TestCase):
    def test_hull_fraction_uses_image_area(self):
        points = np.float32([[0, 0], [10, 0], [10, 10], [0, 10]])
        self.assertAlmostEqual(hull_fraction(points, 20, 20), 0.25)

    def test_visible_tracking_affine_remains_overlap_only(self):
        grid = np.float32([[20, 20], [80, 20], [140, 20], [20, 80], [80, 80], [140, 80]])

        def fake_visit(_root, _visit, camera_set, _example):
            base = 0 if camera_set == 8 else 10
            return {camera: np.uint8([[base + camera]]) for camera in range(4)}

        def fake_matches(source, target, ratio=0.72):
            del ratio
            source_plane = int(source[0, 0])
            target_plane = int(target[0, 0]) - 10
            if source_plane != target_plane:
                return grid[:1], grid[:1]
            offset = np.float32([10 + source_plane, 30 + source_plane])
            return grid.copy(), 0.5 * grid + offset

        captures = [(Path(f"capture-{i}"), {"capture_plan": [12]}) for i in range(3)]
        with patch("psvr2_cross_mode_register.mode12_visit", side_effect=fake_visit), patch(
            "psvr2_cross_mode_register.sift_matches", side_effect=fake_matches
        ):
            result = analyze_mode12_bridge(captures)

        self.assertEqual(result["status"], "estimated_overlap_only")
        self.assertTrue(result["identity_order_supported"])
        self.assertFalse(result["runtime_usable"])
        self.assertEqual(len(result["affine_bootstrap_models"]), 4)
        np.testing.assert_allclose(
            result["affine_bootstrap_models"][2]["matrix_visible_to_tracking"],
            [[0.5, 0.0, 12.0], [0.0, 0.5, 32.0]],
            atol=1e-5,
        )

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
