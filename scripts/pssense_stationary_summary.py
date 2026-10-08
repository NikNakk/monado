#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Read-only validation of stationary settings runs; no tracker or LED identities.

Reuse the mask diagnostic's compact-point detector on unamplified camera-0 ROI
pixels. Keep trial controls, complete wire integrity and incomplete stages
visible. Multiple roots allow an explicitly resumed immutable plan.
"""
import argparse
import collections
import csv
import gzip
import json
from pathlib import Path
import zlib

import cv2
import numpy as np
from psvr2_tracking_mask_analyze import compact_bright_centroids


def summarize(path):
    wire = collections.Counter()
    stages, current, pending, latest = [], None, None, None
    previous = {}
    started = None
    ended = False
    for line in path.open():
        r = json.loads(line)
        event = r['event']
        if event == 'run_start':
            started = r
        elif event == 'run_end':
            ended = True
        elif event == 'settings_rebased':
            pending = r
        elif event == 'action':
            if 'index' in r:
                current = dict(index=r['index'], plan_index=started['arguments'].get('start_step', 0) + r['index'],
                               label=r['label'], led_sequence=r['led_sequence'], complete=False,
                               setting=pending, first_prescan_lead_ns=None, settings_mismatch=0)
                stages.append(current)
            else:
                current = None
        elif event in ('hid_rx', 'hid_tx'):
            data = bytes.fromhex(r['data_hex'])
            seed = 0xa1 if event == 'hid_rx' else 0xa2
            valid = len(data) == 78 and zlib.crc32(data[:-4], zlib.crc32(bytes([seed]))) == int.from_bytes(data[-4:], 'little')
            wire[event] += 1
            wire[event + '_crc_invalid'] += not valid
            wire[event + '_io_error'] += r['result'] != 0
            if event == 'hid_rx' and valid:
                latest = (int.from_bytes(data[49:53], 'little'), r['callback_monotonic_ns'])
            elif event == 'hid_tx' and valid:
                key = r['device']
                if key in previous:
                    nibble, counter = previous[key]
                    wire['transport_sequence_errors'] += data[1] >> 4 != (nibble + 1) % 16
                    wire['transport_counter_errors'] += data[41] != (counter + 1) % 256
                previous[key] = data[1] >> 4, data[41]
                if current and current['setting']:
                    expected = bytes.fromhex(current['setting']['rebased_settings_hex'])
                    actual = data[3:41]
                    current['settings_mismatch'] += any(actual[i] != expected[i] for i in range(38)
                        if i not in range(15, 19) and i != 20) or actual[20] != current['led_sequence']
                    if actual[19] == 1 and latest and current['first_prescan_lead_ns'] is None:
                        age_ns = r['before_monotonic_ns'] - latest[1]
                        now_ticks = (latest[0] + age_ns * 3 // 1000) & 0xffffffff
                        delta = ((int.from_bytes(actual[22:26], 'little') - now_ticks + 2**31) % 2**32) - 2**31
                        current['first_prescan_lead_ns'] = delta * 1000 // 3
                        current['first_clock_age_ns'] = age_ns
        elif event == 'step_end' and current:
            current.update(complete=True, sent=r['sent'], ok=r['ok'])
        elif event in ('off_gate', 'prescan_gate') and current:
            current['gate'] = {k: r[k] for k in ('event', 'roi', 'populated_fraction', 'frame_count')}

    roi = started['arguments'].get('off_roi')
    for stage in stages:
        directory = path.parent / f"{stage['index']:02d}-{stage['label']}"
        frames = []
        if roi and stage['complete']:
            x0, y0, x1, y1 = roi
            for p in sorted([*directory.glob('*set-4-example-*-plane0.pgm'),
                             *directory.glob('*set-4-example-*-plane0.png')]):
                image = cv2.imread(str(p), 0)[y0:y1, x0:x1]
                frames.append(dict(file=p.name, bright_pixels=int((image > 100).sum()),
                    compact_points=len(compact_bright_centroids(image, 100)), max_dn=int(image.max())))
        stage['camera0_roi_frames'] = frames
        stage['ring_frame_fraction'] = sum(f['bright_pixels'] > 10 for f in frames) / len(frames) if frames else None
        stage['median_compact_points'] = float(np.median([f['compact_points'] for f in frames])) if frames else None
        gate = stage.get('gate')
        gate_pass = None
        if gate:
            fraction = gate['populated_fraction']
            gate_pass = fraction is not None and (fraction <= .25 if gate['event'] == 'off_gate' else fraction >= .5)
        stage['gate_pass'] = gate_pass
        stage['accepted'] = bool(stage['complete'] and stage.get('sent', 0) > 0 and
                                 stage.get('ok') == stage.get('sent') and
                                 not stage['settings_mismatch'] and gate_pass is not False)
        lengths = collections.Counter()
        if (directory / 'if8.csv').exists():
            for row in csv.DictReader((directory / 'if8.csv').open()):
                lengths[int(row['length'])] += 1
        stage['if8_read_lengths'] = dict(lengths)
        stage['camera_capture_quality'] = {}
        for manifest in directory.glob('*packets.csv'):
            by_set = collections.defaultdict(list)
            for row in csv.DictReader(manifest.open()):
                by_set[int(row['camera_set'])].append(row)
            for camera_set, rows in by_set.items():
                increments = [(int(b['sequence_id'])-int(a['sequence_id'])) & 0xffffffff
                              for a, b in zip(rows, rows[1:])]
                intervals = [(int(b['vts_us'])-int(a['vts_us'])) & 0xffffffff
                             for a, b in zip(rows, rows[1:])]
                monotonic_sequence = all(0 < n < 2**31 for n in increments)
                span = sum(increments)+1 if monotonic_sequence else None
                stage['camera_capture_quality'][str(camera_set)] = dict(
                    received_packets=len(rows), sequence_increment_counts=dict(collections.Counter(increments)),
                    median_received_vts_interval_us=float(np.median(intervals)) if intervals else None,
                    missing_sequence_fraction=1-len(rows)/span if span else None,
                    monotonic_sequence=monotonic_sequence)
        # Validate one raw compressed example against its manifest byte length.
        for manifest in directory.glob('*packets.csv'):
            for row in csv.DictReader(manifest.open()):
                if row['raw_file']:
                    raw = directory / row['raw_file']
                    payload = gzip.decompress(raw.read_bytes()) if raw.suffix == '.gz' else raw.read_bytes()
                    if len(payload) != int(row['size']) or payload[:2] != b'VI':
                        raise ValueError(f'raw example/manifest mismatch: {raw}')
                    break
    return dict(reports=str(path), side=started['side'], arguments=started['arguments'],
                run_end=ended, wire=dict(wire), stages=stages)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('roots', type=Path, nargs='+')
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    out = args.out.expanduser().resolve()
    if str(out).startswith(('/tmp', '/private/tmp')) or out.exists():
        p.error('use a new persistent output directory')
    logs = sorted({f for root in args.roots for f in root.rglob('reports.jsonl')})
    runs = [summarize(f) for f in logs]
    out.mkdir(parents=True)
    (out / 'summary.json').write_text(json.dumps(dict(runs=runs,
        limitations=['Camera-0 ROI only; points are unlabeled image components, not semantic LED identities.',
                     'Receipt-biased controller lead estimates are not measured emission latency.',
                     'Keep failed and interrupted runs separate from passing shuffled trials.']), indent=2) + '\n')
    fields = ['side', 'reports', 'plan_index', 'label', 'complete', 'accepted', 'gate_pass', 'frame_count', 'ring_frame_fraction',
              'median_compact_points', 'first_prescan_lead_ns', 'first_clock_age_ns', 'settings_mismatch']
    with (out / 'stages.csv').open('x') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for run in runs:
            for s in run['stages']:
                row = {k: s.get(k) for k in fields}
                row.update(side=run['side'], reports=run['reports'], frame_count=len(s['camera0_roi_frames']))
                w.writerow(row)
    print(json.dumps(dict(out=str(out), runs=len(runs), complete_stages=sum(s['complete'] for r in runs for s in r['stages']))))


if __name__ == '__main__':
    main()
