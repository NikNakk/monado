#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check submission changes without adding CI files to the submitted branch."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-ref', required=True)
parser.add_argument('--head-ref', default='HEAD')
parser.add_argument('--report-dir', type=Path, required=True)
parser.add_argument('--mode', choices=['format', 'spelling', 'signoff', 'changelog'], required=True)
args = parser.parse_args()
args.report_dir.mkdir(parents=True, exist_ok=True)


def git(*arguments):
    return subprocess.check_output(['git', *arguments], text=True)


base = git('merge-base', args.base_ref, args.head_ref).strip()
head = git('rev-parse', args.head_ref).strip()
paths = git('diff', '--name-only', '-z', '--diff-filter=ACMRT', base, head).split('\0')
paths = [p for p in paths if p and Path(p).is_file() and not p.startswith('src/external/')]
failed = False
print(f'Checking {base}..{head}: {len(paths)} changed files', flush=True)

if args.mode == 'format':
    c_paths = [p for p in paths if Path(p).suffix in {'.c', '.h', '.cpp', '.hpp', '.m', '.mm'}]
    if c_paths:
        diff = git('diff', '--no-ext-diff', '--no-color', '-U0', base, head, '--', *c_paths)
        result = subprocess.run(['clang-format-diff.py', '-p1'], input=diff, text=True, capture_output=True)
        patch = result.stdout
        (args.report_dir / 'clang-format.patch').write_text(patch)
        if result.returncode or patch.strip():
            print(result.stderr + patch)
            failed = True
    for p in paths:
        if Path(p).name == 'CMakeLists.txt' or Path(p).suffix == '.cmake':
            failed |= subprocess.run(['cmake-format', '-c', '.cmake-format.py', '--check', p]).returncode != 0

elif args.mode == 'spelling':
    failed |= subprocess.run(['git', 'diff', '--check', base, head]).returncode != 0
    # Like the project checker, exclude its own spelling-exception configuration.
    spell_paths = [p for p in paths if p != 'scripts/codespell-project.sh'
                   and Path(p).suffix in {'.c', '.h', '.cpp', '.hpp', '.m', '.mm', '.md', '.py', '.sh', '.comp'}]
    if spell_paths:
        # Check added text so pre-existing spelling in upstream files is not blamed on this series.
        diff = git('diff', '--no-ext-diff', '--no-color', '-U0', base, head, '--', *spell_paths)
        text = '\n'.join(line[1:] for line in diff.splitlines() if line.startswith('+') and not line.startswith('+++'))
        command = ['codespell', '--exclude-file=scripts/monado-codespell.exclude',
                   '--ignore-regex=\\b(pEvent|inout|Kimera|InstallIn)\\b|\\btimebase\\.numer\\b',
                   '--ignore-words-list=ang,sinc,sie,stoll,wil,daa,localy,od,ser,unknwn,parm,inflight,marge,devault,errorprone,childs', '-']
        failed |= subprocess.run(command, input=text, text=True).returncode != 0

elif args.mode == 'signoff':
    commits = git('rev-list', '--reverse', f'{base}..{head}').splitlines()
    for sha in commits:
        author = git('show', '-s', '--format=%an <%ae>', sha).strip()
        message = git('show', '-s', '--format=%B', sha)
        trailers = subprocess.check_output(['git', 'interpret-trailers', '--parse'], input=message, text=True)
        signoffs = [line.partition(':')[2].strip() for line in trailers.splitlines()
                    if line.partition(':')[0].casefold() == 'signed-off-by']
        if not any(s.casefold() == author.casefold() for s in signoffs):
            print(f'ERROR {sha[:12]}: missing author Signed-off-by: {author}')
            failed = True
    print(f'Checked author sign-offs on {len(commits)} commits.')
    print('This checks trailers only; the author must personally certify the DCO.')

elif args.mode == 'changelog':
    fragments = [p for p in paths if p.startswith('doc/changes/') and Path(p).name.startswith('mr.')]
    for p in fragments:
        if not re.fullmatch(r'mr\.\d+\.md', Path(p).name):
            print(f'ERROR {p}: replace the placeholder with the actual MR number')
            failed = True
    print(f'Checked {len(fragments)} changelog fragment filenames.')

sys.exit(1 if failed else 0)
