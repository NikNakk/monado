#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Summarize matched probe captures without treating callbacks as scanout."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('captures', type=Path, nargs='+', help='Directories containing client/ and service/')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
results = []
for capture in a.captures:
    log = (capture / 'client/probe.log').read_text()
    pid = re.search(r'monado_psvr2_(\d+)_present\.csv', log)
    metrics = re.search(r'Runtime queue metrics: submits=(\d+) submit_ns=(\d+) idle_waits=(\d+) idle_ns=(\d+)', log)
    if not pid or not metrics:
        raise RuntimeError(str(capture) + ': missing target identity or runtime metrics')
    prefix = 'monado_psvr2_' + pid[1] + '_'
    def rows(suffix):
        return list(csv.DictReader((capture / 'service' / (prefix + suffix)).open()))
    presented = rows('presented.csv')
    stamps = sorted(set(int(row['presented_monotonic_ns']) for row in presented if int(row['presented_monotonic_ns']) > 0))
    periods = [(y - x) / 1e6 for x, y in zip(stamps, stamps[1:])]
    completions = rows('present_complete.csv')
    gpu = [(float(row['gpu_end_time_s']) - float(row['gpu_start_time_s'])) * 1000 for row in completions
           if float(row['gpu_end_time_s']) > float(row['gpu_start_time_s']) > 0]
    cpu = [int(row['cpu_since_previous_predict_ns']) / 1e6 for row in rows('compositor_rt.csv')
           if int(row['cpu_since_previous_predict_ns']) > 0]
    sources = rows('reprojection_source.csv')
    seen = [int(row['client_frame_id']) for row in sources if row['source_valid'] == '1']
    results.append(dict(capture=str(capture),
        descriptors=re.findall(r'Probe swapchain: (.*)', log),
        results=json.loads((capture / 'client/results.json').read_text()),
        runtime_submit_count=int(metrics[1]), runtime_submit_ms=int(metrics[2]) / 1e6,
        runtime_idle_count=int(metrics[3]), runtime_idle_ms=int(metrics[4]) / 1e6,
        last_consumed_client_frame=max(seen) if seen else None,
        presentation_callbacks=len(presented), positive_presentation_timestamps=len(stamps),
        presentation_interval_ms_median=statistics.median(periods) if periods else None,
        presentation_command_buffer_gpu_ms_median=statistics.median(gpu) if gpu else None,
        compositor_thread_cpu_ms_median=statistics.median(cpu) if cpu else None))
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
