#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Personal macOS PS VR2 profile, not an upstream default. Preview unless --install is given."""

import argparse
from datetime import datetime
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LABEL = 'org.freedesktop.monado.service'
PREFIX = Path.home() / '.local/monado-psvr2'
CALIBRATION = Path.home() / 'Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json'


def prepare(base, calibration, prefix):
    """Overlay deliberately chosen preferences; leave unrelated saved tuning intact."""
    calibration = calibration.resolve(strict=True)
    if not calibration.is_file():
        raise ValueError(f'Calibration is not a file: {calibration}')
    environment = dict(base.get('EnvironmentVariables', {}))
    if environment.get('PSVR2_STAGE_SPACE', '0') != '0':
        raise ValueError('Remove PSVR2_STAGE_SPACE: floor calibration needs a managed STAGE')
    if 'XDG_RUNTIME_DIR' in environment or 'XRT_MACOS_METAL_IPC_SERVICE_NAME' in environment:
        raise ValueError('Remove isolated endpoint overrides before installing the normal local profile')
    environment.update(
        PSVR2_SENSE_6DOF='1',
        PSVR2_SENSE_6DOF_CALIBRATION=str(calibration),
        PSVR2_CAMERA_STREAMS='1',
        PSVR2_LED_DETECTOR_BLOBS='1',
        PSSENSE_LED_BOOTSTRAP_QUICK_LOCK='1',
        PSSENSE_RECONNECT='1',
        XRT_FLOOR_EYE_HEIGHT_M='1.72',
        XRT_FLOOR_ALIGN='1',
        XRT_PSSENSE_STAGE_RECENTER='1',
        XRT_PSSENSE_STAGE_RECENTER_HOLD_MS='1000',
        XRT_MACOS_GPU_QUEUE_PRIORITY='0',
        XRT_NO_STDIN='1',
    )
    # Match the generic installer's lifecycle defaults without replacing user values.
    for key, value in dict(IPC_EXIT_WHEN_IDLE='1', IPC_EXIT_WHEN_IDLE_DELAY_MS='5000',
                           XRT_MACOS_EXIT_ON_DISPLAY_LOSS='1', XRT_MACOS_DISPLAY_LOSS_DELAY_MS='3000',
                           XRT_MACOS_DISPLAY_LOSS_SHUTDOWN_WATCHDOG_MS='5000').items():
        environment.setdefault(key, value)
    profile = dict(base)
    logs = Path.home() / 'Library/Logs/Monado'
    profile.update(Label=LABEL, ProgramArguments=[str(prefix / 'bin/monado-service')],
                   MachServices={'org.freedesktop.monado.metal-ipc': True},
                   RunAtLoad=False, KeepAlive=False, ProcessType='Interactive',
                   EnvironmentVariables=environment,
                   StandardOutPath=str(logs / 'monado-service.out.log'),
                   StandardErrorPath=str(logs / 'monado-service.err.log'))
    return profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', action='store_true', help='apply preferences and reload the on-demand agent')
    parser.add_argument('--calibration', type=Path, default=CALIBRATION)
    args = parser.parse_args()
    path = Path.home() / f'Library/LaunchAgents/{LABEL}.plist'
    base = plistlib.loads(path.read_bytes()) if path.exists() else {}
    try:
        profile = prepare(base, args.calibration, PREFIX)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    output = ROOT / 'build/arm64/local-preferences' / path.name
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(plistlib.dumps(profile))
    print('Prepared:', output, flush=True)
    for key, value in sorted(profile['EnvironmentVariables'].items()):
        print(f'  {key}={value}', flush=True)
    if not args.install:
        print('Preview only. Apply after rebuilding with: scripts/macos/rebuild-both --install-local')
        return
    control = PREFIX / 'bin/monado-service-xpc-control'
    for required in (control, PREFIX / 'bin/monado-service', PREFIX / 'share/openxr/1/openxr_monado.json'):
        if not required.is_file():
            parser.error(f'Install the native build first: missing {required}')
    backup = ROOT / 'build/arm64/local-install-backups' / datetime.now().strftime('%Y%m%d-%H%M%S-%f-local-preferences')
    backup.mkdir(parents=True)
    if path.exists():
        shutil.copy2(path, backup / path.name)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(plistlib.dumps(profile))
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    subprocess.run([str(control), 'install'], check=True)
    state = subprocess.check_output(['/bin/launchctl', 'print', f'gui/{os.getuid()}/{LABEL}'], text=True)
    for key, value in profile['EnvironmentVariables'].items():
        if f'{key} => {value}' not in state:
            raise RuntimeError(f'Loaded LaunchAgent is missing {key}={value}')
    print('Saved and loaded preferences verified. Service starts on demand; no kickstart requested.')
    print('Previous LaunchAgent backup:', backup)


if __name__ == '__main__':
    main()
