#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Run full REUSE validation on the source scope, retaining archives unchanged."""

import argparse
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile


# Historical evidence and patches for other projects, not Monado source to submit.
# Keep this list narrow: never exclude src/, tests/, or all files of an extension.
ARCHIVE_DIRECTORIES = (
    "doc/alyx-arm64-static-results/",
    "doc/alyx-constrained-host-results/",
    "doc/alyx-strict-vulkan-results/",
    "doc/alyx-valve-lua-results/",
    "doc/alyx-vulkan-results/",
    "experiments/alyx-vulkan/patches/",
)


def stage_source(root, destination):
    """Copy version-controlled and non-ignored new files without archive contents."""
    names = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root,
    ).decode().split("\0")
    included = 0
    excluded = 0
    for name in sorted(set(names) - {""}):
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Unexpected repository path: {name}")
        if name.startswith(ARCHIVE_DIRECTORIES):
            excluded += 1
            continue
        source = root / name
        target = destination / name
        # Match REUSE's handling of deleted files and submodule directories.
        if not source.is_symlink() and not source.is_file():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            target.symlink_to(source.readlink())
        else:
            shutil.copyfile(source, target)
        included += 1
    return included, excluded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    root = args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="monado-source-license-") as temporary:
        source_view = Path(temporary)
        included, excluded = stage_source(root, source_view)
        print(f"Source license scope: {included} files; {excluded} archived files excluded.", flush=True)
        for path in ARCHIVE_DIRECTORIES:
            print(f"  Archive outside source scope: {path}", flush=True)
        print("This checks the source scope, not whole-repository REUSE compliance.", flush=True)
        # Keep every REUSE lint criterion; do not filter errors or assign licenses
        # to archived logs, captured third-party output, or external patches.
        return subprocess.run(["reuse", "--root", str(source_view), "lint"]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
