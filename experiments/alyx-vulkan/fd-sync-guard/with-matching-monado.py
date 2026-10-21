#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run one native client with the same build's temporary launchd Monado service."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import resource
import signal
import subprocess
import tempfile
import time


def prepare_client_file_limit(minimum=8192):
    """Raise only this launcher's soft limit; children inherit it at exec."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = minimum if hard == resource.RLIM_INFINITY else min(minimum, hard)
    result = {'soft_before': soft, 'minimum': minimum,
              'hard': None if hard == resource.RLIM_INFINITY else hard}
    if soft != resource.RLIM_INFINITY and soft < target:
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
        except (OSError, ValueError) as error:
            result['raise_error'] = str(error)
    after, after_hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    result.update(soft_after=after,
                  minimum_met=after == resource.RLIM_INFINITY or after >= minimum,
                  hard_unchanged=hard == after_hard)
    return result



def snapshot_video_settings(root):
    """Read numeric/setting entries only; never change shared game configuration."""
    result = {'captured_unix': time.time(), 'files': {}}
    for relative in ['game/hlvr/cfg/deckard/video2.txt', 'game/hlvr/cfg/video.txt']:
        path = root / 'workspace/alyx-native-game-root' / relative
        try:
            data = path.read_bytes()
            values = dict(re.findall(r'"(setting\.[^"\n]+|VendorID|DeviceID|Version)"\s*"([^"\n]*)"',
                                     data.decode('utf-8', errors='replace')))
            result['files'][relative] = {'exists': True, 'sha256': hashlib.sha256(data).hexdigest(),
                                        'mtime_ns': path.stat().st_mtime_ns, 'settings': values}
        except OSError as error:
            result['files'][relative] = {'exists': path.exists(), 'read_error': type(error).__name__}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--mode', choices=['desktop', 'offscreen'], default='desktop')
    parser.add_argument('--output', type=Path, help='Otherwise inferred from the client command --output')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('supply -- command [args]')
    options = command[:command.index('--')] if '--' in command else command
    if args.output is None:
        try:
            args.output = Path(options[options.index('--output') + 1])
        except (ValueError, IndexError):
            parser.error('supply --output here or in the client command')
    output = args.output.resolve()
    service_output = output.with_name(output.name + '-monado')
    build = args.build.resolve(strict=True)
    service = build / 'src/xrt/targets/service/monado-service'
    runtime_json = build / 'openxr_monado-dev.json'
    runtime = Path(json.loads(runtime_json.read_text())['runtime']['library_path'])
    if not runtime.is_absolute():
        runtime = runtime_json.parent / runtime
    if not service.is_file() or not runtime.is_file():
        parser.error('build both monado-service and the OpenXR runtime first')
    env = dict(os.environ)
    env.pop('IPC_IGNORE_VERSION', None)
    # The compositor service retains MoltenVK; the client backend selects its ICD later.
    env.pop('VK_ICD_FILENAMES', None)
    env.update(VK_DRIVER_FILES='/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json',
               XR_RUNTIME_JSON=str(runtime_json), MONADO_OPENXR_RUNTIME_PATH=str(runtime),
               SIMULATED_DESKTOP='1' if args.mode == 'desktop' else '0', SIMULATED_ENABLE='1',
               PSVR2_CAMERA_STREAMS='0', XRT_NO_STDIN='1', IPC_EXIT_WHEN_IDLE='0',
               XRT_COMPOSITOR_NULL='0', XRT_COMPOSITOR_FORCE_DEBUG_IMAGE='0' if args.mode == 'desktop' else '1')
    # Desktop Alyx needs the same interactive devices as the working user runs.
    env.setdefault('QWERTY_ENABLE', '1' if args.mode == 'desktop' else '0')
    if args.mode == 'offscreen':
        env['XRT_MACOS_CLIENT_COMPOSITOR'] = '0'
    if '--dry-run' in options:
        return subprocess.run(command, env=env).returncode
    if output.exists() or service_output.exists():
        parser.error('client and companion -monado output directories must both be new')
    file_limits = prepare_client_file_limit()
    print('Client open-file limit:', file_limits, flush=True)
    service_output.mkdir(parents=True)
    native_root = Path(os.environ.get('ALYX_NATIVE_ROOT', str(Path.home()/'Code/alyx-native')))
    settings = {'before': snapshot_video_settings(native_root)}
    settings_path = service_output/'video-settings.json'
    settings_path.write_text(json.dumps(settings, indent=2)+'\n')
    label = f'org.freedesktop.monado.alyx-kosmickrisp.{os.getpid()}'
    domain = f'gui/{os.getuid()}'
    with tempfile.TemporaryDirectory(prefix='alyx-kk-ipc-') as temporary:
        env.update(XDG_RUNTIME_DIR=temporary, XRT_MACOS_METAL_IPC_SERVICE_NAME=label)
        plist = Path(temporary) / 'service.plist'
        plist.write_bytes(plistlib.dumps(dict(Label=label, ProgramArguments=[str(service)],
            EnvironmentVariables=env, MachServices={label: True}, RunAtLoad=True,
            StandardOutPath=str(service_output/'service.log'), StandardErrorPath=str(service_output/'service.log'))))
        # Persist only the launch description, not inherited environment values/secrets.
        identity = {'mode': args.mode, 'build': str(build), 'runtime_json': str(runtime_json),
                    'service': str(service), 'runtime': str(runtime), 'label': label,
                    'service_sha256': hashlib.sha256(service.read_bytes()).hexdigest(),
                    'runtime_sha256': hashlib.sha256(runtime.read_bytes()).hexdigest(),
                    'version_check_bypassed': False, 'installed_service_changed': False,
                    'client_open_file_limits': file_limits}
        identity['qwerty_enable'] = env['QWERTY_ENABLE']
        (service_output/'identity.json').write_text(json.dumps(identity, indent=2)+'\n')
        print('Matching Monado service logs:', service_output, flush=True)
        bootstrapped = False
        child = None
        try:
            subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True, timeout=10)
            bootstrapped = True
            deadline = time.monotonic() + 20
            while not (Path(temporary)/'monado_comp_ipc').exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError('matching service did not become ready; inspect ' + str(service_output/'service.log'))
                time.sleep(.05)
            # The launcher handles Ctrl+C and its own host process group. Forward the
            # signal once, wait for cleanup, and boot out our uniquely named service.
            child = subprocess.Popen(command, env=env, start_new_session=True)
            try:
                code = child.wait()
            except KeyboardInterrupt:
                os.killpg(child.pid, signal.SIGINT)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGTERM)
                    child.wait(timeout=5)
                code = 130
            identity['client_exit'] = code
            (service_output/'identity.json').write_text(json.dumps(identity, indent=2)+'\n')
            return code if code >= 0 else 128-code
        finally:
            if child is not None and child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
            if bootstrapped:
                subprocess.run(['launchctl', 'bootout', domain, str(plist)], check=False, timeout=10)
            settings['after'] = snapshot_video_settings(native_root)
            settings_path.write_text(json.dumps(settings, indent=2)+'\n')


if __name__ == '__main__':
    raise SystemExit(main())
