#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run the existing finite GPU matrix through an isolated Windows DXMT producer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', required=True, type=Path)
    parser.add_argument('--wine-runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--frames', type=int, default=120)
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--scenario', action='append')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    root, runtime, output = (p.resolve() for p in
                             (args.artifacts, args.wine_runtime, args.output))
    if root.is_relative_to(repo) or output.is_relative_to(repo):
        parser.error('keep test artifacts, Wine prefixes and raw results outside Git')
    output.mkdir(parents=True, exist_ok=True)
    names = ['producer.exe', 'monado_gpu_sync.dll', 'd3d11.dll', 'dxgi.dll',
             'winemetal.dll', 'x86_64-unix/monado_gpu_sync.so']
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
              for name in names}
    prefix = root / 'prefix'
    env = os.environ.copy()
    env.update(WINEPREFIX=str(prefix), WINEDEBUG='-all',
               WINEDLLPATH=f'{root}:{runtime}/lib/wine',
               DYLD_FALLBACK_LIBRARY_PATH=f'{runtime}/lib/wine/x86_64-unix:{runtime}/lib',
               WINEDLLOVERRIDES='d3d11,dxgi=n,b;mscoree=;mshtml=',
               DXMT_LOG_LEVEL='error')
    launcher = root / 'launch.zsh'
    exports = '\n'.join(f'export {key}={shlex.quote(env[key])}' for key in
                         ('WINEPREFIX', 'WINEDEBUG', 'WINEDLLPATH',
                          'DYLD_FALLBACK_LIBRARY_PATH', 'WINEDLLOVERRIDES',
                          'DXMT_LOG_LEVEL'))
    launcher.write_text('#!/bin/zsh\nset -euo pipefail\n' + exports + '\n' +
                        f'exec {shlex.quote(str(runtime / "bin/wine"))} '
                        f'{shlex.quote(str(root / "producer.exe"))} "$@" '
                        f'>>{shlex.quote(str(output / "producer.log"))} 2>&1\n')
    launcher.chmod(0o755)
    # This prefix is private to this harness; never stop any other Wine server.
    try:
        with (output / 'prefix-init.log').open('w') as log:
            subprocess.run([str(runtime / 'bin/wine'), 'wineboot', '-u'],
                           env=env, stdout=log, stderr=log, timeout=120, check=True)
        version = subprocess.check_output([str(runtime / 'bin/wine'), '--version'],
                                          env=env, text=True, timeout=10).strip()
        (output / 'wine-artifacts.json').write_text(json.dumps({
            'wine_version': version, 'artifact_sha256': hashes,
            'prefix': str(prefix), 'producer_api': 'Windows x86-64 D3D11/DXMT',
            'native_helper': 'object/command transport; no pixel writes',
        }, indent=2) + '\n')
        env['GPU_SYNC_WINE_LAUNCHER'] = str(launcher)
        command = [sys.executable, str(repo / 'scripts/macos/run-gpu-sync.py'),
                   '--probe', str(repo / 'build/arm64/tests/tests_macos_gpu_sync'),
                   '--output', str(output), '--frames', str(args.frames),
                   '--repeats', str(args.repeats), '--timeout', '160']
        for scenario in args.scenario or []:
            command += ['--scenario', scenario]
        return subprocess.run(command, cwd=repo, env=env).returncode
    finally:
        subprocess.run([str(runtime / 'bin/wineserver'), '-k'],
                       env=env, timeout=10, check=False)


if __name__ == '__main__':
    raise SystemExit(main())
