#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Test production SteamVR-backend copy elimination with a separate Windows writer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bridge', type=Path, default=Path.home()/'Code/macos-wine-xr')
    parser.add_argument('--dxmt', type=Path, default=Path.home()/'Code/dxmt')
    parser.add_argument('--wine', type=Path, default=Path.home()/'Code/wine-crossover/runtime')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    root = args.output.resolve()
    if root.is_relative_to(repo):
        parser.error('keep raw evidence and the private prefix outside Git')
    root.mkdir(parents=True, exist_ok=False)
    bridge, wine, dxmt = args.bridge.resolve(), args.wine.resolve(), args.dxmt.resolve()
    driver = bridge/'build-in-process/steamvr-driver'
    gate = bridge/'build-in-process/gate-steamvr'
    backend = bridge/'src/steamvr_driver/xr_backend.cpp'
    original = backend.read_text()
    needle = 'context_->CopySubresourceRegion(destination.texture,'
    if original.count(needle) != 1:
        raise RuntimeError('Production copy instrumentation requires review after backend changes')
    counted = root/'xr_backend_counted.cpp'
    counted.write_text('unsigned long long mwxr_test_copies = 0;\n'+
                      original.replace(needle, '++mwxr_test_copies;\n\t\t\t\t'+needle))
    includes = driver/'_deps/openxr_sdk_source-build/include'
    loader = driver/'_deps/openxr_sdk_source-build/src/loader/libopenxr_loader.a'
    command = ['x86_64-w64-mingw32-g++', '-std=c++17', '-O2', '-static',
               '-I'+str(bridge/'src/steamvr_driver'), '-I'+str(bridge/'src'),
               '-I'+str(dxmt/'include'), '-I'+str(includes),
               str(repo/'tests/steamvr_resolve_copy_probe.cpp'), str(counted), str(loader),
               '-ld3d11', '-ldxgi', '-ladvapi32', '-luser32', '-lole32', '-o', str(root/'probe.exe')]
    subprocess.run(command, check=True)
    for name in ['d3d11.dll', 'dxgi.dll', 'winemetal.dll']:
        shutil.copy2(wine/'lib/wine/x86_64-windows'/name, root/name)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(root/'prefix'), WINEARCH='win64', WINEDEBUG='-all',
               WINEDLLPATH=f'{gate}:{wine}/lib/wine',
               DYLD_FALLBACK_LIBRARY_PATH=f'{wine}/lib/wine/x86_64-unix:{wine}/lib',
               WINEDLLOVERRIDES='d3d11,dxgi=n,b;mscoree=;mshtml=',
               MWXR_NATIVE_RUNTIME_JSON=str(repo/'build/x86_64/openxr_monado-dev.json'),
               MWXR_GRAPHICS_BACKEND='dxmt')
    env.pop('XR_RUNTIME_JSON', None)
    keys = ['WINEPREFIX', 'WINEARCH', 'WINEDEBUG', 'WINEDLLPATH', 'DYLD_FALLBACK_LIBRARY_PATH',
            'WINEDLLOVERRIDES', 'MWXR_NATIVE_RUNTIME_JSON', 'MWXR_GRAPHICS_BACKEND']
    launcher = root/'launch.zsh'
    launcher.write_text('#!/bin/zsh\nset -euo pipefail\n'+
                       ''.join(f'export {key}={shlex.quote(env[key])}\n' for key in keys)+
                       'unset XR_RUNTIME_JSON\n'+
                       f'exec {shlex.quote(str(wine/"bin/wine"))} {shlex.quote(str(root/"probe.exe"))} "$@"\n')
    launcher.chmod(0o755)
    files = [backend, bridge/'src/steamvr_driver/xr_backend.h', counted, root/'probe.exe',
             gate/'x86_64-windows/wineopenxr.dll', gate/'x86_64-unix/wineopenxr.so',
             wine/'lib/wine/x86_64-unix/winemetal.so',
             repo/'build/arm64/src/xrt/targets/service/monado-service',
             repo/'build/x86_64/src/xrt/targets/openxr/libopenxr_monado.dylib']
    files += [root/name for name in ['d3d11.dll', 'dxgi.dll', 'winemetal.dll']]
    manifest = {'monado_commit': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                'build_command': command, 'instrumentation': 'one counter before the existing backend per-eye copy',
                'artifacts': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (root/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    try:
        with (root/'prefix-init.log').open('w') as log:
            subprocess.run([str(wine/'bin/wine'), 'wineboot', '-u'], env=env,
                           stdout=log, stderr=log, timeout=120, check=True)
            runtime_dir = root/'prefix/drive_c/openxr'
            runtime_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(gate/'x86_64-windows/wineopenxr.dll', root/'prefix/drive_c/windows/system32')
            (runtime_dir/'wineopenxr64.json').write_text(json.dumps({
                'file_format_version': '1.0.0', 'runtime': {'library_path': r'C:\windows\system32\wineopenxr.dll'}}))
            subprocess.run([str(wine/'bin/wine'), 'reg', 'add', r'HKLM\Software\Khronos\OpenXR\1',
                            '/v', 'ActiveRuntime', '/t', 'REG_SZ', '/d', r'C:\openxr\wineopenxr64.json', '/f'],
                           env=env, stdout=log, stderr=log, timeout=30, check=True)
        results = []
        for mode in ['four', 'three']:
            path = root/mode
            command = [sys.executable, str(repo/'scripts/macos/run-gpu-sync-service.py'),
                       '--output', str(path), '--client-timeout', '180']
            if mode == 'three':
                command += ['--three-images']
            command += ['--client', str(launcher), mode]
            subprocess.run(command, cwd=repo, check=True)
            text = (path/'client.txt').read_text()
            events = [json.loads(row) for row in text.splitlines() if row.startswith('{')]
            result = next(row for row in events if row.get('status') == 'PASS')
            log = (path/'service.log').read_text()
            matches = sum('rgbaL=(255,0,0,255)' in row and 'rgbaR=(0,0,255,255)' in row
                          for row in log.splitlines())
            if matches < 2:
                raise RuntimeError(mode+': insufficient composed eye pixel checks')
            if ('Three-image compatibility:' in log) != (mode == 'three'):
                raise RuntimeError(mode+': unexpected service scheduler')
            result.update(mode=mode, composed_stereo_checks=matches)
            results.append(result)
            print(json.dumps(result), flush=True)
        (root/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    finally:
        subprocess.run([str(wine/'bin/wineserver'), '-k'], env=env, timeout=10, check=False)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
