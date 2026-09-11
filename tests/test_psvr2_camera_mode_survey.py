#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import struct
import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_camera_mode_survey import CameraPacketFramer, parse_vi_header  # noqa: E402
from psvr2_tracking_mask_analyze import centroid_match_fraction, compact_bright_centroids, segment_for_time  # noqa: E402


def camera_packet(size, sequence, camera_set=8, width=4, height=3):
    header = bytearray(256)
    struct.pack_into(
        "<2sHIIIHHHHHH",
        header,
        0,
        b"VI",
        0x200,
        size,
        123456 + sequence,
        sequence,
        camera_set,
        height,
        height,
        width,
        1,
        4,
    )
    return bytes(header) + bytes((i + sequence) % 256 for i in range(size - len(header)))


class CameraPacketFramerTests(unittest.TestCase):
    def test_mask_segment_assignment_excludes_transition_edges(self):
        segments = [{"start_monotonic_ns": "1000", "end_monotonic_ns": "2000", "label": "bit_00"}]
        self.assertIsNone(segment_for_time(1099, segments, 100))
        self.assertEqual(segment_for_time(1100, segments, 100)["label"], "bit_00")
        self.assertEqual(segment_for_time(1900, segments, 100)["label"], "bit_00")
        self.assertIsNone(segment_for_time(1901, segments, 100))

    def test_mask_analyzer_finds_compact_source_and_rejects_large_region(self):
        difference = np.zeros((80, 80), dtype=np.uint8)
        difference[10:13, 20:23] = 100
        difference[30:75, 30:75] = 100
        centroids = compact_bright_centroids(difference, 40)
        self.assertEqual(len(centroids), 1)
        np.testing.assert_allclose(centroids[0], [21.0, 11.0])

    def test_centroid_matching_recognizes_shared_constellation(self):
        points = [[10.0, 10.0], [20.0, 20.0], [100.0, 100.0]]
        reference = [[20.5, 19.5], [9.5, 10.5], [200.0, 200.0]]
        self.assertAlmostEqual(centroid_match_fraction(points, reference), 2 / 3)

    def test_fragmented_and_coalesced_packets(self):
        first = camera_packet(320, 10)
        second = camera_packet(384, 11, camera_set=9)
        framer = CameraPacketFramer()

        self.assertEqual(framer.feed(b"stale" + first[:1]), [])
        self.assertEqual(framer.feed(first[1:29]), [])
        packets = framer.feed(first[29:] + second[:100])
        self.assertEqual(packets, [first])
        self.assertEqual(framer.feed(second[100:]), [second])
        self.assertEqual(framer.discarded_bytes, 5)
        self.assertEqual(framer.invalid_headers, 0)
        self.assertEqual(len(framer.buffer), 0)

    def test_skips_incidental_invalid_signature(self):
        packet = camera_packet(320, 20)
        invalid = b"VI" + bytes(40)
        framer = CameraPacketFramer()
        self.assertEqual(framer.feed(invalid + packet), [packet])
        self.assertEqual(framer.invalid_headers, 1)
        self.assertEqual(framer.discarded_bytes, len(invalid))
        self.assertEqual(parse_vi_header(packet)["header_packet_size"], len(packet))

    def test_preserves_split_signature_and_reset_discards_partial_packet(self):
        packet = camera_packet(320, 30)
        framer = CameraPacketFramer()
        self.assertEqual(framer.feed(b"junkV"), [])
        self.assertEqual(framer.feed(packet[1:100]), [])
        self.assertEqual(framer.discarded_bytes, 4)
        framer.reset()
        self.assertEqual(len(framer.buffer), 0)
        self.assertEqual(framer.discarded_bytes, 104)


if __name__ == "__main__":
    unittest.main()
