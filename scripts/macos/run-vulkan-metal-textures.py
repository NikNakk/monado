#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run opt-in MoltenVK texture tests in an isolated launchd service."""
import argparse
import json
import os
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--moltenvk', type=Path, default=Path('/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib'))
    parser.add_argument('--mode', choices=['import', 'probe', 'hello'], default='probe')
    parser.add_argument('--cross-process', action='store_true', help='Import test also verifies an XPC Metal consumer')
    parser.add_argument('--skip-peer-wait', action='store_true', help='Negative import test: must detect missing GPU wait')
    parser.add_argument('--hello', type=Path, help='Unmodified SDK hello_xr executable')
    parser.add_argument('--seconds', type=int, default=8)
    parser.add_argument('--frames', type=int, default=180)
    parser.add_argument('--format', type=int, default=44, help='Vulkan colour format (44=BGRA8 UNORM; 50=BGRA8 sRGB)')
    parser.add_argument('--hosted', action='store_true')
    parser.add_argument('--headset', action='store_true')
    parser.add_argument('--no-readbacks', action='store_true', help='Visual acceptance: disable compositor pixel readbacks')
    parser.add_argument('--per-view', action='store_true', help='Existing single-layer IOSurface path')
    parser.add_argument('--runtime-defaults', action='store_true', help='Leave both Vulkan switches unset to verify runtime defaults')
    parser.add_argument('--cpu-handoff', action='store_true', help='Keep texture import, disable frame shared-event option')
    parser.add_argument('--suppress-signal', action='store_true', help='Test-only missing producer timeline signal')
    parser.add_argument('--unsupported-cube', action='store_true')
    parser.add_argument('--suite', action='store_true', help='Concurrent healthy client, failed overlay and replacement')
    parser.add_argument('--repeat', type=int, default=1, help='Sessions in one process, runtime pinned (1–16)')
    parser.add_argument('--child', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 1 <= args.repeat <= 16:
        parser.error('repeat must be 1–16')
    if args.suite and (args.mode != 'probe' or args.cpu_handoff or args.suppress_signal):
        parser.error('--suite needs a normal shared-event probe')
    if not 1 <= args.frames <= 3600 or not 1 <= args.seconds <= 120:
        parser.error('frames must be 1–3600 and seconds 1–120')
    if args.mode == 'hello' and not args.hello:
        parser.error('--mode hello requires --hello')
    if args.skip_peer_wait and not (args.mode == 'import' and args.cross_process):
        parser.error('--skip-peer-wait requires --mode import --cross-process')
    if args.cross_process and args.mode != 'import':
        parser.error('--cross-process is for --mode import')
    if args.runtime_defaults and args.cpu_handoff:
        parser.error('--runtime-defaults cannot be combined with --cpu-handoff')
    if args.suppress_signal and args.cpu_handoff:
        parser.error('--suppress-signal requires the shared-event handoff')
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not args.moltenvk.is_file():
        parser.error(f'Missing MoltenVK library: {args.moltenvk}')
    with tempfile.TemporaryDirectory(prefix='monado-vulkan-metal-') as temporary:
        icd = Path(temporary)/'moltenvk.json'
        icd.write_text(json.dumps({'file_format_version': '1.0.1', 'ICD': {
            'library_path': str(args.moltenvk.resolve()), 'api_version': '1.2.0'}}))
        env = dict(os.environ, VK_DRIVER_FILES=str(icd), MTL_DEBUG_LAYER='1',
                   XR_RUNTIME_JSON=str(build/'openxr_monado-dev.json'))
        if args.mode == 'import':
            command = [str(build/'tests/tests_macos_vk_metal_texture_import')]
            label = f'org.freedesktop.monado.vulkan-texture-test.{os.getpid()}'
            domain = f'gui/{os.getuid()}'
            plist = Path(temporary)/'peer.plist'
            if args.cross_process:
                peer_env = {'MTL_DEBUG_LAYER': '1'}
                if args.skip_peer_wait:
                    peer_env['VK_METAL_TEST_SKIP_PEER_WAIT'] = '1'
                plist.write_bytes(plistlib.dumps(dict(Label=label, MachServices={label: True}, RunAtLoad=True,
                    ProgramArguments=[str(build/'tests/tests_macos_vk_metal_texture_peer'), label],
                    EnvironmentVariables=peer_env, StandardOutPath=str(output/'peer.txt'),
                    StandardErrorPath=str(output/'peer.txt'))))
                subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True, timeout=10)
                command += [label]
            try:
                result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                text = result.stdout+result.stderr
                (output/'import.txt').write_text(text)
                print(text, end='')
                if args.skip_peer_wait:
                    assert result.returncode != 0 and 'FAIL consumer ran before producer gate' in text
                    print('PASS negative control: missing cross-process GPU wait detected')
                    return 0
                return result.returncode
            finally:
                if args.cross_process:
                    subprocess.run(['launchctl', 'bootout', domain, str(plist)], timeout=10, check=False)
        if not args.child:
            # Existing launcher creates a unique socket/Mach service, and removes
            # it in finally. Only the child receives the experimental options.
            command = [sys.executable, str(Path(__file__).with_name('run-gpu-sync-service.py')),
                       '--build', str(build), '--output', str(output), '--client-timeout',
                       str(max(30, args.seconds+15, args.repeat*3+15))]
            if args.no_readbacks:
                command += ['--service-env', 'XRT_COMPOSITOR_LOG_APPLE_SAMPLES=0']
            else:
                command += ['--sample-interval', '15']
            if args.headset:
                command += ['--headset']
            child = Path(temporary)/'client.py'
            child.write_text('import runpy, sys\n' +
                             f'sys.argv = {[str(Path(__file__).resolve()), *sys.argv[1:], "--child"]!r}\n' +
                             f'runpy.run_path({str(Path(__file__).resolve())!r}, run_name="__main__")\n')
            command += ['--client', sys.executable, str(child)]
            result = subprocess.run(command, env=env, timeout=max(60, args.seconds+45, args.repeat*3+30))
            if args.hosted and result.returncode == 0:
                log = (output/'client.txt').read_text()
                if 'In-process compositor unavailable' in log or 'remote=0' not in log:
                    print('Hosted path was not used; inspect client.txt (headset hosting required)', file=sys.stderr)
                    return 1
            return result.returncode
        env.update(XRT_MACOS_VK_CLIENT_METAL_TEXTURES='1',
                   XRT_MACOS_VK_CLIENT_SHARED_EVENT='0' if args.cpu_handoff else '1',
                   XRT_MACOS_CLIENT_COMPOSITOR='1' if args.hosted else '0',
                   MACOS_OPENXR_VULKAN_PROBE_FRAMES=str(args.frames),
                   MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS='1' if args.per_view else '0',
                   MACOS_OPENXR_VULKAN_PROBE_COLOR_FORMAT=str(args.format),
                   MACOS_OPENXR_VULKAN_PROBE_SOURCE_SAMPLES='1',
                   MACOS_OPENXR_VULKAN_PROBE_ASYNC_SUBMIT='1',
                   MACOS_OPENXR_VULKAN_PROBE_REPEAT=str(args.repeat))
        if args.runtime_defaults:
            env.pop('XRT_MACOS_VK_CLIENT_METAL_TEXTURES', None)
            env.pop('XRT_MACOS_VK_CLIENT_SHARED_EVENT', None)
        if args.no_readbacks:
            env.pop('MACOS_OPENXR_VULKAN_PROBE_SOURCE_SAMPLES', None)
        for key, enabled in [('MACOS_OPENXR_VULKAN_PROBE_SUPPRESS_RUNTIME_SIGNAL', args.suppress_signal),
                             ('MACOS_OPENXR_VULKAN_PROBE_UNSUPPORTED_CUBE', args.unsupported_cube)]:
            env.pop(key, None)
            if enabled:
                env[key] = '1'
        if args.mode == 'probe' and args.suite:
            command = [str(build/'tests/tests_macos_openxr_vulkan_probe')]
            healthy_log = (output/'healthy.txt').open('w')
            healthy_env = dict(env, MACOS_OPENXR_VULKAN_PROBE_FRAMES='300')
            healthy = subprocess.Popen(command, env=healthy_env, stdout=healthy_log, stderr=subprocess.STDOUT)
            try:
                time.sleep(.5)
                fault_env = dict(env, MACOS_OPENXR_VULKAN_PROBE_OVERLAY='1',
                                 MACOS_OPENXR_VULKAN_PROBE_SUPPRESS_RUNTIME_SIGNAL='1',
                                 MACOS_OPENXR_VULKAN_PROBE_FRAMES='120')
                started = time.monotonic()
                failed = subprocess.run(command, env=fault_env, capture_output=True, text=True, timeout=15)
                text = failed.stdout+failed.stderr
                (output/'failed.txt').write_text(text)
                assert failed.returncode != 0 and 'failed: -17' in text
                assert 'Retaining unresolved Vulkan Metal frontend' in text
                loss_seconds = time.monotonic()-started
                replacement = subprocess.run(command, env=dict(env, MACOS_OPENXR_VULKAN_PROBE_OVERLAY='1',
                                             MACOS_OPENXR_VULKAN_PROBE_FRAMES='60'),
                                             capture_output=True, text=True, timeout=15)
                (output/'replacement.txt').write_text(replacement.stdout+replacement.stderr)
                assert replacement.returncode == 0
                assert healthy.wait(timeout=15) == 0
                print(json.dumps(dict(concurrent_healthy=True, replacement=True, missing_signal_session_loss=True,
                                      fault_process_seconds=loss_seconds)))
                return 0
            finally:
                if healthy.poll() is None:
                    healthy.kill()
                    healthy.wait(timeout=5)
                healthy_log.close()
        if args.mode == 'probe':
            return subprocess.run([str(build/'tests/tests_macos_openxr_vulkan_probe')],
                                  env=env, timeout=max(25, args.repeat*3)).returncode
        process = subprocess.Popen([str(args.hello.resolve()), '-g', 'Vulkan2', '-bm', 'Opaque', '-v'],
                                   env=env, stdin=subprocess.PIPE)
        try:
            try:
                process.wait(timeout=args.seconds)
                print('hello_xr exited before the requested observation interval', file=sys.stderr)
                return 1
            except subprocess.TimeoutExpired:
                process.communicate(input=b'\n', timeout=10)
                return process.returncode
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


if __name__ == '__main__':
    sys.exit(main())
