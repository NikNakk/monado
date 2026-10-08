import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from pssense_stationary_spots import match_spots


class ImageSpotTests(unittest.TestCase):
    def test_one_component_cannot_fill_two_spot_locations(self):
        matched = match_spots([[0, 0], [3, 0]], [[1, 0]])
        self.assertEqual(int(matched.sum()), 1)

    def test_reordered_components_and_outlier(self):
        matched = match_spots([[0, 0], [10, 0], [20, 0]], [[10.2, 0], [.3, 0], [80, 0]])
        self.assertEqual(matched.tolist(), [True, True, False])

    def test_dark_control(self):
        self.assertEqual(match_spots([[0, 0]], []).tolist(), [False])


if __name__ == '__main__':
    unittest.main()
