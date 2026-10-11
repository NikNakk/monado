#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare physical presentation traces from matched native service runs."""
import argparse
import csv
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {name: values[min(len(values)-1, math.ceil(len(values)*fraction)-1)]
            for name, fraction in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}


def rows(path, suffix):
    with next(path.glob('*'+suffix+'.csv')).open() as file:
        return list(csv.DictReader(file))


def summarize(path, warmup):
    events = [json.loads(line) for line in (path/'client.txt').read_text().splitlines()
              if line.startswith('{')]
    frames = {row['frame']: row for row in events if row['event'] == 'frame'}
    measured = {key: row for key, row in frames.items() if key > warmup}
    if len(measured) < 2:
        raise RuntimeError('Insufficient measured client frames: '+str(path))
    mapping = {row['system_frame_id']: row for row in rows(path, 'client_frame_map')
               if row['client_slot'] == '0' and int(row['client_frame_id']) in measured}
    presented_all = rows(path, 'presented')
    presented = [row for row in presented_all if row['frame_id'] in mapping
                 and int(row['presented_monotonic_ns']) > 0]
    presented.sort(key=lambda row: int(row['presented_monotonic_ns']))
    times = [int(row['presented_monotonic_ns']) for row in presented]
    if len(times) < 2:
        raise RuntimeError('Insufficient physical presentations: '+str(path))
    cadence = [(b-a)/1e6 for a, b in zip(times, times[1:])]
    # Count each new client frame once, at its first confirmed physical presentation.
    fresh = {}
    for row in presented:
        source = mapping[row['frame_id']]
        client_id = int(source['client_frame_id'])
        # An initial latch can be superseded before presentation; a later reuse
        # may be this client frame's first actual appearance on the display.
        fresh.setdefault(client_id, row)
    entry_latency = [(int(row['presented_monotonic_ns'])-frames[key]['begin_ns'])/1000
                     for key, row in fresh.items()]
    exit_latency = [(int(row['presented_monotonic_ns'])-frames[key]['end_ns'])/1000
                    for key, row in fresh.items()]
    complete = rows(path, 'present_complete')
    stages = rows(path, 'renderer_stage')
    ready = rows(path, 'client_gpu')
    client_times = sorted(row['end_ns'] for row in measured.values())
    result = dict(run=path.name, client_frames=len(frames), warmup_client_frames=warmup,
                  measured_client_frames=len(measured), confirmed_presentations=len(presented),
                  zero_timestamp_callbacks=sum(int(row['presented_monotonic_ns']) == 0 for row in presented_all),
                  physical_throughput_hz=(len(times)-1)*1e9/(times[-1]-times[0]),
                  client_throughput_hz=(len(client_times)-1)*1e9/(client_times[-1]-client_times[0]),
                  cadence_ms=distribution(cadence),
                  cadence_gaps_over_12_5_ms=sum(gap > 12.5 for gap in cadence),
                  cadence_gaps_at_least_20_ms=sum(gap >= 20 for gap in cadence),
                  estimated_skipped_refreshes=sum(max(round(gap/(1000/120))-1, 0) for gap in cadence),
                  distinct_client_frames_physically_presented=len(fresh),
                  client_frames_without_confirmed_presentation=len(measured)-len(fresh),
                  end_frame_entry_to_physical_us=distribution(entry_latency),
                  end_frame_exit_to_physical_us=distribution(exit_latency),
                  end_frame_us=distribution([(row['end_ns']-row['begin_ns'])/1000 for row in measured.values()]),
                  readiness_wait_us=distribution([int(row['duration_ns'])/1000 for row in ready
                      if row['event'] == 'semaphore_ready' and int(row['client_frame_id']) in measured]),
                  presentation_gpu_errors=sum(row['status'] != '4' for row in complete),
                  presentation_without_shared_event=sum(row['shared_event_wait'] != '1' for row in complete),
                  presentation_gpu_work_us=distribution([(float(row['gpu_end_time_s'])-float(row['gpu_start_time_s']))*1e6
                      for row in complete if row['frame_id'] in mapping and float(row['gpu_end_time_s']) > 0]),
                  config=next(row for row in events if row['event'] == 'config'),
                  client_result=next(row for row in events if row['event'] == 'result'))
    for stage in ['queue_submit', 'draw_dispatch', 'previous_fence_wait']:
        result[stage+'_us'] = distribution([(int(row['end_ns'])-int(row['begin_ns']))/1000
            for row in stages if row['stage'] == stage and row['frame_id'] in mapping])
    if 'vk-target frame=' in (path/'service.log').read_text() or 'producer ready wait:' in (path/'service.log').read_text():
        raise RuntimeError('Readback/per-image diagnostics were not disabled: '+str(path))
    if result['presentation_gpu_errors'] or result['presentation_without_shared_event']:
        raise RuntimeError('Presentation GPU failure or missing dependency: '+str(path))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--warmup', type=int, default=120)
    args = parser.parse_args()
    results = [summarize(args.root/name, args.warmup)
               for name in ['baseline-0', 'modified-0', 'modified-1', 'baseline-1']]
    (args.root/'presentation-distributions.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(results, indent=2))
