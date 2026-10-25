# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'sense_summary', Path(__file__).resolve().parents[1] / 'scripts/macos/summarize-sense-losses.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class LossSummaryTest(unittest.TestCase):
    def summarize(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'service.log'
            path.write_text(text)
            return summary.summarize(path)

    def test_active_snapshots_are_not_added_to_recovered_totals(self):
        result = self.summarize('''JOINT_LOSS_ACTIVE device=1 gap_ms=300 exposures=18 dark_unpredicted=1
JOINT_LOSS_ACTIVE device=1 gap_ms=5300 exposures=318 dark_unpredicted=301
JOINT_LOSS device=1 gap_ms=6000 exposures=360 dark_unpredicted=343
JOINT_LOSS_ACTIVE device=1 gap_ms=300 exposures=18 dark_unpredicted=1
''')
        device = result['devices']['1']
        self.assertEqual(device['recovered_losses'], 1)
        self.assertEqual(device['recovered_gap_ms'], 6000)
        self.assertEqual(device['exposure_classes']['dark_unpredicted'], 343)
        self.assertEqual(device['latest_active']['exposures'], '18')

    def test_transport_failure_and_reconnection_remain_separate_from_optical(self):
        result = self.summarize('''PSSENSE_OUTPUT side=R start_ns=1000000 end_ns=2000000 result=-1 expected=78 input_ns=500000
CONNECTION side=R event=disconnected result=-1,
PSSENSE_OUTPUT side=R start_ns=3000000 end_ns=5000000 result=78 expected=78 input_ns=2500000
CONNECTION side=R event=connected
''')
        self.assertEqual(result['devices'], {})
        transport = result['transport_by_side']['R']
        self.assertEqual(transport['failed_writes'], 1)
        self.assertEqual(transport['max_write_ms'], 2)
        self.assertEqual(transport['max_input_gap_ms'], 2)
        self.assertEqual(len(result['connections']), 2)


if __name__ == '__main__':
    unittest.main()
