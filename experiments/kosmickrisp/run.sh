#!/bin/sh
# Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0
set -eu
if [ "$#" -lt 1 ]; then
    echo "usage: $0 /absolute/path/to/kosmickrisp_icd.json [probe arguments]" >&2
    exit 2
fi
icd=$1
shift
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/kosmickrisp-probe.XXXXXX")
brew_prefix=$(brew --prefix)
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -I"$brew_prefix/include" "$here/probe.mm" -L"$brew_prefix/lib" -lvulkan \
    -Wl,-rpath,"$brew_prefix/lib" -framework Metal -framework Foundation -o "$out/probe"
python3 - "$out/probe" "$icd" "$@" <<'PY'
import os, subprocess, sys
env = dict(os.environ, VK_DRIVER_FILES=os.path.abspath(sys.argv[2]))
try:
    result = subprocess.run([sys.argv[1], *sys.argv[3:]], env=env, timeout=30)
except subprocess.TimeoutExpired:
    print("FAIL: process exceeded 30-second deadline", file=sys.stderr)
    sys.exit(1)
sys.exit(result.returncode)
PY
