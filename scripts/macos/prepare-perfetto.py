#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Download hash-pinned official Perfetto SDK/tools outside the checkout."""
import argparse
import hashlib
from pathlib import Path
import urllib.request
import zipfile

VERSION = 'v58.2'
ASSETS = {
    'perfetto-cpp-sdk-src.zip': 'e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09',
    'mac-arm64.zip': '9dbd484a32c9833c95cf58ca5ef86d2c9ad4767149054ad4eaa52bdf027d1507',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-dir', type=Path, required=True)
    parser.add_argument('--tools', action='store_true', help='Also download macOS arm64 trace processor')
    args = parser.parse_args()
    root = args.sdk_dir.resolve()
    repo = Path(__file__).resolve().parents[2]
    if root == repo or repo in root.parents:
        parser.error('Use a dependency directory outside the repository')
    root.mkdir(parents=True, exist_ok=True)
    for name in ['perfetto-cpp-sdk-src.zip'] + (['mac-arm64.zip'] if args.tools else []):
        archive = root / name
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != ASSETS[name]:
            with urllib.request.urlopen(f'https://github.com/google/perfetto/releases/download/{VERSION}/{name}', timeout=30) as response:
                payload = response.read()
            if hashlib.sha256(payload).hexdigest() != ASSETS[name]:
                raise RuntimeError(f'Checksum mismatch for {name}')
            archive.write_bytes(payload)
        target = root / 'tools' if name == 'mac-arm64.zip' else root
        with zipfile.ZipFile(archive) as package:
            for member in package.namelist():
                if member.startswith('/') or '..' in Path(member).parts:
                    raise RuntimeError(f'Invalid archive path: {member}')
            package.extractall(target)
        if name == 'mac-arm64.zip':
            for path in target.rglob('*'):
                if path.is_file():
                    path.chmod(path.stat().st_mode | 0o111)
    print(f'Verified Perfetto {VERSION}: {root}')


if __name__ == '__main__':
    main()
