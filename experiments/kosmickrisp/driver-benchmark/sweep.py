#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run kkbench on KosmicKrisp and MoltenVK in A-B-B-A order and print a summary.

Usage: sweep.py <kosmickrisp ICD JSON> <MoltenVK ICD JSON> <output.jsonl>
Build first, from this directory:
  for s in fs.vert alu.frag tex.frag; do glslangValidator -V -o $s.spv $s; done
  clang -O2 -I$(brew --prefix)/include -L$(brew --prefix)/lib -lvulkan kkbench.c -o kkbench
"""
import collections, json, os, subprocess, sys

kk, mvk, output = sys.argv[1:4]
cases = [('alu', 4, 1), ('alu', 16, 1), ('alu', 64, 1), ('tex', 1, 1), ('tex', 4, 1), ('tex', 12, 1),
         ('alu', 1, 200), ('alu', 1, 2000)]
rows = []
with open(output, 'w') as out:
    for shader, iters, draws in cases:
        for icd in (kk, mvk, mvk, kk):
            env = dict(os.environ, VK_DRIVER_FILES=icd, KKB_DRAWS=str(draws))
            r = subprocess.run(['./kkbench', shader, str(iters), '4000', '2040', '600', '2'], env=env,
                               capture_output=True, text=True, timeout=120, check=True)
            out.write(r.stdout)
            rows.append(json.loads(r.stdout))
groups = collections.defaultdict(list)
for r in rows:
    groups[(r['shader'], r['iters'], r['draws'], r['driver'])].append(r)
for (shader, iters, draws, driver), rs in sorted(groups.items()):
    print(f"{shader} x{iters} draws={draws} {driver}: GPU median "
          + ' / '.join(f"{r['gpu_ms_median']:.3f}" for r in rs) + ' ms, CPU record+submit median '
          + ' / '.join(f"{r['cpu_record_submit_us_median']:.0f}" for r in rs) + ' us')
