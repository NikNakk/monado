#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Compare matched native/WSI captures from one interleaved experiment manifest."""
import argparse
import collections
import json
import pathlib

from analyze import summarize, table, distribution


# The presentation implementation is the intended independent variable.
ALLOWED_ENV_DIFFERENCES = {
    'XRT_MACOS_WSI_GENERIC', 'XRT_MACOS_WSI_GENERIC_CV', 'XRT_MACOS_WSI_GENERIC_CALIBRATE',
}
METRICS = {
    'present_hz': ('observed_present_hz', None),
    'sample_age_p50_ms': ('sample_to_present_ms', 'p50'),
    'sample_age_p99_ms': ('sample_to_present_ms', 'p99'),
    'abs_prediction_p50_ms': ('abs_prediction_error_ms', 'p50'),
    'abs_prediction_p99_ms': ('abs_prediction_error_ms', 'p99'),
    'application_source_age_p99_ms': ('application_source_age_at_present_ms', 'p99'),
    'reused_source_percent': ('reused_source_percent', None),
}


def pair_results(manifest, results):
    if not manifest.get('end_utc') or manifest.get('display_restore_exit_code') != 0 or manifest.get('job_restore_exit_code', 0) != 0:
        raise ValueError('experiment incomplete or display/service restoration failed')
    runs = manifest['runs']
    if len(runs) != len(results):
        raise ValueError('one analyzed result is required per capture')
    groups = collections.defaultdict(dict)
    for run, result in zip(runs, results):
        condition = run['condition']
        if condition not in ('native', 'wsi-generic-calibrated'):
            raise ValueError('expected native and calibrated WSI conditions only')
        if (run.get('application_exit_code') != 0 or not run.get('end_utc') or
                result['directory'] != run['directory']):
            raise ValueError('capture incomplete or analysis ordering mismatch')
        key = (run['requested_hz'], run['load'], run['repeat'])
        if condition in groups[key]:
            raise ValueError('duplicate capture for condition/refresh/load/repeat')
        groups[key][condition] = (run, result)
    pairs = []
    for (hz, load, repeat), conditions in sorted(groups.items()):
        if set(conditions) != {'native', 'wsi-generic-calibrated'}:
            raise ValueError('unmatched native/WSI capture')
        native_run, native = conditions['native']
        wsi_run, wsi = conditions['wsi-generic-calibrated']
        keys = set(native_run['environment']) | set(wsi_run['environment'])
        differences = {key for key in keys if native_run['environment'].get(key) != wsi_run['environment'].get(key)}
        # Trace destinations vary by capture; this cannot affect rendering policy.
        differences.discard('PSVR2_TIMING_TRACE_DIR')
        if differences - ALLOWED_ENV_DIFFERENCES:
            raise ValueError('uncontrolled environment difference: ' + ', '.join(sorted(differences)))
        if native['warmup_seconds'] != wsi['warmup_seconds']:
            raise ValueError('warm-up differs between compared captures')
        pair = {'requested_hz': hz, 'load': load, 'repeat': repeat,
                'native_directory': native['directory'], 'wsi_directory': wsi['directory'],
                'deltas_wsi_minus_native': {}}
        for name, (metric, percentile) in METRICS.items():
            a = native[metric][percentile] if percentile else native[metric]
            b = wsi[metric][percentile] if percentile else wsi[metric]
            pair['deltas_wsi_minus_native'][name] = b - a if a is not None and b is not None else None
        pair['present_rate_change_percent'] = (wsi['observed_present_hz'] / native['observed_present_hz'] - 1) * 100
        pairs.append(pair)
    return pairs


def source_metrics(run, warmup):
    """Join display feedback to service-observed client readiness, not predicted pose time."""
    directory = pathlib.Path(run['directory'])
    projection = [r for r in table(directory, 'reprojection') if int(r['source_valid'])]
    start = int(projection[0]['sample_ns']) + warmup * 1e9
    samples = {int(r['system_frame_id']): int(r['sample_ns']) for r in projection if int(r['sample_ns']) >= start}
    frames = set(samples)
    source = table(directory, 'client_frame_map')
    if len({r['client_slot'] for r in source}) != 1:
        raise ValueError('client readiness join requires exactly one active client slot')
    source = {int(r['system_frame_id']): r for r in source if int(r['system_frame_id']) in frames}
    ready = {int(r['client_frame_id']): int(r['event_ns']) for r in table(directory, 'client_gpu')
             if r['event'] == 'semaphore_ready'}
    delivered = {int(r['client_frame_id']): int(r['event_ns']) for r in table(directory, 'app_pacing')
                 if r['event'] == 'delivered'}
    if run['condition'] == 'native':
        actual = {int(r['frame_id']): int(r['presented_monotonic_ns']) for r in table(directory, 'presented')
                  if int(r['presented_monotonic_ns']) > 0}
    else:
        actual = {int(r['frame_id']): int(r['actual_present_ns']) for r in table(directory, 'wsi')
                  if r['event'] == 'feedback' and int(r['actual_present_ns']) > 0}
    ready_age, delivered_age, uses, presented, ready_to_sample = [], [], [], [], []
    for frame, row in source.items():
        if frame not in actual:
            continue
        client = int(row['client_frame_id'])
        if client not in ready or client not in delivered:
            continue
        if actual[frame] < ready[client] or actual[frame] < delivered[client]:
            raise ValueError('client readiness/delivery is after reported presentation')
        if samples[frame] < ready[client]:
            raise ValueError('client readiness is after compositor rendering sample')
        ready_to_sample.append((samples[frame] - ready[client]) / 1e6)
        ready_age.append((actual[frame] - ready[client]) / 1e6)
        delivered_age.append((actual[frame] - delivered[client]) / 1e6)
        uses.append(int(row['source_use_ordinal']))
        presented.append((actual[frame], client))
    if not ready_age:
        raise ValueError('no client readiness frames join to presentation')
    expected = sum(frame in actual for frame in frames)
    coverage = 100 * len(ready_age) / expected if expected else 0
    if coverage < 99:
        raise ValueError('client readiness joins cover less than 99 percent of presentations')
    presented.sort()
    repeat_count, longest, current = 0, 1, 1
    for (_, previous), (_, client) in zip(presented, presented[1:]):
        current = current + 1 if client == previous else 1
        repeat_count += client == previous
        longest = max(longest, current)
    span = presented[-1][0] - presented[0][0]
    fresh_hz = (len({client for _, client in presented}) - 1) * 1e9 / span if span > 0 else None
    return {'presented_unique_client_hz': fresh_hz,
            'presented_reuse_percent': 100 * repeat_count / len(presented),
            'max_presented_source_run': longest,
            'client_ready_to_sample_ms': distribution(ready_to_sample),
            'client_ready_to_present_ms': distribution(ready_age),
            'client_delivery_to_present_ms': distribution(delivered_age),
            'max_presented_source_latch_ordinal': max(uses),
            'readiness_joined_presentations': len(ready_age),
            'readiness_join_coverage_percent': coverage}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=pathlib.Path)
    parser.add_argument('--warmup-seconds', type=float, default=2)
    args = parser.parse_args()
    if args.warmup_seconds < 0:
        parser.error('warm-up must be nonnegative')
    manifest = json.loads(args.manifest.read_text())
    results = [summarize(run, args.warmup_seconds) for run in manifest['runs']]
    sources = [{'directory': run['directory'], **source_metrics(run, args.warmup_seconds)} for run in manifest['runs']]
    print(json.dumps({'pairs': pair_results(manifest, results), 'results': results, 'client_sources': sources}, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
