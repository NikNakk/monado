#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Paired ICD GPU benchmark. Initial cache state is unknown; never delete user caches."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

kk, mvk, output = sys.argv[1:4]
cases = [('alu', 4, 1), ('alu', 16, 1), ('alu', 64, 1), ('tex', 1, 1), ('tex', 4, 1), ('tex', 12, 1),
         ('alu', 1, 200), ('alu', 1, 2000)]
rows = []
with open(output, 'w') as out:
    for shader, iters, draws in cases:
        # First observation is recorded independently; it is not a guaranteed cold cache.
        for index, (driver, icd) in enumerate([('kk', kk), ('mvk', mvk)] * 1 +
                                              [('kk', kk), ('mvk', mvk), ('mvk', mvk), ('kk', kk)]):
            thermal_before = subprocess.check_output([os.environ.get('THERMAL_PROBE', './thermal-state')], text=True).strip()
            env = dict(os.environ, VK_DRIVER_FILES=icd, KKB_DRAWS=str(draws))
            started = time.monotonic()
            r = subprocess.run(['/usr/bin/time', '-l', './kkbench', shader, str(iters), '4000', '2040', '600', '2'],
                               env=env, capture_output=True, text=True, timeout=120, check=True)
            row = json.loads(r.stdout)
            row.update(phase='first_observation' if index < 2 else 'warm', order=index,
                       selected=driver, icd=icd, process_seconds=time.monotonic()-started,
                       thermal_before=thermal_before,
                       thermal_after=subprocess.check_output([os.environ.get('THERMAL_PROBE', './thermal-state')], text=True).strip(),
                       time_stderr=r.stderr)
            out.write(json.dumps(row)+'\n'); out.flush()
            rows.append(row)
            time.sleep(1)
print(f'{len(rows)} runs recorded in {output}')
