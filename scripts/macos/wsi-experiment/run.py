#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Run the real Monado service/application comparison, restoring launchd and display state."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import plistlib
import re
import signal
import subprocess
import tempfile
import time


def wait_unloaded(domain, label, timeout=15):
    """launchctl bootout returns before launchd has removed the job; wait for it."""
    deadline = time.monotonic() + timeout
    while subprocess.run(['launchctl', 'print', f'{domain}/{label}'], capture_output=True).returncode == 0:
        if time.monotonic() > deadline:
            return False
        time.sleep(0.25)
    return True


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--display-helper', type=pathlib.Path, required=True)
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--hz', type=int, nargs='+', default=[90, 120])
    parser.add_argument('--conditions', nargs='+', choices=['native', 'native-timed', 'wsi', 'wsi-adaptive'],
                        default=['native', 'native-timed', 'wsi', 'wsi-adaptive'])
    parser.add_argument('--native-display-link', choices=['ca', 'cv'], default='ca')
    parser.add_argument('--metal-validation', action='store_true')
    parser.add_argument('--loads', nargs='+', choices=['baseline', 'light', 'cpu', 'gpu4', 'gpu16'], default=['baseline'])
    parser.add_argument('--generic-max-comp-percent', type=int, default=30)
    parser.add_argument('--generic-fake-pacing', action='store_true')
    parser.add_argument('--app-arg', action='append', default=[], metavar='ARG',
                        help='Extra psvr2-openxr-test argument, for example --animate; repeatable')
    parser.add_argument('--service-env', action='append', default=[], metavar='KEY=VALUE',
                        help='Extra service environment variable, for example MoltenVK settings; repeatable')
    parser.add_argument('--generic-latch-us', type=int, default=0, help='Latch allowance for wsi-generic-adaptive')
    parser.add_argument('--hosted', action='store_true')
    parser.add_argument('--refresh-cycle', action='store_true')
    parser.add_argument('--passthrough', choices=['overlay', 'only'])
    parser.add_argument('--passthrough-without-bridge', action='store_true', help='Negative capability check: request passthrough without enabling the WSI bridge')
    args = parser.parse_args()
    if args.seconds <= 0 or args.repeats < 1 or any(hz not in (90, 120) for hz in args.hz):
        parser.error('seconds/repeats must be positive; refresh rates must be 90 or 120')
    if not 1 <= args.generic_max_comp_percent <= 100:
        parser.error('generic compositor budget must be between 1 and 100 percent')
    build = args.build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    helper = args.display_helper.resolve()
    service = build / 'src/xrt/targets/service/monado-service'
    app = build / 'src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test'
    for p in [service, app, helper, build / 'openxr_monado-dev.json']:
        if not p.exists():
            raise RuntimeError(f'Missing {p}')
    domain = f'gui/{os.getuid()}'
    old_label = 'org.freedesktop.monado.service'
    old_plist = pathlib.Path.home() / 'Library/LaunchAgents' / f'{old_label}.plist'
    label = 'org.freedesktop.monado.wsi-experiment'
    old = subprocess.run(['launchctl', 'print', f'{domain}/{old_label}'], capture_output=True, text=True)
    was_loaded = old.returncode == 0
    if was_loaded and re.search(r'^\s*pid = \d+', old.stdout, re.MULTILINE):
        raise RuntimeError('Existing Monado service is running; stop its applications before this experiment')
    if not wait_unloaded(domain, label):
        raise RuntimeError('An experiment job is already loaded')
    initial_mode = subprocess.check_output([str(helper)], text=True).strip()
    manifest = {'start_utc': stamp(), 'initial_mode': initial_mode, 'old_job_was_loaded': was_loaded,
                'build': str(build), 'runs': [], 'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                                          for p in [service, app, helper, build / 'src/xrt/targets/openxr/libopenxr_monado.dylib']}}
    cache = (build / 'CMakeCache.txt').read_text()
    source = pathlib.Path(re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.MULTILINE).group(1))
    manifest['source_commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    manifest['source_branch'] = subprocess.check_output(['git', 'branch', '--show-current'], cwd=source, text=True).strip()
    (output / 'source.diff').write_bytes(subprocess.check_output(['git', 'diff', '--binary'], cwd=source))
    manifest['source_diff_sha256'] = hashlib.sha256((output / 'source.diff').read_bytes()).hexdigest()
    # Record the exact bytes of any uncommitted new sources too.
    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=source, text=True).splitlines()
    manifest['source_untracked_sha256'] = {}
    for name in untracked:
        path = source / name
        if not path.is_file():
            continue
        target = output / 'source-untracked' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = path.read_bytes()
        target.write_bytes(data)
        manifest['source_untracked_sha256'][name] = hashlib.sha256(data).hexdigest()
    manifest_path = output / 'manifest.json'
    runtime = pathlib.Path(tempfile.mkdtemp(prefix='mwsi-'))
    def terminate(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, terminate)
    registered = False
    paused_old = False
    client = None
    try:
        if was_loaded:
            subprocess.run(['launchctl', 'bootout', f'{domain}/{old_label}'], check=True)
            paused_old = True
        for hz in args.hz:
            for repeat in range(args.repeats):
                offset = repeat % len(args.conditions)
                order = args.conditions[offset:] + args.conditions[:offset]
                for load in args.loads:
                    for condition in order:
                        directory = output / f'{hz}hz-r{repeat + 1}-{condition}-{load}'
                        directory.mkdir()
                        env = {
                            'PSVR2_TEST_GPU_PASSES': {'gpu4': '4', 'gpu16': '16'}.get(load, '1'),
                            'PSVR2_TEST_MAX_INSTANCES': '24' if load == 'light' else '10000',
                            'PSVR2_TEST_CPU_DELAY_US': '20000' if load == 'cpu' else '0',
                            'XDG_RUNTIME_DIR': str(runtime), 'XRT_NO_STDIN': '1',
                            'PSVR2_TIMING_TRACE': '1', 'PSVR2_DRIVER_TIMING_TRACE': '1',
                            'PSVR2_TIMING_TRACE_FULLY_BUFFERED': '1', 'PSVR2_TIMING_TRACE_DIR': str(directory),
                            'PSVR2_CAMERA_STREAMS': '1' if args.passthrough else '0', 'PSVR2_GAZE_STREAMS': '0',
                            'XRT_MACOS_CLIENT_COMPOSITOR': '1' if args.hosted else '0', 'XRT_MACOS_GPU_QUEUE_PRIORITY': '0',
                            'XRT_MACOS_DISPLAY_LINK': args.native_display_link, 'XRT_MACOS_REFRESH_RATE_HZ': str(hz),
                            'XRT_MACOS_PRESENTER': condition if condition.startswith('wsi') else 'native',
                            'XRT_MACOS_WSI_GENERIC_LATCH_US': str(args.generic_latch_us),
                            'XRT_COMPOSITOR_PREFERRED_IMAGE_COUNT': '3',
                            'XRT_COMPOSITOR_DEPTH_REPROJECTION': '0',
                            'XRT_COMPOSITOR_USE_DISPLAY_TIMING': '0' if args.generic_fake_pacing else '1',
                            'U_PACING_COMP_PRESENT_TO_DISPLAY_OFFSET_MS': '4',
                            'U_PACING_COMP_MARGIN_MS': '1',
                            'U_PACING_COMP_TIME_MAX_FRACTION_PERCENT': str(args.generic_max_comp_percent),
                            'XRT_MACOS_PRESENT_MIN_DURATION_US': '0' if condition == 'native-timed' else '8000',
                            'XRT_MACOS_EXIT_ON_DISPLAY_LOSS': '0',
                            'IPC_EXIT_WHEN_IDLE': '0', 'IPC_EXIT_ON_DISCONNECT': '0',
                            'XR_RUNTIME_JSON': str(build / 'openxr_monado-dev.json'),
                        }
                        for item in args.service_env:
                            key, sep, value = item.partition('=')
                            if not sep or not key:
                                raise RuntimeError(f'--service-env needs KEY=VALUE, not {item!r}')
                            env[key] = value
                        if args.refresh_cycle:
                            env['PSVR2_TEST_REFRESH_CYCLE'] = '1'
                        if args.passthrough and not args.passthrough_without_bridge:
                            env['XRT_MACOS_WSI_PASSTHROUGH'] = '1'
                        if args.metal_validation:
                            env['MTL_DEBUG_LAYER'] = '1'
                        plist = runtime / 'experiment.plist'
                        plist.write_bytes(plistlib.dumps({
                            'Label': label, 'ProgramArguments': [str(service)], 'RunAtLoad': True,
                            'MachServices': {'org.freedesktop.monado.metal-ipc': True},
                            'EnvironmentVariables': env, 'ProcessType': 'Interactive',
                            'StandardOutPath': str(directory / 'service.stdout.log'),
                            'StandardErrorPath': str(directory / 'service.stderr.log'),
                        }))
                        app_command = [str(app)] + ([('--passthrough-only' if args.passthrough == 'only' else '--passthrough')] if args.passthrough else []) + args.app_arg
                        trace_directory = directory / 'application-traces' if args.hosted else directory
                        if args.hosted:
                            trace_directory.mkdir()
                        run = {'condition': condition, 'load': load, 'gpu_attribution': False, 'requested_hz': hz, 'repeat': repeat + 1,
                               'start_utc': stamp(), 'directory': str(directory), 'environment': env,
                               'service_command': [str(service)], 'application_command': app_command,
                               'hosted': args.hosted, 'refresh_cycle': args.refresh_cycle, 'passthrough': args.passthrough,
                               'trace_directory': str(trace_directory)}
                        manifest['runs'].append(run)
                        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
                        print(f'{hz} Hz repeat {repeat + 1}: {condition} {load}', flush=True)
                        if condition.startswith('wsi-generic'):
                            subprocess.run([str(helper), '--refresh', str(hz)], check=True)
                        subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True)
                        registered = True
                        # The service must create its private Unix socket before launching the app.
                        deadline = time.monotonic() + 30
                        while not any(p.is_socket() for p in runtime.iterdir()):
                            if time.monotonic() > deadline:
                                raise RuntimeError(f'Service socket did not appear; see {directory}')
                            time.sleep(.1)
                        with (directory / 'application.log').open('w') as log:
                            app_env = os.environ.copy()
                            app_env.update(env)
                            app_env['PSVR2_TIMING_TRACE_DIR'] = str(trace_directory)
                            client = subprocess.Popen(app_command, env=app_env, stdout=log, stderr=subprocess.STDOUT)
                            deadline = time.monotonic() + 30
                            while 'session running' not in (directory / 'application.log').read_text():
                                if client.poll() is not None:
                                    raise RuntimeError(f'Application exited {client.returncode}; see {directory}')
                                if time.monotonic() > deadline:
                                    raise RuntimeError(f'Application session timeout; see {directory}')
                                time.sleep(.1)
                            run['session_ready_utc'] = stamp()
                            time.sleep(args.seconds)
                            client.send_signal(signal.SIGINT)
                            run['application_exit_code'] = client.wait(timeout=15)
                            client = None
                        current = subprocess.check_output([str(helper)], text=True).strip()
                        run['measured_display_mode'] = current
                        subprocess.run(['launchctl', 'bootout', f'{domain}/{label}'], check=True)
                        registered = False
                        if not wait_unloaded(domain, label):
                            raise RuntimeError('Experiment job did not unload')
                        time.sleep(2)
                        run['end_utc'] = stamp()
                        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
                        if run['application_exit_code'] != 0:
                            raise RuntimeError(f'Application failed: {directory}')
    finally:
        if client is not None and client.poll() is None:
            client.send_signal(signal.SIGINT)
            try:
                client.wait(timeout=10)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait()
        if registered:
            subprocess.run(['launchctl', 'bootout', f'{domain}/{label}'], check=False)
            wait_unloaded(domain, label)
            time.sleep(2)
        restored = subprocess.run([str(helper), '--restore', initial_mode.split()[0]])
        manifest['display_restore_exit_code'] = restored.returncode
        if paused_old:
            restore = subprocess.run(['launchctl', 'bootstrap', domain, str(old_plist)])
            manifest['job_restore_exit_code'] = restore.returncode
        manifest['end_utc'] = stamp()
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        if restored.returncode or manifest.get('job_restore_exit_code', 0):
            raise RuntimeError('Original display/job restoration failed; see manifest')
    print(f'Captures saved: {output}', flush=True)


if __name__ == '__main__':
    main()
