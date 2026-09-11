#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from psvr2_camera_mode_survey import CameraPacketFramer, parse_vi_header  # noqa: E402


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
