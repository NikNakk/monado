#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import struct
import sys
import unittest
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_camera_mode_survey import CameraPacketFramer, parse_vi_header, decode_bc4_unorm, compact_decoded_images, write_pgm  # noqa: E402
from psvr2_tracking_mask_analyze import (  # noqa: E402
    centroid_match_fraction,
    classify_led_blink_semantics,
    compact_bright_centroids,
    segment_for_time,
)


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
    def test_lossless_png_preserves_every_original_dn(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            pixels = bytes(range(256)) * 16
            write_pgm(directory / 'frame.pgm', 64, 64, pixels)
            names = compact_decoded_images(directory, ['frame.pgm'])
            self.assertEqual(names, ['frame.png'])
            with Image.open(directory / names[0]) as image:
                self.assertEqual(image.mode, 'L')
                self.assertEqual(image.tobytes(), pixels)

    def test_full_size_decoder_matches_scalar_blocks_with_partial_edges(self):
        width, height = 101, 53
        blocks = []
        expected = np.zeros((56, 104), dtype=np.uint8)
        for by in range(14):
            for bx in range(26):
                a, b = (210, 0) if (bx + by) % 2 else (0, 200)
                indices = sum(((i + bx + by) % 8) << (3 * i) for i in range(16))
                block = bytes([a, b]) + indices.to_bytes(6, "little")
                blocks.append(block)
                expected[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = np.frombuffer(
                    decode_bc4_unorm(block, 4, 4), dtype=np.uint8).reshape(4, 4)
        self.assertEqual(decode_bc4_unorm(b"".join(blocks), width, height),
                         expected[:height, :width].tobytes())

    def test_bc4_indices_are_texels_not_eight_camera_lanes(self):
        indices = sum((i % 8) << (3 * i) for i in range(16))
        block = bytes([210, 0]) + indices.to_bytes(6, "little")
        self.assertEqual(list(decode_bc4_unorm(block, 4, 4)), [210, 0, 180, 150, 120, 90, 60, 30] * 2)

    def test_bc4_six_value_palette_and_partial_block(self):
        indices = sum((i % 8) << (3 * i) for i in range(16))
        block = bytes([0, 200]) + indices.to_bytes(6, "little")
        self.assertEqual(list(decode_bc4_unorm(block, 4, 4)), [0, 200, 40, 80, 120, 160, 0, 255] * 2)
        self.assertEqual(list(decode_bc4_unorm(block, 3, 2)), [0, 200, 40, 120, 160, 0])
        with self.assertRaises(ValueError):
            decode_bc4_unorm(block[:-1], 4, 4)

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

    # A constant held bit toggling the whole matched constellation is stronger
    # evidence than median-image grouped/shared-mask behaviour alone.
    def test_constant_bit_toggle_takes_precedence_as_temporal_waveform_evidence(self):
        self.assertEqual(
            classify_led_blink_semantics({5}, [5], [5]),
            "temporal_waveform_supported",
        )
        self.assertEqual(
            classify_led_blink_semantics({5}, [5], []),
            "grouped_or_shared_mask_supported",
        )
        self.assertEqual(
            classify_led_blink_semantics({5}, [], []),
            "spatial_mask_supported",
        )

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
