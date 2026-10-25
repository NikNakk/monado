#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Summarise Sense loss evidence from service logs without starting hardware.

Device IDs are tracker IDs, not guaranteed hand labels. Active snapshots are
cumulative and are never added to recovered-loss totals. Logs cannot establish
whether a ring with no visible blobs was occluded or pulsing outside exposure.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

CLASSES = ('acquiring', 'lit_multi', 'lit_ambiguous', 'lit_single',
           'dark_in_view', 'dark_out_of_view', 'dark_unpredicted')
FIELDS = re.compile(r'(\w+)=([^\s,]+)')


def summarize(path):
    devices = {}
    filters = {}
    transport = {}
    pose_states = Counter()
    led_losses = Counter()
    connections = []
    with path.open(errors='replace') as log:
        for line_number, line in enumerate(log, 1):
            if not any(marker in line for marker in ('JOINT_LOSS', 'FILTER side=', 'PSSENSE_OUTPUT ',
                                                    'POSE_STATE ', 'LED_BOOTSTRAP ', 'CONNECTION ')):
                continue
            fields = dict(FIELDS.findall(line))
            if 'CONNECTION ' in line:
                connections.append(dict(line=line_number, **fields))
            elif 'JOINT_LOSS' in line and 'device' in fields:
                device = devices.setdefault(fields['device'], dict(
                    recovered_losses=0, recovered_gap_ms=0.0, max_recovered_gap_ms=0.0,
                    exposure_classes=Counter(), latest_active=None, longest_losses=[]))
                if 'JOINT_LOSS_ACTIVE ' in line:
                    device['latest_active'] = dict(line=line_number, **fields)
                elif 'JOINT_LOSS ' in line:
                    gap = float(fields['gap_ms'])
                    device['recovered_losses'] += 1
                    device['recovered_gap_ms'] += gap
                    device['max_recovered_gap_ms'] = max(device['max_recovered_gap_ms'], gap)
                    device['exposure_classes'].update({k: int(fields.get(k, 0)) for k in CLASSES})
                    device['latest_active'] = None
                    device['longest_losses'].append(dict(line=line_number, **fields))
                    device['longest_losses'].sort(key=lambda d: float(d['gap_ms']), reverse=True)
                    del device['longest_losses'][5:]
            elif 'FILTER side=' in line:
                filters[fields['side']] = dict(line=line_number, **fields)
            elif 'PSSENSE_OUTPUT ' in line and 'side' in fields:
                data = transport.setdefault(fields['side'], dict(writes=0, failed_writes=0,
                                                                max_write_ms=0.0, max_input_gap_ms=0.0,
                                                                last_input_ns=0))
                data['writes'] += 1
                data['failed_writes'] += fields.get('result') != fields.get('expected')
                data['max_write_ms'] = max(data['max_write_ms'],
                                           (int(fields['end_ns']) - int(fields['start_ns'])) / 1e6)
                input_ns = int(fields['input_ns'])
                if input_ns > 0:
                    if data['last_input_ns']:
                        data['max_input_gap_ms'] = max(data['max_input_gap_ms'],
                                                       (input_ns - data['last_input_ns']) / 1e6)
                    data['last_input_ns'] = input_ns
            elif 'POSE_STATE ' in line:
                pose_states[f"{fields['side']}:{fields['flags']}"] += 1
            elif 'LED_BOOTSTRAP ' in line and fields.get('event') == 'lost':
                led_losses[fields['side']] += 1
    return dict(path=str(path.resolve()), devices=devices, last_filter_by_side=filters,
                transport_by_side=transport, connections=connections, led_losses_by_side=led_losses,
                pose_state_records_by_side_and_flags=pose_states,
                caveat='Recovered gaps exclude unfinished losses and gaps below the logging threshold; '
                       'input gaps are observed at output writes, not every HID packet. '
                       'Pose record counts are not time fractions. Use one service lifetime per log.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps([summarize(path) for path in args.logs], indent=2))


if __name__ == '__main__':
    main()
