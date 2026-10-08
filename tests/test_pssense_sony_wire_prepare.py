# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from pssense_sony_wire_prepare import reports, rebase_settings, phase_reference, aligned_prescan_phase


class WirePreparationTests(unittest.TestCase):
    def test_estimator_creep_is_not_transferred_as_a_physical_cycle_rate(self):
        lines = ['LED_BOOTSTRAP side=R event=locked lit_start_us=15000']
        for i in range(100):
            lines.append(f'LED_SCHEDULE side=R cycle_position={1000000 + i * 50049} '
                         f'blink_host={1000000000 + i * 16683350} period=16683350 pulse=1000000')
        ref = phase_reference('\n'.join(lines), 'R')
        self.assertNotIn('mac_cycle_ticks', ref)
        settings = bytearray(38)
        settings[19] = 1
        struct.pack_into('<I', settings, 26, 50_050_050)
        ticks = ref['cycle_position'] + 3_000_000
        phase = aligned_prescan_phase(settings, ref, ticks, ref['blink_host'] + 1_000_000_000)
        rebased = rebase_settings(settings, ticks, phase)
        pulse = int.from_bytes(rebased[22:26], 'little')
        cycles = round((pulse - ref['cycle_position']) / 50050.05)
        # Nanosecond/third-microsecond conversions round twice: under 1 us.
        self.assertLessEqual(abs(pulse - ref['cycle_position'] - cycles * 50050.05), 2)
        self.assertEqual(rebased[26:30], settings[26:30])
        with self.assertRaises(ValueError):
            aligned_prescan_phase(settings, ref, ticks, ref['blink_host'] + 91_000_000_000)

    def test_mac_alignment_projects_healthy_anchor_and_rejects_restart(self):
        log = "LED_BOOTSTRAP side=R event=locked lit_start_us=15000\n"
        log += "LED_SCHEDULE side=R cycle_position=4294901760 blink_host=1000000000 period=16683350 pulse=1000000\n"
        ref = phase_reference(log, "R")
        settings = bytearray(38)
        settings[19] = 1
        struct.pack_into("<I", settings, 26, 50_050_050)
        now_ns = ref['blink_host'] + 100_000_000
        ticks = (ref['cycle_position'] + 300_000) & 0xffffffff
        phase = aligned_prescan_phase(settings, ref, ticks, now_ns)
        rebased = rebase_settings(settings, ticks, phase)
        anchor_age = ((int.from_bytes(rebased[22:26], 'little') - ref['cycle_position']) & 0xffffffff) * 1000 // 3
        self.assertLess(min(anchor_age % 16_683_350, 16_683_350 - anchor_age % 16_683_350), 334)
        with self.assertRaises(ValueError):
            aligned_prescan_phase(settings, ref, 1, now_ns)
        with self.assertRaises(ValueError):
            phase_reference(log + 'LED_BOOTSTRAP side=R event=stuck_lit reason=fault\n', 'R')

    def test_prescan_rebase_preserves_modulo_phase_across_tick_wrap(self):
        settings = bytearray(range(38))
        settings[19] = 1
        struct.pack_into("<I", settings, 26, 50_050_050)
        result = rebase_settings(settings, 0xFFFFFFF0, 15_701_616)
        ticks = int.from_bytes(result[22:26], "little")
        delta = (ticks - 0xFFFFFFF0) & 0xFFFFFFFF
        self.assertGreaterEqual(delta * 1000 // 3, 50_000_000)
        self.assertLess(abs((delta * 1000 // 3) % 16_683_350 - 15_701_616), 334)
        self.assertEqual(result[:22] + result[26:], settings[:22] + settings[26:])
        settings[19], settings[22:26] = 2, bytes(4)
        self.assertEqual(rebase_settings(settings, 1234, None), bytes(settings))

    def test_signature_requires_acl_lengths_and_hid_crc(self):
        body = bytearray(78)
        body[0] = 0x31
        body[-4:] = zlib.crc32(body[:-4], zlib.crc32(b"\xa2")).to_bytes(4, "little")
        wire = struct.pack("<HHHH", 0x2014, 83, 79, 0x42) + b"\xa2" + body
        self.assertEqual(list(reports(wire)), [(0xA2, 20, 0x42, bytes(body))])
        self.assertEqual(list(reports(wire[:-1] + bytes([wire[-1] ^ 1]))), [])
        self.assertEqual(list(reports(wire[8:])), [])

    @unittest.skipUnless(sys.platform == "darwin", "IOKit sender is macOS-only")
    def test_complete_settings_preserved_with_fresh_latch_counter_and_crc(self):
        from pssense_led_poke import Sender
        settings = bytes(range(38))
        sender = Sender([])
        sender.seq, sender.counter, sender.led_seq = 15, 255, 256
        first = sender.report(2, settings=settings)
        second = sender.report(2, settings=settings)
        for packet in (first, second):
            actual = packet[3:41]
            self.assertEqual([actual[i] for i in range(38) if i not in range(15, 19) and i != 20],
                             [settings[i] for i in range(38) if i not in range(15, 19) and i != 20])
            self.assertEqual(actual[20], 0)
            self.assertEqual(zlib.crc32(packet[:-4], zlib.crc32(b"\xa2")), int.from_bytes(packet[-4:], "little"))
        self.assertEqual((first[1], first[41], second[1], second[41]), (240, 255, 0, 0))


if __name__ == "__main__":
    unittest.main()
