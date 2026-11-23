#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check fork changes without reformatting unrelated upstream source."""

import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--base-ref", default="9dc151cd2", help="upstream commit used for the current sync")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=root, text=True).splitlines()


paths = set(git("diff", "--name-only", "--diff-filter=ACMRT", args.base_ref))
paths.update(git("ls-files", "--others", "--exclude-standard"))
paths = sorted(p for p in paths if (root / p).is_file() and not p.startswith("src/external/"))
failed = False
# Like GitLab's style job, format only the lines this fork changed, with
# clang-format 14.0.6; whole files and newer releases disagree with upstream.
c_paths = [p for p in paths if Path(p).suffix in (".c", ".h", ".cpp", ".hpp", ".m", ".mm")]
if c_paths:
    diff = subprocess.run(["git", "diff", "--no-ext-diff", "--no-color", "-U0", args.base_ref, "--", *c_paths],
                          cwd=root, text=True, capture_output=True, check=True).stdout
    tracked = set(git("ls-files", "--", *c_paths))
    for path in (p for p in c_paths if p not in tracked):
        diff += subprocess.run(["git", "diff", "--no-index", "--no-color", "-U0", "/dev/null", path],
                               cwd=root, text=True, capture_output=True).stdout
    result = subprocess.run(["clang-format-diff.py", "-p1"], cwd=root, input=diff, text=True, capture_output=True)
    if result.returncode or result.stdout.strip():
        print(result.stderr + result.stdout)
        failed = True
for path in paths:
    if Path(path).name == "CMakeLists.txt" or Path(path).suffix == ".cmake":
        command = ["cmake-format", "--check", "-c", str(root / ".cmake-format.py"), path]
        failed |= subprocess.run(command, cwd=root).returncode != 0

spell_paths = [p for p in paths if Path(p).suffix in
               (".c", ".h", ".cpp", ".hpp", ".m", ".mm", ".md", ".py", ".sh", ".zsh", ".comp")]
if spell_paths:
    command = ["codespell", "--exclude-file=scripts/monado-codespell.exclude",
               "--ignore-regex=\\b(pEvent|inout|Kimera|InstallIn|UE|numer)\\b",
               "--ignore-words-list=ang,sinc,sie,stoll,wil,daa,localy,od,ser,unknwn,parm,inflight,marge,devault,errorprone,childs,zink",
               *spell_paths]
    failed |= subprocess.run(command, cwd=root).returncode != 0
failed |= subprocess.run(["git", "diff", "--check", args.base_ref], cwd=root).returncode != 0
sys.exit(1 if failed else 0)
