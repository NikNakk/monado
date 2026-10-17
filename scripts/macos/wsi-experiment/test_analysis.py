# SPDX-License-Identifier: BSL-1.0
"""Check frame joins and reject invalid experimental controls."""
import csv
import pathlib
import tempfile
import unittest

from analyze import summarize


class AnalysisTests(unittest.TestCase):
    def write(self, directory, suffix, rows):
        with (directory / f'monado_psvr2_1_{suffix}.csv').open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def fixture(self, directory, condition='native'):
        frames = [dict(system_frame_id=i, source_valid=1, sample_ns=1000000000 + (i - 10) * 11122222,
                       predicted_display_ns=1006000000 + (i - 10) * 11122222) for i in (10, 11, 12)]
        self.write(directory, 'reprojection', frames)
        self.write(directory, 'vblank', [dict(display_period_ns=11122222)])
        self.write(directory, 'compositor_gpu', [dict(frame_id=10, start_ns=1, end_ns=1000001)])
        self.write(directory, 'client_frame_map', [dict(system_frame_id=10, reused=0), dict(system_frame_id=12, reused=1)])
        self.write(directory, 'pose', [dict(host_query_ns=1000000000, relation_flags=0x33),
                                       dict(host_query_ns=1100000000, relation_flags=3)])
        # Deliberately reversed feedback order and a missing middle frame.
        self.write(directory, 'presented', [dict(frame_id=12, presented_monotonic_ns=1029244444),
                                            dict(frame_id=10, presented_monotonic_ns=1008000000)])
        self.write(directory, 'present_complete', [dict(frame_id=10, status=4, gpu_start_time_s=1, gpu_end_time_s=1.0002)])
        self.write(directory, 'present', [dict(frame_id=10)])
        self.write(directory, 'wsi', [
            dict(event='submit', frame_id=10, actual_present_ns=0, begin_ns=100, end_ns=200,
                 credit_wait_ns=0, pending=2, result=0),
            dict(event='feedback', frame_id=12, actual_present_ns=1029244444, begin_ns=0, end_ns=0,
                 credit_wait_ns=0, pending=2, result=0),
            dict(event='feedback', frame_id=10, actual_present_ns=1008000000, begin_ns=0, end_ns=0,
                 credit_wait_ns=0, pending=1, result=0)])
        return dict(directory=str(directory), application_exit_code=0, measured_display_mode='67 90 4000 2040',
                    condition=condition, requested_hz=90, repeat=1,
                    environment={'XRT_MACOS_WSI_MAX_PENDING': '2'})

    def test_frame_id_join_preserves_missing_feedback(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.fixture(pathlib.Path(temporary))
            r = summarize(run, 0)
            self.assertEqual(r['joined_feedback_frames'], 2)
            self.assertAlmostEqual(r['feedback_coverage_percent'], 200 / 3)
            self.assertAlmostEqual(r['sample_to_present_ms']['p50'], 7.5)
            self.assertAlmostEqual(r['prediction_error_ms']['p50'], 1.5)
            self.assertEqual(r['reused_source_percent'], 50)
            self.assertEqual(r['valid_tracked_pose_percent'], 50)

    def test_wsi_credit_limit_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.fixture(pathlib.Path(temporary), 'wsi2')
            self.assertEqual(summarize(run, 0)['max_pending_at_submit'], 2)
            path = pathlib.Path(temporary) / 'monado_psvr2_1_wsi.csv'
            path.write_text(path.read_text().replace('0,2,0', '0,3,0'))
            with self.assertRaisesRegex(ValueError, 'credit bound'):
                summarize(run, 0)

    def test_failed_metal_command_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'present_complete', [dict(frame_id=10, status=5)])
            with self.assertRaisesRegex(ValueError, 'Metal command'):
                summarize(run, 0)

    def test_reversed_display_order_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'presented', [dict(frame_id=12, presented_monotonic_ns=1030000000),
                                                dict(frame_id=10, presented_monotonic_ns=1040000000)])
            with self.assertRaisesRegex(ValueError, 'frame order'):
                summarize(run, 0)

    def test_combined_gpu_span_joins_same_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.fixture(pathlib.Path(temporary))
            result = summarize(run, 0)
            self.assertAlmostEqual(result['compositor_combined_gpu_span_ms']['p50'], 1.2)
            self.assertEqual(result['compositor_combined_gpu_span_ms']['count'], 1)

    def test_missing_load_and_gpu_failure_logs_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            run['load'] = 'gpu16'
            with self.assertRaisesRegex(ValueError, 'GPU load was not observed'):
                summarize(run, 0)
            (directory / 'application.log').write_text('stress GPU passes per eye=16\nGPU command error: failed\n')
            with self.assertRaisesRegex(ValueError, 'error in logs'):
                summarize(run, 0)

    def test_invalid_acquired_image_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory, 'wsi')
            self.write(directory, 'wsi_acquire', [dict(frame_id=10, begin_ns=1, end_ns=10, image_index=3, result=0)])
            with self.assertRaisesRegex(ValueError, 'invalid WSI acquisition'):
                summarize(run, 0)

    def test_native_trace_id_must_match_renderer_timeline(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            self.write(directory, 'present_complete', [dict(frame_id=10, timeline_value=11, status=4)])
            with self.assertRaisesRegex(ValueError, 'renderer timeline ID'):
                summarize(run, 0)

    def test_generic_feedback_deadline_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory, 'wsi-generic')
            self.write(directory, 'refresh', [dict(display_period_ns=11122222)])
            rows = [dict(event='submit', frame_id=10, actual_present_ns=0, begin_ns=100, end_ns=200,
                         credit_wait_ns=0, pending=0, result=0, desired_present_ns=1006000000, slop_ns=500000),
                    dict(event='feedback', frame_id=10, actual_present_ns=1008000000, begin_ns=0, end_ns=0,
                         credit_wait_ns=0, pending=0, result=0, desired_present_ns=1005500000, slop_ns=0)]
            self.write(directory, 'wsi', rows)
            self.assertEqual(summarize(run, 0)['joined_feedback_frames'], 1)
            rows[1]['desired_present_ns'] += 1
            self.write(directory, 'wsi', rows)
            with self.assertRaisesRegex(ValueError, 'deadline did not round trip'):
                summarize(run, 0)

    def cv_fixture(self, directory):
        run = self.fixture(directory, 'wsi-generic-cv')
        self.write(directory, 'refresh', [dict(display_period_ns=11122222)])
        self.write(directory, 'wsi', [
            dict(event='submit', frame_id=10, actual_present_ns=0, begin_ns=100, end_ns=200,
                 credit_wait_ns=0, pending=0, result=0, desired_present_ns=1006000000, slop_ns=500000),
            dict(event='feedback', frame_id=10, actual_present_ns=1008000000, begin_ns=0, end_ns=0,
                 credit_wait_ns=0, pending=0, result=0, desired_present_ns=1005500000, slop_ns=0)])
        run['environment']['XRT_MACOS_WSI_GENERIC_CV'] = '1'
        (directory / 'service.stderr.log').write_text('CVDisplayLink=1\nnon-feedback version\n')
        rows = [dict(consumed_ns=1000000000 + i * 11122222, vblank_ns=999000000 + i * 11122222,
                     display_period_ns=11122222) for i in range(9)]
        self.write(directory, 'cv_vblank', rows)
        return run, rows

    def test_cv_source_and_phase_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run, rows = self.cv_fixture(directory)
            self.assertEqual(summarize(run, 0)['pacing_source'], 'CVDisplayLink')
            (directory / 'service.stderr.log').write_text('standard display-timing pacer\n')
            with self.assertRaisesRegex(ValueError, 'source / fallback pacer'):
                summarize(run, 0)
            (directory / 'service.stderr.log').write_text('CVDisplayLink=1\nnon-feedback version\n')
            rows[4]['vblank_ns'] = rows[3]['vblank_ns']
            self.write(directory, 'cv_vblank', rows)
            with self.assertRaisesRegex(ValueError, 'phase did not advance'):
                summarize(run, 0)

    def test_hosted_uses_physical_cv_instead_of_nominal_wsi_query(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run, _ = self.cv_fixture(directory)
            run['hosted'] = True
            self.write(directory, 'refresh', [dict(display_period_ns=16666666)])
            (directory / 'service.stderr.log').write_text('')
            (directory / 'application.log').write_text('CVDisplayLink=1\nnon-feedback version\n')
            result = summarize(run, 0)
            self.assertAlmostEqual(result['display_hz'], 89.91, places=2)
            self.assertEqual(result['refresh_source'], 'CVDisplayLink (hosted physical display)')

    def test_cv_future_timestamp_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run, rows = self.cv_fixture(directory)
            rows[0]['vblank_ns'] = rows[0]['consumed_ns'] + 1
            self.write(directory, 'cv_vblank', rows)
            with self.assertRaisesRegex(ValueError, 'invalid or missing CVDisplayLink'):
                summarize(run, 0)

    def test_calibration_bounds_filter_and_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run, _ = self.cv_fixture(directory)
            run['environment']['XRT_MACOS_WSI_GENERIC_CALIBRATE'] = '1'
            rows = [dict(frame_id=i, sample_ns=5000000, filtered_ns=5000000, sample_count=i, accepted=1)
                    for i in range(1, 9)]
            rows += [dict(frame_id=9, sample_ns=-1, filtered_ns=5000000, sample_count=8, accepted=0),
                     dict(frame_id=10, sample_ns=50000000, filtered_ns=5000000, sample_count=8, accepted=0)]
            self.write(directory, 'cv_offset', rows)
            self.assertEqual(summarize(run, 0)['calibrated_offset_ms']['p50'], 5)
            rows[-1]['accepted'] = 1
            self.write(directory, 'cv_offset', rows)
            with self.assertRaisesRegex(ValueError, 'filter/bounds'):
                summarize(run, 0)
            self.write(directory, 'cv_offset', rows[:7])
            with self.assertRaisesRegex(ValueError, 'calibration did not activate'):
                summarize(run, 0)

    def test_current_presenter_manifest_uses_calibrated_wsi_without_old_flags(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run, _ = self.cv_fixture(directory)
            run['condition'] = 'wsi'
            run['environment'] = {'XRT_MACOS_PRESENTER': 'wsi'}
            self.write(directory, 'cv_offset', [
                dict(frame_id=i, sample_ns=5000000, filtered_ns=5000000, sample_count=i, accepted=1)
                for i in range(1, 9)])
            self.assertEqual(summarize(run, 0)['calibrated_offset_ms']['p50'], 5)
            run['condition'] = 'wsi-adaptive'
            run['environment']['XRT_MACOS_PRESENTER'] = 'wsi-adaptive'
            result = summarize(run, 0)
            self.assertEqual(result['joined_feedback_frames'], 1)
            self.assertEqual(result['calibrated_offset_ms']['count'], 0)

    def test_refresh_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.fixture(pathlib.Path(temporary))
            run['requested_hz'] = 120
            with self.assertRaisesRegex(ValueError, 'refresh was not applied'):
                summarize(run, 0)

    def test_failed_client_and_missing_projection_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            run = self.fixture(directory)
            run['application_exit_code'] = -11
            with self.assertRaisesRegex(ValueError, 'application failed'):
                summarize(run, 0)
            run['application_exit_code'] = 0
            self.write(directory, 'reprojection', [dict(system_frame_id=1, source_valid=0)])
            with self.assertRaisesRegex(ValueError, 'no application projection'):
                summarize(run, 0)


class PresenterPolicyTests(unittest.TestCase):
    def test_current_selector_overrides_archived_flags(self):
        from analyze import presenter_policy
        for mode, expected in [('native', (False, False)), ('wsi', (True, True)),
                               ('wsi-adaptive', (False, False))]:
            env = {'XRT_MACOS_PRESENTER': mode, 'XRT_MACOS_WSI_GENERIC_CV': '1',
                   'XRT_MACOS_WSI_GENERIC_CALIBRATE': '1'}
            self.assertEqual(presenter_policy({'environment': env}), expected)
        with self.assertRaisesRegex(ValueError, 'invalid presenter'):
            presenter_policy({'environment': {'XRT_MACOS_PRESENTER': 'typo'}})


if __name__ == '__main__':
    unittest.main()
