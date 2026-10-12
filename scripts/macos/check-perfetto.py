#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Validate native Perfetto capture and exact matching CSV observations."""
import argparse
import csv
import io
import json
import subprocess
import tempfile
from pathlib import Path


def query(processor, trace, sql):
    result = subprocess.run([str(processor), 'query', '--full-sort', str(trace), sql],
                            capture_output=True, text=True, check=True, timeout=20)
    return list(csv.DictReader(io.StringIO(result.stdout)))


FIELDS = """
name, ts, dur,
EXTRACT_ARG(arg_set_id, 'debug.scope_id') AS scope_id,
EXTRACT_ARG(arg_set_id, 'debug.frame_id') AS frame_id,
EXTRACT_ARG(arg_set_id, 'debug.event_ns') AS event_ns,
EXTRACT_ARG(arg_set_id, 'debug.timeline_value') AS timeline_value,
EXTRACT_ARG(arg_set_id, 'debug.duration_ns') AS duration_ns
"""


def check_loss(processor, trace):
    bad = query(processor, trace, """
        SELECT name, value FROM stats WHERE value != 0
        AND (severity IN ('error', 'data_loss') OR name GLOB '*discard*'
             OR name GLOB '*overwrit*' OR name GLOB '*loss*')
    """)
    if bad:
        raise RuntimeError(f'Incomplete capture {trace}: {bad}')


def check_fixture(args, directory):
    trace = directory / 'fixture.pftrace'
    rows_path = directory / 'fixture.csv'
    subprocess.run([str(args.fixture), 'dual', str(trace), str(rows_path)], check=True, timeout=15)
    check_loss(args.processor, trace)
    assert query(args.processor, trace, "SELECT COUNT(*) AS count FROM slice WHERE name = 'MonadoCaptureEnd' AND EXTRACT_ARG(arg_set_id, 'debug.flush_acknowledged') = 1") == [{'count': '1'}]
    crash = directory / 'crash.pftrace'
    subprocess.run([str(args.fixture), 'crash', str(crash), str(directory / 'crash.csv')], check=True, timeout=15)
    surviving = query(args.processor, crash, "SELECT COUNT(*) AS count FROM slice WHERE name = 'fixture_observation'")
    assert int(surviving[0]['count']) > 0, surviving
    assert query(args.processor, crash, "SELECT COUNT(*) AS count FROM slice WHERE name = 'MonadoCaptureEnd'") == [{'count': '0'}]
    threaded = Path(str(trace) + '.threaded')
    check_loss(args.processor, threaded)
    assert query(args.processor, threaded, "SELECT COUNT(*) AS count, COUNT(DISTINCT EXTRACT_ARG(arg_set_id, 'debug.scope_id')) AS scopes FROM slice WHERE name = 'threaded_observation'") == [{'count': '4000', 'scopes': '4'}]
    restart = Path(str(trace) + '.restart')
    check_loss(args.processor, restart)
    assert query(args.processor, restart, "SELECT COUNT(*) AS count FROM slice WHERE name = 'capture_restart'") == [{'count': '1'}]
    observations = query(args.processor, trace, f"""
        SELECT {FIELDS} FROM slice WHERE name = 'fixture_observation'
        AND EXTRACT_ARG(arg_set_id, 'debug.record_type') = 'observation' ORDER BY ts
    """)
    expected = list(csv.DictReader(rows_path.open()))
    actual = [{key: row[key] for key in expected[0]} for row in observations]
    if actual != expected:
        raise RuntimeError(f'CSV/Perfetto observations differ: {len(actual)} vs {len(expected)}')
    assert len(actual) == 10000
    assert len({row['scope_id'] for row in actual}) == 2
    samples = query(args.processor, trace, """
        SELECT ts, EXTRACT_ARG(arg_set_id, 'debug.native_ns') AS native_ns,
               EXTRACT_ARG(arg_set_id, 'debug.mach_absolute_ns') AS mach_ns,
               EXTRACT_ARG(arg_set_id, 'debug.uncertainty_ns') AS uncertainty_ns
        FROM slice WHERE name = 'MonadoClockCorrelation'
    """)
    offsets = [int(row['native_ns']) - int(row['mach_ns']) for row in samples]
    uncertainty = max(int(row['uncertainty_ns']) for row in samples)
    drift = max(offsets) - min(offsets)
    assert all(abs(int(row['ts']) - int(row['mach_ns'])) <= uncertainty for row in samples), samples
    assert all(min(abs(int(row['event_ns']) - int(row['ts']) - offset) for offset in offsets)
               <= uncertainty + drift for row in observations)

    intervals = query(args.processor, trace, """
        SELECT COUNT(*) AS count, MIN(dur) AS minimum, MAX(dur) AS maximum
        FROM slice WHERE name = 'fixture_observation'
        AND EXTRACT_ARG(arg_set_id, 'debug.record_type') = 'duration'
    """)
    assert intervals == [{'count': '10000', 'minimum': '100', 'maximum': '100'}], intervals
    clocks = query(args.processor, trace, """
        SELECT COUNT(*) AS count, MAX(EXTRACT_ARG(arg_set_id, 'debug.bracket_ns')) AS bracket
        FROM slice WHERE name = 'MonadoClockCorrelation'
    """)
    assert clocks[0]['count'] == '2', clocks
    print(json.dumps(dict(status='PASS', observations=len(actual), intervals=10000,
                          scopes=2, exact_csv_parity=True, clock_snapshots=clocks), indent=2))


def check_service(args):
    directory = args.service.resolve()
    csv_path = next(directory.glob('*_client_gpu.csv'))
    expected = list(csv.DictReader(csv_path.open()))
    all_rows = []
    for trace in directory.glob('*.pftrace'):
        check_loss(args.processor, trace)
        rows = query(args.processor, trace, f"""
            SELECT {FIELDS} FROM slice
            WHERE EXTRACT_ARG(arg_set_id, 'debug.domain') = 'client'
            AND EXTRACT_ARG(arg_set_id, 'debug.record_type') = 'observation' ORDER BY ts
        """)
        all_rows.extend(rows)
    def key(row, native):
        names = ['name' if native else 'event', 'frame_id' if native else 'client_frame_id',
                 'event_ns', 'timeline_value' if native else 'semaphore_value', 'duration_ns', 'scope_id']
        return tuple(row[name] for name in names)
    actual = sorted(key(row, True) for row in all_rows)
    reference = sorted(key(row, False) for row in expected)
    if actual != reference:
        raise RuntimeError(f'Service CSV/Perfetto mismatch: {len(actual)} vs {len(reference)}')
    ownership = {}
    for trace in directory.glob('*.pftrace'):
        rows = query(args.processor, trace, """
            SELECT name, COUNT(*) AS count FROM slice
            WHERE EXTRACT_ARG(arg_set_id, 'debug.domain') = 'ownership'
            AND EXTRACT_ARG(arg_set_id, 'debug.record_type') = 'observation'
            GROUP BY name
        """)
        for row in rows:
            ownership[row['name']] = ownership.get(row['name'], 0) + int(row['count'])
    owner_ids = set()
    for trace in directory.glob('*.pftrace'):
        rows = query(args.processor, trace, "SELECT DISTINCT EXTRACT_ARG(arg_set_id, 'debug.owner_scope_id') AS owner FROM slice WHERE EXTRACT_ARG(arg_set_id, 'debug.domain') = 'ownership'")
        owner_ids.update(row['owner'] for row in rows if row['owner'] not in ('0', '[NULL]', ''))
    assert owner_ids and owner_ids <= {row['scope_id'] for row in all_rows}, owner_ids
    assert ownership.get('producer_obligation', 0) > 0, ownership
    assert ownership.get('storage_quarantined', 0) > 0, ownership
    print(json.dumps(dict(status='PASS', service_observations=len(actual),
                          exact_csv_parity=True, owner_ids=sorted(owner_ids), ownership=ownership), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--processor', type=Path, required=True)
    parser.add_argument('--service', type=Path)
    args = parser.parse_args()
    if args.service:
        check_service(args)
    elif args.fixture:
        with tempfile.TemporaryDirectory(prefix='monado-perfetto-test-') as temporary:
            check_fixture(args, Path(temporary))
    else:
        parser.error('Specify --fixture or --service')


if __name__ == '__main__':
    main()
