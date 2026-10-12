#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0
"""Check a Chromium video fixture, joined trace and native fresh-latch evidence."""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess


def query(processor, trace, sql):
    run = subprocess.run([str(processor), str(trace), '-Q', sql],
                         capture_output=True, text=True, check=True, timeout=30)
    return list(csv.DictReader(io.StringIO(run.stdout)))


def check(directory, processor):
    summary = json.loads((directory/'video-summary.json').read_text())
    assert summary['status'] == 'PASS', summary.get('error', summary)
    assert summary['gpuCrash'], 'GPU failure was not exercised'
    assert summary['nativeHealthy']['exit'] == 0
    assert summary['nativeHealthy']['frames'] == 1200
    expected = ['playing', 'paused', 'buffering', 'prolonged_buffering', 'resumed',
                'seek', 'rapid_pause_resume', 'replacement_session', 'gpu_recovery_replacement']
    assert [row['phase'] for row in summary['phases']] == expected
    for phase in summary['phases']:
        assert phase['after']['frames']-phase['before']['frames'] >= 5, phase['phase']
        assert phase['after']['healthyFrames']-phase['before']['healthyFrames'] >= 5, phase['phase']
    buffered = next(row for row in summary['phases'] if row['phase'] == 'prolonged_buffering')
    assert buffered['after']['decoded'] == buffered['before']['decoded']
    assert summary['afterGpuCrash']['healthyFrames']-summary['nativeFramesAtGpuCrash'] >= 5

    archive = directory/'joined.tar'
    merge = Path(__file__).with_name('merge-perfetto.py')
    subprocess.run(['python3', str(merge), '--processor', str(processor),
                    '--output', str(archive), *map(str, sorted(directory.glob('monado_*.pftrace'))),
                    str(directory/'chromium.pftrace')], check=True, timeout=60)
    faults = query(processor, archive,
                   "SELECT name,value FROM stats WHERE severity IN ('error','data_loss') AND value<>0")
    assert not faults, faults
    # Use the service's own two-point calibration for its CSV timestamps.
    # CLOCK_MONOTONIC can slew relative to Mach raw time; a constant offset
    # across processes/start/end can create a false drift failure.
    service_trace = None
    for candidate in sorted(directory.glob('monado_*.pftrace')):
        count = query(processor, candidate,
                      "SELECT count(*) n FROM slice WHERE name='producer_obligation'")
        if int(count[0]['n']) > 0:
            assert service_trace is None, 'Multiple service ownership captures'
            service_trace = candidate
    assert service_trace is not None
    footer = query(processor, service_trace, """
      SELECT EXTRACT_ARG(arg_set_id,'debug.flush_acknowledged') ack
      FROM slice WHERE name='MonadoCaptureEnd'
    """)
    assert len(footer) == 1 and footer[0]['ack'] == '1', 'Service capture is incomplete'
    correlations = query(processor, service_trace, """
      SELECT EXTRACT_ARG(arg_set_id,'debug.native_ns') native_ns,
             EXTRACT_ARG(arg_set_id,'debug.mach_absolute_ns') mach_ns,
             EXTRACT_ARG(arg_set_id,'debug.uncertainty_ns') uncertainty_ns
      FROM slice WHERE name='MonadoClockCorrelation' ORDER BY ts
    """)
    assert len(correlations) == 2, 'Complete service start/end calibration required'
    first, last = correlations
    n0, n1 = int(first['native_ns']), int(last['native_ns'])
    m0, m1 = int(first['mach_ns']), int(last['mach_ns'])
    assert n1 > n0 and m1 > m0
    # This fixture has no suspend/resume. Reject large discontinuities, and
    # retain the measured slope/uncertainty with the derived latch counts.
    assert abs((m1-m0)/(n1-n0)-1) < 0.001, 'Clock discontinuity exceeds fixture bound'
    def normalize(timestamp):
        assert n0 <= timestamp <= n1, 'CSV timestamp outside calibration'
        return m0+(timestamp-n0)*(m1-m0)//(n1-n0)
    marks = query(processor, archive, """
      SELECT name,ts FROM slice WHERE category='blink.user_timing'
      AND name GLOB 'monado-test-phase-*' ORDER BY ts
    """)
    assert marks, 'Browser phase markers are absent'
    marks = {row['name'].removeprefix('monado-test-phase-'): int(row['ts']) for row in marks}
    latches = list(csv.DictReader(next(directory.glob('*client_frame_map.csv')).open()))
    fresh = [normalize(int(row['latch_ns'])) for row in latches
             if row['client_slot'] == '0' and row['reused'] == '0']
    assert fresh
    counts = {}
    for phase, finish in [('prolonged_buffering', 'data_appended'),
                          ('gpu_crash', 'gpu_recovery_replacement')]:
        assert phase in marks and finish in marks, (phase, finish)
        count = sum(marks[phase] <= timestamp <= marks[finish] for timestamp in fresh)
        assert count >= 5, f'{phase}: only {count} fresh native latches'
        counts[phase] = count
    storage = query(processor, archive, """
      SELECT name,count(*) n FROM slice WHERE name IN
      ('producer_obligation','consumer_gpu_observed','storage_quarantined','storage_reclaimed')
      GROUP BY name
    """)
    storage = {row['name']: int(row['n']) for row in storage}
    assert all(storage.get(name, 0) > 0 for name in
               ['producer_obligation', 'consumer_gpu_observed', 'storage_quarantined', 'storage_reclaimed'])
    service = (directory/'service.log').read_text()
    assert service.count('starting up...') == 1, 'Service startup count changed'
    assert 'VK_ERROR_DEVICE_LOST' not in service and 'VK_NOT_READY' not in service, 'Service GPU failed'
    result = dict(status='PASS', scope='Offscreen transitions/recovery; not exact handoff pixel or performance validation',
                  native_frames=1200, fresh_latches=len(fresh), fault_fresh_latches=counts,
                  ownership_events=storage, trace_errors=faults,
                  clock_calibration=correlations, clock_rate_ppm=((m1-m0)/(n1-n0)-1)*1e6)
    (directory/'validation.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--processor', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.directory, args.processor), indent=2))
