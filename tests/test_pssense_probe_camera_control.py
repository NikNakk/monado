# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from pssense_probe_camera_control import last_probe_led_sequence


class LatchHandoffTests(unittest.TestCase):
    def test_sequence_handoff_uses_last_actual_full_tx(self):
        first, last = bytearray(78), bytearray(78)
        first[0] = last[0] = 0x31
        first[23], last[23] = 1, 255
        text = f"PSSENSE_RAW direction=tx result=0 hex={first.hex()}\n"
        text += f"PSSENSE_RAW direction=rx result=0 hex={first.hex()}\n"
        text += f"PSSENSE_RAW direction=tx result=0 hex={last.hex()}\n"
        text += "PSSENSE_RAW direction=tx hex=00\n"
        self.assertEqual(last_probe_led_sequence(text), 255)
        self.assertEqual((last_probe_led_sequence(text) + 1) & 255, 0)

    def test_other_controller_tx_cannot_seed_selected_hand(self):
        right, left = bytearray(78), bytearray(78)
        right[0] = left[0] = 0x31
        right[23], left[23] = 9, 80
        text = f'PSSENSE_RAW side=R direction=tx result=0 hex={right.hex()}\n'
        text += f'PSSENSE_RAW side=L direction=tx result=0 hex={left.hex()}\n'
        self.assertEqual(last_probe_led_sequence(text, 'R'), 9)
        self.assertEqual(last_probe_led_sequence(text, 'L'), 80)


if __name__ == "__main__":
    unittest.main()
