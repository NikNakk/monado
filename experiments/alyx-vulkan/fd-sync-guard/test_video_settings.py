# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('paired', Path(__file__).with_name('with-matching-monado.py'))
paired = importlib.util.module_from_spec(spec)
spec.loader.exec_module(paired)


class SettingsCapture(unittest.TestCase):
    def test_read_only_settings_and_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            relative = 'game/hlvr/cfg/deckard/video2.txt'
            path = root/'workspace/alyx-native-game-root'/relative
            path.parent.mkdir(parents=True)
            original = b'"video.cfg" {\n"DeviceID" "100"\n"setting.gpu_mem_level" "0"\n"setting.volume_fog_width" "48"\n"private_user_name" "not collected"\n}\n'
            path.write_bytes(original)
            before = path.stat().st_mtime_ns
            result = paired.snapshot_video_settings(root)['files'][relative]
            self.assertEqual(result['settings'], {'DeviceID': '100', 'setting.gpu_mem_level': '0', 'setting.volume_fog_width': '48'})
            self.assertEqual(len(result['sha256']), 64)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(path.stat().st_mtime_ns, before)

    def test_missing_files_do_not_break_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = paired.snapshot_video_settings(Path(temporary))
            self.assertEqual(len(result['files']), 2)
            self.assertTrue(all(not x['exists'] for x in result['files'].values()))

    def test_before_and_after_record_actual_reset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            relative = 'game/hlvr/cfg/deckard/video2.txt'
            path = root/'workspace/alyx-native-game-root'/relative
            path.parent.mkdir(parents=True)
            path.write_text('"setting.gpu_mem_level" "0"')
            before = paired.snapshot_video_settings(root)['files'][relative]
            path.write_text('"setting.gpu_mem_level" "3"')
            after = paired.snapshot_video_settings(root)['files'][relative]
            self.assertEqual(before['settings']['setting.gpu_mem_level'], '0')
            self.assertEqual(after['settings']['setting.gpu_mem_level'], '3')
            self.assertNotEqual(before['sha256'], after['sha256'])


if __name__ == '__main__':
    unittest.main()
