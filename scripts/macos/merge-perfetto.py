#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Join same-host captures using Perfetto's namespace-aware archive importer."""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--processor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, help='Explicit machine/clock mapping for other clock domains')
    parser.add_argument('traces', type=Path, nargs='+')
    args = parser.parse_args()
    if len({path.name for path in args.traces}) != len(args.traces):
        parser.error('Capture basenames must be distinct inside the archive')
    if not args.manifest:
        def query(path, sql):
            result = subprocess.run([str(args.processor), 'query', '--full-sort', str(path), sql],
                                    check=True, capture_output=True, text=True, timeout=20)
            return list(csv.DictReader(io.StringIO(result.stdout)))
        native = []
        boots = set()
        for path in args.traces:
            samples = query(path, "SELECT ts, EXTRACT_ARG(arg_set_id, 'debug.mach_absolute_ns') AS mach_ns, "
                                  "EXTRACT_ARG(arg_set_id, 'debug.uncertainty_ns') AS uncertainty_ns "
                                  "FROM slice WHERE name='MonadoClockCorrelation'")
            if not samples:
                continue
            if any(row['uncertainty_ns'] in ('[NULL]', '') or
                   abs(int(row['ts']) - int(row['mach_ns'])) > int(row['uncertainty_ns']) for row in samples):
                parser.error(f'{path}: missing or invalid native-to-Mach clock correlation; use a calibrated manifest')
            context = query(path, "SELECT EXTRACT_ARG(arg_set_id, 'debug.boot_id') AS boot_id "
                                  "FROM slice WHERE name='MonadoCaptureContext'")
            if not context or not context[0]['boot_id']:
                parser.error(f'{path}: missing boot provenance; provide an explicit manifest')
            boots.add(context[0]['boot_id'])
            native.append(path)
        if not native or len(boots) != 1:
            parser.error('Automatic joining requires calibrated Monado captures from one boot; otherwise provide --manifest')
        # Both pinned macOS SDKs use Mach uptime as their SDK clock. Monado
        # records its true CLOCK_MONOTONIC under a separate clock ID and emits
        # snapshots to MONOTONIC_RAW/Mach. Never guess a fixed sleep offset.
        args.manifest = args.output.with_suffix('.manifest.json')
        manifest = {'perfetto_manifest': {'version': 1,
            'trace_time': {'clock': 'MONOTONIC_RAW', 'file': native[0].name},
            'files': [{'path': path.name, 'machine': {'name': next(iter(boots))},
                       'clocks': {'clock': 'BOOTTIME',
                                  'sync_to': {'file': native[0].name, 'clock': 'MONOTONIC_RAW'},
                                  'offset_ns': 0}} for path in args.traces],
            'attributes': {'timeline': 'Mach absolute nanoseconds',
                           'native_clock': 'Monado native clock with captured snapshots',
                           'provenance': 'Caller-supplied same-host Chromium/native SDK captures',
                           'sdk_clock_alias': 'macOS SDK BOOTTIME is Mach uptime, zero offset to MONOTONIC_RAW'}}}
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2) + '\n')
    command = [str(args.processor), 'util', 'merge', '--strict', '-o', str(args.output)]
    if args.manifest:
        command += ['--manifest', str(args.manifest)]
    subprocess.run(command + [str(path) for path in args.traces], check=True, timeout=60)
    print(f'Merged capture: {args.output}')


if __name__ == '__main__':
    main()
