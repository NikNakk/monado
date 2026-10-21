# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Exercise actual OS resource limits in disposable child processes."""
import json
from pathlib import Path
import subprocess
import sys
import unittest


class FileLimitTests(unittest.TestCase):
    def run_child(self, soft, hard=None):
        helper = Path(__file__).with_name('with-matching-monado.py')
        code = '''
import importlib.util,json,resource,sys
spec=importlib.util.spec_from_file_location('helper',sys.argv[1])
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
old_soft,old_hard=resource.getrlimit(resource.RLIMIT_NOFILE)
hard=old_hard if sys.argv[3]=='inherit' else int(sys.argv[3])
resource.setrlimit(resource.RLIMIT_NOFILE,(int(sys.argv[2]),hard))
result=module.prepare_client_file_limit()
print(json.dumps(result))
'''
        process = subprocess.run([sys.executable, '-c', code, str(helper), str(soft),
                                  'inherit' if hard is None else str(hard)],
                                 capture_output=True, text=True, check=True)
        return json.loads(process.stdout)

    def test_terminal_limit_is_raised(self):
        result = self.run_child(256)
        self.assertEqual(result['soft_before'], 256)
        self.assertEqual(result['soft_after'], 8192)
        self.assertTrue(result['minimum_met'])
        self.assertTrue(result['hard_unchanged'])

    def test_higher_existing_limit_is_preserved(self):
        result = self.run_child(16384)
        self.assertEqual(result['soft_after'], 16384)
        self.assertTrue(result['hard_unchanged'])

    def test_hard_limit_is_respected(self):
        result = self.run_child(128, 512)
        self.assertEqual(result['soft_after'], 512)
        self.assertEqual(result['hard'], 512)
        self.assertFalse(result['minimum_met'])
        self.assertTrue(result['hard_unchanged'])


if __name__ == '__main__':
    unittest.main()
