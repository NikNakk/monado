#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Join real compositor predictions, GPU timing and display feedback by frame ID."""
import argparse
import collections
import csv
import json
import math
import pathlib
import re
import statistics


def presenter_policy(run):
    """Current selector takes precedence; retain old flags only for archived captures."""
    env = run['environment']
    if 'XRT_MACOS_PRESENTER' in env:
        mode = env['XRT_MACOS_PRESENTER']
        if mode not in ('native', 'wsi', 'wsi-adaptive'):
            raise ValueError('invalid presenter selection in capture manifest')
        return mode == 'wsi', mode == 'wsi'
    return (env.get('XRT_MACOS_WSI_GENERIC_CV') == '1',
            env.get('XRT_MACOS_WSI_GENERIC_CALIBRATE') == '1')


def distribution(values):
    a = sorted(values)
    def q(p):
        if not a:
            return None
        i = (len(a) - 1) * p
        return a[math.floor(i)] + (a[math.ceil(i)] - a[math.floor(i)]) * (i - math.floor(i))
    return {'count': len(a), 'p50': q(.5), 'p95': q(.95), 'p99': q(.99), 'min': min(a) if a else None,
            'max': max(a) if a else None}


def table(directory, suffix):
    paths = list(directory.glob(f'monado_psvr2_*_{suffix}.csv'))
    if len(paths) != 1:
        raise ValueError(f'{directory}: expected one {suffix} trace; found {len(paths)}')
    with paths[0].open(newline='') as stream:
        return list(csv.DictReader(stream))


def summarize(run, warmup):
    directory = pathlib.Path(run.get('trace_directory', run['directory']))
    log_directory = pathlib.Path(run['directory'])
    if run.get('application_exit_code') != 0:
        raise ValueError(f'{directory}: application failed')
    mode = run['measured_display_mode'].split()
    if abs(float(mode[1]) - run['requested_hz']) > 1:
        raise ValueError(f'{directory}: requested refresh was not applied')
    app_log = (log_directory / 'application.log').read_text() if (log_directory / 'application.log').exists() else ''
    service_log = (log_directory / 'service.stderr.log').read_text() if (log_directory / 'service.stderr.log').exists() else ''
    if re.search(r'GPU command error:|Asynchronous Metal presentation failed:|^.*ERROR \[|failed assertion|message sent to deallocated',
                 app_log + '\n' + service_log, re.MULTILINE | re.IGNORECASE):
        raise ValueError(f'{directory}: GPU/runtime/validation error in logs')
    if 'load' in run:
        expected_passes = {'gpu4': 4, 'gpu16': 16}.get(run['load'], 1)
        if f'stress GPU passes per eye={expected_passes}\n' not in app_log:
            raise ValueError(f'{directory}: requested GPU load was not observed')
        if run['load'] == 'light' and 'stress instance limit=24\n' not in app_log:
            raise ValueError(f'{directory}: light-scene control was not observed')
        if run['load'] == 'cpu' and 'stress CPU delay frame=' not in app_log:
            raise ValueError(f'{directory}: no injected CPU stalls observed')
    reprojection = [r for r in table(directory, 'reprojection') if int(r['source_valid'])]
    if not reprojection:
        raise ValueError(f'{directory}: no application projection layers')
    start = int(reprojection[0]['sample_ns']) + warmup * 1e9
    frames = {int(r['system_frame_id']): r for r in reprojection if int(r['sample_ns']) >= start}
    if not frames:
        raise ValueError(f'{directory}: no frames after warm-up')
    generic_wsi = (run['condition'].startswith('wsi-generic') or
                   run['environment'].get('XRT_MACOS_PRESENTER') in ('wsi', 'wsi-adaptive'))
    wsi = run['condition'].startswith('wsi')
    acquire = []
    if wsi:
        if list(directory.glob('*_wsi_acquire.csv')):
            acquire = [r for r in table(directory, 'wsi_acquire') if int(r['frame_id']) in frames]
            if any(int(r['result']) not in (0, 1000001003) or not 0 <= int(r['image_index']) < 3 for r in acquire):
                raise ValueError(f'{directory}: invalid WSI acquisition result/image')
        trace = table(directory, 'wsi')
        if generic_wsi:
            submitted = {int(r['frame_id']): r for r in trace if r['event'] == 'submit'}
            for row in trace:
                if row['event'] == 'feedback' and int(row['frame_id']) in submitted:
                    sent = submitted[int(row['frame_id'])]
                    if int(row['desired_present_ns']) + int(sent['slop_ns']) != int(sent['desired_present_ns']):
                        raise ValueError(f'{directory}: generic deadline did not round trip exactly')
        feedback = {int(r['frame_id']): int(r['actual_present_ns']) for r in trace
                    if r['event'] == 'feedback' and int(r['actual_present_ns']) > 0}
        submits = [r for r in trace if r['event'] == 'submit' and int(r['frame_id']) in frames]
        present_call = [(int(r['end_ns']) - int(r['begin_ns'])) / 1e6 for r in submits]
        credits = [int(r['credit_wait_ns']) / 1e6 for r in submits]
        max_pending = max((int(r['pending']) for r in submits), default=0)
        limit = int(run['environment'].get('XRT_MACOS_WSI_MAX_PENDING', '0'))
        if limit and max_pending > limit:
            raise ValueError(f'{directory}: presentation credit bound violated')
        if any(int(r['result']) not in (0, 1000001003) for r in submits):
            raise ValueError(f'{directory}: Vulkan present failure')
    else:
        feedback = {int(r['frame_id']): int(r['presented_monotonic_ns']) for r in table(directory, 'presented')
                    if int(r['presented_monotonic_ns']) > 0}
        submits = [r for r in table(directory, 'present') if int(r['frame_id']) in frames]
        # The native path measures async handoff in a different CSV. Do not compare
        # its worker's full duration to vkQueuePresentKHR as if both were CPU submit time.
        present_call, credits, max_pending = [], [], None
    joined = [(frame_id, frame, feedback[frame_id]) for frame_id, frame in frames.items() if frame_id in feedback]
    if not joined:
        raise ValueError(f'{directory}: no frame IDs join to display feedback')
    if any(actual < int(frame['sample_ns']) for _, frame, actual in joined):
        raise ValueError(f'{directory}: presentation clock precedes rendering sample')
    ordered = sorted(joined, key=lambda row: row[0])
    if any(b[2] <= a[2] for a, b in zip(ordered, ordered[1:])):
        raise ValueError(f'{directory}: presentation frame order is not strictly increasing')
    times = sorted(actual for _, _, actual in joined)
    intervals = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
    cv, calibrated = presenter_policy(run)
    nominal = statistics.median(int(r['display_period_ns']) for r in table(directory, 'cv_vblank' if run.get('hosted') and cv else 'refresh' if generic_wsi else 'vblank')
                                if int(r['display_period_ns']) > 0)
    measured_hz = 1e9 / nominal
    if abs(measured_hz - run['requested_hz']) > 1:
        raise ValueError(f'{directory}: display-link period differs from requested refresh')
    cv_rows = table(directory, 'cv_vblank') if cv else []
    if cv:
        if len(cv_rows) < 8 or any(int(r['vblank_ns']) <= 0 or int(r['vblank_ns']) > int(r['consumed_ns']) for r in cv_rows):
            raise ValueError(f'{directory}: invalid or missing CVDisplayLink timing')
        if any(int(b['vblank_ns']) <= int(a['vblank_ns']) for a, b in zip(cv_rows, cv_rows[1:])):
            raise ValueError(f'{directory}: CVDisplayLink phase did not advance')
        if abs(1e9 / statistics.median(int(r['display_period_ns']) for r in cv_rows) - run['requested_hz']) > 1:
            raise ValueError(f'{directory}: CVDisplayLink period differs from requested refresh')
        if 'CVDisplayLink=1' not in service_log + app_log or 'non-feedback version' not in service_log + app_log:
            raise ValueError(f'{directory}: CV source / fallback pacer not observed')
    offset_rows = table(directory, 'cv_offset') if calibrated else []
    if calibrated:
        if not cv or not any(int(r['sample_count']) >= 8 for r in offset_rows):
            raise ValueError(f'{directory}: calibration did not activate')
        count, filtered = 0, 0
        period = int(cv_rows[0]['display_period_ns'])
        for row in offset_rows:
            sample = int(row['sample_ns'])
            accepted = 0 < sample <= period * 4
            if accepted:
                filtered = sample if count == 0 else (filtered * 7 + sample) // 8
                count += 1
            if (int(row['accepted']) != accepted or int(row['sample_count']) != count or
                    int(row['filtered_ns']) != filtered):
                raise ValueError(f'{directory}: calibration trace violates filter/bounds')
    gpu = [r for r in table(directory, 'compositor_gpu') if int(r['frame_id']) in frames]
    if not gpu:
        raise ValueError(f'{directory}: no compositor GPU query results')
    if any(int(r['end_ns']) < int(r['start_ns']) for r in gpu):
        raise ValueError(f'{directory}: negative Vulkan GPU interval')
    all_completions = [] if wsi else table(directory, 'present_complete')
    completions = [r for r in all_completions if int(r['frame_id']) in frames]
    if any(int(r['frame_id']) != int(r['timeline_value']) for r in all_completions if 'timeline_value' in r):
        raise ValueError(f'{directory}: native trace ID differs from renderer timeline ID')
    if run.get('gpu_attribution') and any(int(r['shared_event_wait']) != 0 for r in all_completions):
        raise ValueError(f'{directory}: native GPU wait still inside attribution interval')
    if any(int(r['status']) != 4 for r in all_completions):
        raise ValueError(f'{directory}: Metal command did not complete successfully')
    blits = {int(r['frame_id']): (float(r['gpu_end_time_s']) - float(r['gpu_start_time_s'])) * 1000
             for r in completions if float(r['gpu_end_time_s']) > float(r['gpu_start_time_s'])}
    if not wsi and not blits:
        raise ValueError(f'{directory}: no native Metal GPU completion intervals')
    gpu_durations = {int(r['frame_id']): (int(r['end_ns']) - int(r['start_ns'])) / 1e6 for r in gpu}
    total = [duration + blits[i] for i, duration in gpu_durations.items() if i in blits] if not wsi else list(gpu_durations.values())
    drawable_wait = []
    if not wsi and list(directory.glob('*_present_worker.csv')):
        drawable_wait = [int(r['drawable_wait_ns']) / 1e6 for r in table(directory, 'present_worker')
                         if r['event'] == 'submitted' and int(r['frame_id']) in frames]
    source = {int(r['system_frame_id']): r for r in table(directory, 'client_frame_map')}
    reuse = [int(source[i]['reused']) for i, _, _ in joined if i in source]
    poses = [r for r in table(directory, 'pose') if int(r['host_query_ns']) >= start]
    valid_pose = [((int(r['relation_flags']) & 0x33) == 0x33) for r in poses]
    return {
        'directory': run['directory'], 'condition': run['condition'], 'requested_hz': run['requested_hz'],
        'repeat': run['repeat'], 'load': run.get('load', 'baseline'),
        'gpu_attribution': run.get('gpu_attribution', False), 'warmup_seconds': warmup, 'display_hz': measured_hz,
        'refresh_source': 'CVDisplayLink (hosted physical display)' if run.get('hosted') and cv else 'GOOGLE refresh-cycle query' if generic_wsi else 'CVDisplayLink',
        'rendered_frames': len(frames), 'joined_feedback_frames': len(joined),
        'feedback_coverage_percent': len(joined) / len(frames) * 100,
        'observed_present_hz': (len(times) - 1) * 1e9 / (times[-1] - times[0]) if len(times) > 1 and times[-1] > times[0] else None,
        'observed_long_intervals': sum(dt > 1.5 * nominal / 1e6 for dt in intervals),
        'observed_interval_ms': distribution(intervals),
        'sample_to_present_ms': distribution([(actual - int(frame['sample_ns'])) / 1e6 for _, frame, actual in joined]),
        'prediction_error_ms': distribution([(actual - int(frame['predicted_display_ns'])) / 1e6 for _, frame, actual in joined]),
        'abs_prediction_error_ms': distribution([abs(actual - int(frame['predicted_display_ns'])) / 1e6 for _, frame, actual in joined]),
        'application_source_age_at_present_ms': distribution([(actual - int(frame['layer_timestamp_ns'])) / 1e6
            for _, frame, actual in joined if 'layer_timestamp_ns' in frame]),
        'application_gpu_batch_mean_ms': distribution([float(v) for v in re.findall(r'GPU normal n=240 mean=([0-9.]+)ms', app_log)]),
        'injected_cpu_stalls': app_log.count('stress CPU delay frame='),
        'compositor_gpu_ms': distribution([(int(r['end_ns']) - int(r['start_ns'])) / 1e6 for r in gpu]),
        'native_metal_command_ms': distribution(list(blits.values())),
        'compositor_combined_gpu_span_ms': distribution(total),
        'native_successful_completions': len(completions),
        'native_drawable_worker_wait_ms': distribution(drawable_wait),
        'wsi_acquire_ms': distribution([(int(r['end_ns']) - int(r['begin_ns'])) / 1e6 for r in acquire]),
        'wsi_present_call_ms': distribution(present_call), 'wsi_credit_wait_ms': distribution(credits),
        'max_pending_at_submit': max_pending,
        'generic_display_timing_enabled': not cv and run['environment'].get('XRT_COMPOSITOR_USE_DISPLAY_TIMING', '1') == '1',
        'cv_vblank_samples': len(cv_rows),
        'pacing_source': 'CVDisplayLink' if cv else ('GOOGLE feedback' if run['condition'].startswith('wsi') and run['environment'].get('XRT_COMPOSITOR_USE_DISPLAY_TIMING', '1') == '1' else 'fallback estimates' if run['condition'].startswith('wsi') else 'native CADisplayLink' if run['environment'].get('XRT_MACOS_DISPLAY_LINK', 'cv') == 'ca' else 'native CVDisplayLink'),
        'cv_observed_interval_ms': distribution([(int(b['vblank_ns']) - int(a['vblank_ns'])) / 1e6 for a, b in zip(cv_rows, cv_rows[1:])]),
        'calibrated_offset_ms': distribution([int(r['filtered_ns']) / 1e6 for r in offset_rows if int(r['sample_count']) >= 8]),
        'generic_max_comp_percent': int(run['environment'].get('U_PACING_COMP_TIME_MAX_FRACTION_PERCENT', '30')),
        'generic_present_margin_ms': distribution([int(r['present_margin_ns']) / 1e6 for r in trace
            if r['event'] == 'feedback' and int(r['frame_id']) in frames and 'present_margin_ns' in r]) if wsi else distribution([]),
        'reused_source_percent': sum(reuse) / len(reuse) * 100 if reuse else None,
        'valid_tracked_pose_percent': sum(valid_pose) / len(valid_pose) * 100 if valid_pose else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=pathlib.Path)
    parser.add_argument('--warmup-seconds', type=float, default=2)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    if args.warmup_seconds < 0:
        parser.error('warm-up must be nonnegative')
    manifest = json.loads(args.manifest.read_text())
    results = [summarize(run, args.warmup_seconds) for run in manifest['runs']]
    if args.json:
        print(json.dumps(results, indent=2, allow_nan=False))
        return
    for r in results:
        print(f"{r['requested_hz']}Hz r{r['repeat']} {r['condition']}: {r['observed_present_hz']:.2f} observed Hz, "
              f"feedback {r['feedback_coverage_percent']:.2f}%")
        for name in ['sample_to_present_ms', 'prediction_error_ms', 'abs_prediction_error_ms', 'compositor_gpu_ms', 'wsi_credit_wait_ms']:
            d = r[name]
            if d['count']:
                print(f"  {name}: p50={d['p50']:.3f} p95={d['p95']:.3f} p99={d['p99']:.3f} max={d['max']:.3f}")
        print(f"  reused={r['reused_source_percent']:.2f}% tracked poses={r['valid_tracked_pose_percent']:.2f}% "
              f"long feedback intervals={r['observed_long_intervals']}")


if __name__ == '__main__':
    main()
