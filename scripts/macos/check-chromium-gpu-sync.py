#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
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


def check_ownership(processor, service_trace):
    from collections import Counter
    rows = query(processor, service_trace, """
      SELECT name, EXTRACT_ARG(arg_set_id,'debug.scope_id') scope,
             EXTRACT_ARG(arg_set_id,'debug.image_index') image,
             EXTRACT_ARG(arg_set_id,'debug.event_ns') event_ns
      FROM slice WHERE name IN
        ('consumer_claimed','consumer_claim_released','storage_quarantined','storage_reclaimed')
        AND EXTRACT_ARG(arg_set_id,'debug.record_type')='observation'
      ORDER BY CAST(EXTRACT_ARG(arg_set_id,'debug.event_ns') AS INTEGER),id
    """)
    assert rows, 'Ownership observations absent'
    pending = Counter()
    reclaimed = set()
    counts = Counter()
    for row in rows:
        scope, image = int(row['scope']), int(row['image'])
        name = row['name']
        counts[name] += 1
        assert scope not in reclaimed, 'Ownership event after storage reclamation'
        if name == 'consumer_claimed':
            pending[(scope,image)] += 1
        elif name == 'consumer_claim_released':
            assert pending[(scope,image)] > 0, 'Released nonexistent consumer claim'
            pending[(scope,image)] -= 1
        elif name == 'storage_reclaimed':
            assert all(n==0 for (owner,_),n in pending.items() if owner==scope), 'Storage reclaimed with outstanding CPU consumer claims'
            reclaimed.add(scope)
    assert counts['consumer_claimed'] > 0
    assert all(n==0 for n in pending.values()), 'Unreleased consumer claims in complete service capture'
    # Claim release is logical ownership only. It does not prove GPU completion;
    # the actual Vulkan readback and production GPU reuse tests provide that.
    return dict(observations=dict(counts), consumer_claims_balanced=True,
                no_claims_at_reclamation=True)


def check_transport(directory, summary):
    log = (directory/'chromium.log').read_text()
    expected = ('iosurface-render-copy' if summary['forceCopy'] or
                summary.get('producer', 'angle') == 'dawn' else 'monado-iosurface-zero-copy')
    marker = 'transport='+expected
    assert marker in log, 'Requested transport was not demonstrated: '+expected
    return dict(expected=expected, observed_creations=log.count(marker))


def check_trace_scope(processor, trace, summary):
    rows = query(processor, trace, """
      SELECT name,count(*) n FROM slice WHERE name IN
      ('OpenXrRenderLoop::GetFrameData','CompositeAndSubmit') GROUP BY name
    """)
    counts = {row['name']: int(row['n']) for row in rows}
    if summary.get('traceScope') == 'browser':
        assert all(counts.get(name, 0) > 0 for name in
                   ['OpenXrRenderLoop::GetFrameData', 'CompositeAndSubmit']), \
            'Browser capture omitted the XR utility process'
    return dict(scope=summary.get('traceScope', 'legacy page scope'),
                xr_utility_events=counts)


def check_auxiliary(directory, processor, summary):
    from collections import Counter
    from math import ceil
    def distribution(values):
        values = sorted(values)
        assert values, 'Required timing samples absent'
        return {name: values[min(len(values)-1, ceil(len(values)*fraction)-1)]
                for name, fraction in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}
    archive = directory/'joined.tar'
    subprocess.run(['python3', str(Path(__file__).with_name('merge-perfetto.py')),
                    '--processor', str(processor), '--output', str(archive),
                    *map(str, sorted(directory.glob('monado_*.pftrace'))),
                    str(directory/'chromium.pftrace')], check=True, timeout=60)
    faults = query(processor, archive,
                  "SELECT name,value FROM stats WHERE value<>0 AND (severity IN ('error','data_loss') OR name GLOB '*discard*' OR name GLOB '*overwrit*' OR name GLOB '*loss*')")
    assert not faults, faults
    service = (directory/'service.log').read_text()
    startups = service.count('starting up...')
    assert startups <= 1 if summary['mode']=='benchmark' else startups == 1
    assert 'VK_ERROR_DEVICE_LOST' not in service and 'VK_NOT_READY' not in service
    service_trace = None
    for candidate in sorted(directory.glob('monado_*.pftrace')):
        count = query(processor, candidate, "SELECT count(*) n FROM slice WHERE name='producer_obligation'")
        if int(count[0]['n']):
            assert service_trace is None
            service_trace = candidate
    assert service_trace is not None
    footer = query(processor, service_trace, "SELECT EXTRACT_ARG(arg_set_id,'debug.flush_acknowledged') ack FROM slice WHERE name='MonadoCaptureEnd'")
    assert len(footer) == 1 and footer[0]['ack'] == '1'
    acquire = list(csv.DictReader(next(directory.glob('*ipc_swapchain.csv')).open()))
    reuse = Counter((row['swapchain_id'], row['image_index']) for row in acquire if row['event'] == 'acquire' and row['result'] == '0')
    assert sum(n >= 10 for n in reuse.values()) >= 3, 'Transport images were not repeatedly recycled'
    result = dict(status='PASS', mode=summary['mode'], producer=summary.get('producer', 'angle'),
                  force_copy=summary['forceCopy'], image_acquisitions={f'{s}:{i}': n for (s,i),n in reuse.items()},
                  trace_errors=faults, graceful_service_capture=True,
                  transport=check_transport(directory, summary),
                  ownership=check_ownership(processor, service_trace),
                  trace_scope=check_trace_scope(processor, archive, summary))
    if summary['mode'] == 'pixels':
        expected = [([255,0,0,255],[0,0,255,255]), ([0,255,0,255],[255,0,0,255]),
                    ([0,0,255,255],[0,255,0,255]), ([255,0,0,255],[0,0,255,255])]
        proof = summary['pixelProof']
        assert len(proof) == 4
        for phase, color in zip(proof, expected):
            assert phase['expected'] == dict(left=color[0],right=color[1]) and phase['samples']
            for sample in phase['samples']:
                for eye in ['left', 'right']:
                    assert all(abs(a-b) <= 1 for a,b in zip(sample[eye], phase['expected'][eye])), sample
        result['pixel_proof'] = proof
        result['scope'] = 'Exact GPU-written colors after ANGLE/Dawn SharedImage transport, Vulkan composition and GPU-completed output readback; sampled eye interiors'
    else:
        assert summary['mode'] == 'benchmark'
        phase = next(row for row in summary['phases'] if row['phase'] == 'normal_path')
        elapsed_ms = phase['after']['perfNow']-phase['before']['perfNow']
        result['xr_callback_hz'] = (phase['after']['frames']-phase['before']['frames'])*1000/elapsed_ms
        assert phase['after']['decoded']-phase['before']['decoded'] >= 100, 'Normal path stopped decoding'
        assert 'vk-target' not in service and 'vk-source' not in service, 'Readbacks enabled during benchmark'
        assert result['xr_callback_hz'] >= 45, result
        correlations = query(processor, service_trace, "SELECT EXTRACT_ARG(arg_set_id,'debug.native_ns') native_ns, EXTRACT_ARG(arg_set_id,'debug.mach_absolute_ns') mach_ns FROM slice WHERE name='MonadoClockCorrelation' ORDER BY ts")
        assert len(correlations) == 2
        n0,n1 = [int(row['native_ns']) for row in correlations]
        m0,m1 = [int(row['mach_ns']) for row in correlations]
        assert n1>n0 and m1>m0 and abs((m1-m0)/(n1-n0)-1)<.001
        marks = query(processor, archive, "SELECT name,ts FROM slice WHERE category='blink.user_timing' AND name IN ('monado-test-phase-normal_path','monado-test-phase-normal_path_end')")
        marks = {row['name']:int(row['ts']) for row in marks}
        begin,end = marks['monado-test-phase-normal_path'],marks['monado-test-phase-normal_path_end']
        def measured(native):
            return begin <= m0+(int(native)-n0)*(m1-m0)//(n1-n0) < end
        ready = list(csv.DictReader(next(directory.glob('*client_gpu.csv')).open()))
        result['cpu_readiness_wait_us'] = distribution([int(row['duration_ns'])/1000 for row in ready if row['event']=='semaphore_ready' and measured(row['event_ns'])])
        submits = list(csv.DictReader(next(directory.glob('*ipc_submit.csv')).open()))
        entries = {row['client_frame_id']:int(row['event_ns']) for row in submits if row['event']=='handler_entry'}
        result['ipc_handler_us'] = distribution([(int(row['event_ns'])-entries[row['client_frame_id']])/1000 for row in submits if row['event']=='after_commit' and row['client_frame_id'] in entries and measured(row['event_ns'])])
        mapping = list(csv.DictReader(next(directory.glob('*client_frame_map.csv')).open()))
        fresh = [row for row in mapping if row['reused']=='0' and measured(row['latch_ns'])]
        result['fresh_latches'] = len(fresh)
        result['fresh_latch_hz'] = len(fresh)*1e9/(end-begin)
        result['handler_entry_to_latch_us'] = distribution([(int(row['latch_ns'])-entries[row['client_frame_id']])/1000 for row in fresh if row['client_frame_id'] in entries])
        stages = list(csv.DictReader(next(directory.glob('*renderer_stage.csv')).open()))
        for stage in ['previous_fence_wait', 'queue_submit', 'draw_dispatch']:
            result[stage+'_us'] = distribution([(int(row['end_ns'])-int(row['begin_ns']))/1000 for row in stages if row['stage']==stage and measured(row['end_ns'])])
        wait_counts = query(processor, archive, f"SELECT name,count(*) n FROM slice WHERE ts>={begin} AND ts<{end} AND name IN ('OpenXRMetalFenceQuery','OpenXRMetalSharedEventWait','OpenXRMetalSharedEventNoWait','OpenXRMetalSharedEventTimeout','OpenXRMetalFenceQueryFailed') GROUP BY name")
        result['readiness_trace_counts'] = {row['name']:int(row['n']) for row in wait_counts}
        assert result['readiness_trace_counts'].get('OpenXRMetalFenceQuery',0) >= 100, 'Actual GPU fence queries absent'
        assert not result['readiness_trace_counts'].get('OpenXRMetalSharedEventTimeout',0)
        assert not result['readiness_trace_counts'].get('OpenXRMetalFenceQueryFailed',0)
        slices = query(processor, archive, f"SELECT name,dur FROM slice WHERE ts>={begin} AND ts<{end} AND dur>=0 AND name IN ('OpenXRMetalSharedEventWait','ExecuteRAFCallbacks','XRFrameTransport::SubmitFrameDrawnIntoTexture')")
        for name in ['OpenXRMetalSharedEventWait','ExecuteRAFCallbacks','XRFrameTransport::SubmitFrameDrawnIntoTexture']:
            values = [int(row['dur'])/1000 for row in slices if row['name']==name]
            result[name+'_us'] = distribution(values) if values else None
        thermal = [json.loads(line) for line in (directory/'thermal.jsonl').read_text().splitlines()]
        assert len(thermal) >= 3 and all(row['exit']==0 for row in thermal), 'Thermal telemetry incomplete'
        result['thermal_samples'] = [dict(epoch=row['epoch'], **json.loads(row['output'])) for row in thermal]
        result['scope'] = '20 second normal-path measurement after warmup; same-clock durations; phase windows calibrated to service clocks; readbacks disabled by wrapper'
    (directory/'validation.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def check(directory, processor):
    summary = json.loads((directory/'video-summary.json').read_text())
    assert summary['status'] == 'PASS', summary.get('error', summary)
    if summary.get('mode', 'faults') != 'faults':
        return check_auxiliary(directory, processor, summary)
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
                   "SELECT name,value FROM stats WHERE value<>0 AND (severity IN ('error','data_loss') OR name GLOB '*discard*' OR name GLOB '*overwrit*' OR name GLOB '*loss*')")
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
                  transport=check_transport(directory, summary),
                  ownership=check_ownership(processor, service_trace),
                  native_frames=1200, fresh_latches=len(fresh), fault_fresh_latches=counts,
                  ownership_events=storage, trace_errors=faults,
                  clock_calibration=correlations, clock_rate_ppm=((m1-m0)/(n1-n0)-1)*1e6,
                  trace_scope=check_trace_scope(processor, archive, summary))
    (directory/'validation.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--processor', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.directory, args.processor), indent=2))
