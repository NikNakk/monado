# SPDX-License-Identifier: BSL-1.0
"""Prevent mismatched controls from being presented as native/WSI pairs."""
import copy
import csv
import pathlib
import tempfile
import unittest

from compare import pair_results, source_metrics, METRICS


class PairTests(unittest.TestCase):
    def fixture(self):
        runs, results = [], []
        for i, condition in enumerate(('native', 'wsi-generic-calibrated')):
            runs.append(dict(condition=condition, requested_hz=90, load='baseline', repeat=1,
                             application_exit_code=0, end_utc='done', directory=condition,
                             environment={'XRT_MACOS_WSI_GENERIC': str(i), 'PSVR2_TIMING_TRACE_DIR': condition,
                                          'PSVR2_TEST_GPU_PASSES': '1'}))
            result = dict(directory=condition, warmup_seconds=2)
            for metric, percentile in METRICS.values():
                if percentile:
                    result.setdefault(metric, {})[percentile] = 10 + i
                else:
                    result[metric] = 90 + i
            results.append(result)
        return dict(end_utc='done', display_restore_exit_code=0, runs=runs), results

    def test_matched_pair_has_signed_difference(self):
        manifest, results = self.fixture()
        pair = pair_results(manifest, results)[0]
        self.assertEqual(pair['deltas_wsi_minus_native']['sample_age_p99_ms'], 1)
        self.assertAlmostEqual(pair['present_rate_change_percent'], 100 / 90)

    def test_changed_workload_rejected(self):
        manifest, results = self.fixture()
        manifest['runs'][1]['environment']['PSVR2_TEST_GPU_PASSES'] = '16'
        with self.assertRaisesRegex(ValueError, 'uncontrolled environment'):
            pair_results(manifest, results)

    def test_missing_or_duplicate_pair_rejected(self):
        manifest, results = self.fixture()
        with self.assertRaisesRegex(ValueError, 'unmatched'):
            pair_results({**manifest, 'runs': manifest['runs'][:1]}, results[:1])
        manifest['runs'].append(copy.deepcopy(manifest['runs'][0]))
        results.append(copy.deepcopy(results[0]))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            pair_results(manifest, results)

    def test_failed_restoration_and_reordered_results_rejected(self):
        manifest, results = self.fixture()
        with self.assertRaisesRegex(ValueError, 'restoration failed'):
            pair_results({**manifest, 'display_restore_exit_code': 1}, results)
        with self.assertRaisesRegex(ValueError, 'ordering mismatch'):
            pair_results(manifest, list(reversed(results)))
        results[1]['warmup_seconds'] = 0
        with self.assertRaisesRegex(ValueError, 'warm-up differs'):
            pair_results(manifest, results)


class SourceTests(unittest.TestCase):
    def write(self, directory, suffix, rows):
        path = directory / f'monado_psvr2_1_{suffix}.csv'
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def fixture(self, directory):
        self.write(directory, 'reprojection', [dict(source_valid=1, sample_ns=1000000000 + i * 5000000, system_frame_id=i) for i in (1, 2, 3)])
        self.write(directory, 'client_frame_map', [dict(system_frame_id=i, client_slot=0, client_frame_id=c, source_use_ordinal=n)
                                                for i, c, n in [(1, 7, 1), (2, 7, 2), (3, 8, 1)]])
        self.write(directory, 'client_gpu', [dict(event='semaphore_ready', client_frame_id=c, event_ns=t)
                                           for c, t in [(7, 1000000000), (8, 1010000000)]])
        self.write(directory, 'app_pacing', [dict(event='delivered', client_frame_id=c, event_ns=t)
                                           for c, t in [(7, 998000000), (8, 1008000000)]])
        self.write(directory, 'presented', [dict(frame_id=i, presented_monotonic_ns=1000000000 + i * 10000000) for i in (1, 2, 3)])
        return dict(directory=str(directory), condition='native')

    def test_unique_frame_rate_distinguishes_repeated_presentations(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.fixture(pathlib.Path(temporary))
            result = source_metrics(run, 0)
            self.assertEqual(result['presented_unique_client_hz'], 50)
            self.assertAlmostEqual(result['presented_reuse_percent'], 100 / 3)
            self.assertEqual(result['max_presented_source_run'], 2)
            self.assertEqual(result['client_ready_to_present_ms']['p50'], 20)
            self.assertEqual(result['client_delivery_to_present_ms']['p50'], 22)

    def test_readiness_after_presentation_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'client_gpu', [dict(event='semaphore_ready', client_frame_id=7, event_ns=2000000000)])
            with self.assertRaisesRegex(ValueError, 'after reported presentation'):
                source_metrics(run, 0)

    def test_incomplete_readiness_join_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'client_gpu', [dict(event='semaphore_ready', client_frame_id=7, event_ns=1000000000)])
            with self.assertRaisesRegex(ValueError, 'less than 99 percent'):
                source_metrics(run, 0)

    def test_multi_client_trace_cannot_be_joined_by_frame_id_alone(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'client_frame_map', [dict(system_frame_id=i, client_slot=i, client_frame_id=7, source_use_ordinal=1)
                                                     for i in (1, 2)])
            with self.assertRaisesRegex(ValueError, 'exactly one active client'):
                source_metrics(run, 0)


if __name__ == '__main__':
    unittest.main()
