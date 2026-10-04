#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Prepare a persistent local Sense LaunchAgent from the existing normal configuration; never activate hardware."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shlex

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('calibration', type=Path)
parser.add_argument('--base-plist', type=Path, default=Path(f'/private/tmp/org.freedesktop.monado.service.{os.getuid()}.plist'))
args = parser.parse_args()
calibration = args.calibration.resolve(strict=True)
build = root / 'build-wine'
cache = (build / 'CMakeCache.txt').read_text()
if f'CMAKE_HOME_DIRECTORY:INTERNAL={root}\n' not in cache:
    parser.error('build-wine belongs to another checkout')
commands = json.loads((build / 'compile_commands.json').read_text())
for suffix in ('joint_pose_solver.cpp', 'pssense_driver.c', 'comp_renderer.c', 'comp_window_macos.m'):
    found = [c for c in commands if c['file'].endswith('/' + suffix)]
    if not found or any(not re.search(r'(?:^| )-O(?:[123sz]|fast)(?: |$)', c['command'])
                        or re.search(r'(?:^| )-O0(?: |$)', c['command']) for c in found):
        parser.error(f'{suffix} must be built with optimisation')
plist = plistlib.loads(args.base_plist.read_bytes())
env = plist.setdefault('EnvironmentVariables', {})
env.update(PSVR2_SENSE_6DOF='1', PSVR2_SENSE_6DOF_CALIBRATION=str(calibration), XRT_NO_STDIN='1')
# Normal clients use the ordinary socket and Mach endpoint.
env.pop('XDG_RUNTIME_DIR', None)
env.pop('XRT_MACOS_METAL_IPC_SERVICE_NAME', None)
plist.update(Label='org.freedesktop.monado.service', RunAtLoad=False, KeepAlive=False,
             ProgramArguments=[str(build / 'src/xrt/targets/service/monado-service')],
             MachServices={'org.freedesktop.monado.metal-ipc': True})
output = build / 'local-sense-default'
output.mkdir(exist_ok=True)
prepared = output / 'org.freedesktop.monado.service.plist'
prepared.write_bytes(plistlib.dumps(plist))
manifest = build / 'openxr_monado-dev.json'
(output / 'native-client.env').write_text(
    f'export XR_RUNTIME_JSON={shlex.quote(str(manifest))}\n'
    'export XRT_MACOS_CLIENT_COMPOSITOR=1\n'
    '# Remove the isolated test endpoint overrides when using the normal service.\n'
    'unset XRT_MACOS_METAL_IPC_SERVICE_NAME\n'
    f'if [ "${{XDG_RUNTIME_DIR:-}}" = {shlex.quote(str(root / "build-macos-sense-rel/sense-runtime"))} ]; then unset XDG_RUNTIME_DIR; fi\n')
print('Prepared', prepared)
print('Preserved normal service tuning; enabled Sense only in this local profile.')
print('Native clients: source', output / 'native-client.env')
print('Wine launchers must retain their x86-64 runtime manifest override.')
print('Close clients, then switch registration (user hardware step):')
print(f'launchctl bootout gui/{os.getuid()}/org.freedesktop.monado.sense-integration')
print(f'launchctl bootout gui/{os.getuid()}/org.freedesktop.monado.service')
print('mkdir -p "$HOME/Library/LaunchAgents"')
print(f'cp {shlex.quote(str(prepared))} "$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist"')
print(f'launchctl bootstrap gui/{os.getuid()} "$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist"')
