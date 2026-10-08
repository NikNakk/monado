#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check fork changes without reformatting unrelated upstream source."""

import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--base-ref", default="ec188bb13", help="upstream commit used for the current sync")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=root, text=True).splitlines()


paths = set(git("diff", "--name-only", "--diff-filter=ACMRT", args.base_ref))
paths.update(git("ls-files", "--others", "--exclude-standard"))
paths = sorted(p for p in paths if (root / p).is_file() and not p.startswith("src/external/"))
failed = False
for path in paths:
    suffix = Path(path).suffix
    command = None
    if suffix in (".c", ".h", ".cpp", ".hpp", ".m", ".mm"):
        command = ["clang-format", "--dry-run", "--Werror", path]
    elif Path(path).name == "CMakeLists.txt" or suffix == ".cmake":
        command = ["cmake-format", "--check", "-c", str(root / ".cmake-format.py"), path]
    if command:
        failed |= subprocess.run(command, cwd=root).returncode != 0

spell_paths = [p for p in paths if Path(p).suffix in
               (".c", ".h", ".cpp", ".hpp", ".m", ".mm", ".md", ".py", ".sh", ".zsh", ".comp")]
if spell_paths:
    command = ["codespell", "--exclude-file=scripts/monado-codespell.exclude",
               "--ignore-regex=\\b(pEvent|inout|Kimera|InstallIn|UE|numer)\\b",
               "--ignore-words-list=ang,sinc,sie,stoll,wil,daa,localy,od,ser,unknwn,parm,inflight,marge,devault,errorprone,childs",
               *spell_paths]
    failed |= subprocess.run(command, cwd=root).returncode != 0
failed |= subprocess.run(["git", "diff", "--check", args.base_ref], cwd=root).returncode != 0
sys.exit(1 if failed else 0)
