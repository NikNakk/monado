#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check the supplied observable IF8 candidate partition, without decoding records.

Candidate: 64 + 4*(4 + 256*36) = 36,944 bytes. Compare each group's leading
little-endian integer to an independently counted number of nonzero records.
This is not a Sony private structure definition or a camera/LED identity map.
USB read boundaries are retained as reads, not asserted to be packet boundaries.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def candidate_counts(data):
    if len(data) != 36944:
        raise ValueError('candidate requires exactly 36,944 bytes')
    declared, nonzero = [], []
    for group in range(4):
        start = 64 + group * (4 + 256*36)
        declared.append(int.from_bytes(data[start:start+4], 'little'))
        nonzero.append(sum(any(data[start+4+i*36:start+4+(i+1)*36]) for i in range(256)))
    return declared, nonzero


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--stride', type=int, default=10)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    out = args.out.expanduser().resolve()
    root = args.capture.expanduser().resolve()
    if args.stride < 1 or out.exists() or str(out).startswith(('/tmp/', '/private/tmp/')):
        p.error('positive stride and a new persistent output directory required')
    windows = []
    for manifest in sorted(root.glob('*packets.csv')):
        rows = list(csv.DictReader(manifest.open()))
        if rows:
            times = [int(r['host_monotonic_ns']) for r in rows]
            windows.append((min(times), max(times), manifest.stem))
    lengths, output = collections.Counter(), []
    with (root/'if8.bin').open('rb') as raw:
        eligible = 0
        for row in csv.DictReader((root/'if8.csv').open()):
            length = int(row['length']); lengths[length] += 1
            if length != 36944:
                continue
            selected = eligible % args.stride == 0
            eligible += 1
            if not selected:
                continue
            raw.seek(int(row['offset'])); data = raw.read(length)
            declared, nonzero = candidate_counts(data)
            timestamp = int(row['host_monotonic_ns'])
            visit = next((name for lo, hi, name in windows if lo <= timestamp <= hi), 'outside_vi_sample_windows')
            output.append(dict(offset=int(row['offset']), host_monotonic_ns=timestamp,
                               host_realtime_ns=int(row['host_realtime_ns']), visit=visit,
                               leading_counts=declared, nonzero_record_counts=nonzero,
                               agrees=declared==nonzero))
    summary = dict(capture=str(root), stride=args.stride, read_lengths=dict(lengths),
                   sampled_reads=len(output), disagreements=sum(not r['agrees'] for r in output),
                   limitations=['The supplied partition is a tested wire hypothesis; record bodies remain uninterpreted.',
                                'Group indices do not identify physical cameras or LEDs.',
                                'Visit association uses host receipt windows, not exposure-time synchronization.'])
    out.mkdir(parents=True)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    with (out/'counts.jsonl').open('x') as f:
        for row in output:
            f.write(json.dumps(row)+'\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
