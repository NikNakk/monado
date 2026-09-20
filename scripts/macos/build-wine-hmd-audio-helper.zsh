#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
build_dir=${MONADO_WINE_AUDIO_BUILD_DIR:-${repo_root}/build-wine-audio}

cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++ || true)}
if [[ -z "${cxx}" ]]; then
    print -u2 "MinGW-w64 cross compiler is required."
    print -u2 "Install it with: brew install mingw-w64"
    exit 1
fi

mkdir -p "${build_dir}"
out=${build_dir}/wine-hmd-audio.exe

"${cxx}" \
    -std=c++17 \
    -O2 \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    "${repo_root}/tests/windows/wine_hmd_audio.cpp" \
    -o "${out}" \
    -lole32 \
    -luuid

description=$(file "${out}")
if [[ "${description}" != *"PE32+ executable"*"x86-64"* ]]; then
    print -u2 "Unexpected Wine HMD audio helper architecture:"
    print -u2 "  ${description}"
    exit 1
fi

print "Built Wine HMD audio helper:"
print "  ${out}"
