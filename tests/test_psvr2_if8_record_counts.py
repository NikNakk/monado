import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from psvr2_if8_record_counts import candidate_counts


class CandidatePartitionTests(unittest.TestCase):
    def test_independent_population_in_nonadjacent_records(self):
        data = bytearray(36944)
        start = 64 + 2*9220
        data[start:start+4] = (2).to_bytes(4, 'little')
        data[start+4+3*36] = 1
        data[start+4+255*36+35] = 1
        self.assertEqual(candidate_counts(data), ([0, 0, 2, 0], [0, 0, 2, 0]))

    def test_count_disagreement_is_visible(self):
        data = bytearray(36944)
        data[64] = 1
        declared, nonzero = candidate_counts(data)
        self.assertNotEqual(declared, nonzero)

    def test_partial_read_is_not_silently_a_packet(self):
        with self.assertRaises(ValueError):
            candidate_counts(bytes(36943))


if __name__ == '__main__':
    unittest.main()
