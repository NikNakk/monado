# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import importlib.util
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'local_preferences', Path(__file__).resolve().parents[1] / 'scripts/macos/install-local-preferences.py')
preferences = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preferences)


class LocalPreferencesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.calibration = self.root / 'mode 4.json'
        self.calibration.write_text('{}')

    def test_repair_preserves_unrelated_settings_and_does_not_mutate_base(self):
        base = dict(EnvironmentVariables=dict(IPC_EXIT_WHEN_IDLE_DELAY_MS='9000',
                                               XRT_FLOOR_EYE_HEIGHT_M='1.75', CUSTOM_SETTING='kept'))
        original = plistlib.dumps(base)
        result = preferences.prepare(base, self.calibration, self.root)
        environment = result['EnvironmentVariables']
        self.assertEqual(environment['IPC_EXIT_WHEN_IDLE_DELAY_MS'], '9000')
        self.assertEqual(environment['CUSTOM_SETTING'], 'kept')
        self.assertEqual(environment['XRT_FLOOR_EYE_HEIGHT_M'], '1.72')
        self.assertEqual(environment['PSVR2_SENSE_6DOF_CALIBRATION'], str(self.calibration))
        self.assertEqual(plistlib.dumps(base), original)

    def test_fresh_profile_stays_on_demand_without_experimental_diagnostics(self):
        result = preferences.prepare({}, self.calibration, self.root)
        self.assertEqual(result['ProgramArguments'], [str(self.root / 'bin/monado-service')])
        self.assertFalse(result['RunAtLoad'])
        self.assertFalse(result['KeepAlive'])
        environment = result['EnvironmentVariables']
        for key in ('PSVR2_TIMING_TRACE', 'PSSENSE_TIMING_DIAG', 'PSSENSE_POSE_DIAG',
                    'PSSENSE_IMU_FALLBACK_MS', 'XRT_MACOS_SHARED_TRACKING'):
            self.assertNotIn(key, environment)
        self.assertEqual(plistlib.loads(plistlib.dumps(result)), result)

    def test_missing_calibration_cannot_replace_installed_preferences(self):
        with self.assertRaises(FileNotFoundError):
            preferences.prepare({}, self.root / 'missing.json', self.root)

    def test_incompatible_stage_or_isolated_endpoint_is_refused(self):
        for key, value in (('PSVR2_STAGE_SPACE', '1'), ('XDG_RUNTIME_DIR', '/tmp/isolated'),
                           ('XRT_MACOS_METAL_IPC_SERVICE_NAME', 'isolated')):
            with self.subTest(key=key), self.assertRaises(ValueError):
                preferences.prepare({'EnvironmentVariables': {key: value}}, self.calibration, self.root)


@unittest.skipUnless(shutil.which('zsh'), 'requires zsh')
class RebuildModeTest(unittest.TestCase):
    def run_rebuild(self, arguments, fail=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for command in ('cmake', 'python3', 'launchctl'):
                path = root / command
                path.write_text('#!/bin/sh\n'
                                'printf "%s %s\\n" "${0##*/}" "$*" >> "$MONADO_TEST_LOG"\n'
                                'if [ "${0##*/}" = cmake ]; then exit "${MONADO_TEST_BUILD_EXIT:-0}"; fi\n')
                path.chmod(0o700)
            log = root / 'commands'
            env = dict(os.environ, PATH=f'{root}:{os.environ["PATH"]}', MONADO_TEST_LOG=str(log),
                       MONADO_TEST_BUILD_EXIT='1' if fail else '0')
            result = subprocess.run(['zsh', str(preferences.ROOT / 'scripts/macos/rebuild-both'), *arguments],
                                    env=env, capture_output=True, text=True)
            return result, log.read_text().splitlines() if log.exists() else []

    def test_install_is_explicit_and_follows_both_builds(self):
        result, commands = self.run_rebuild(['--install-local'])
        self.assertEqual(result.returncode, 0, result.stderr)
        wine = next(i for i, line in enumerate(commands) if '--build build/x86_64' in line)
        install = next(i for i, line in enumerate(commands) if 'cmake --install ' in line)
        self.assertGreater(install, wine)
        self.assertTrue(commands[-1].endswith('install-local-preferences.py --install'))
        self.assertFalse(any('kickstart' in line for line in commands))

    def test_default_does_not_install(self):
        result, commands = self.run_rebuild([])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any('--install' in line or 'install-local-preferences.py' in line for line in commands))

    def test_build_failure_prevents_install(self):
        result, commands = self.run_rebuild(['--install-local'], fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any('--install ' in line or line.endswith(' --install') for line in commands))


if __name__ == '__main__':
    unittest.main()
