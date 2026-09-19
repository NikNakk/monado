#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
out_dir=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}/bin
source_file=${repo_root}/tests/windows/macos_wine_d3d11_iosurface_producer.cpp
output=${out_dir}/macos_wine_d3d11_iosurface_producer.exe

cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++ || true)}
if [[ -z ${cxx} ]]; then
	print -u2 "x86_64-w64-mingw32-g++ is required."
	print -u2 "Install it with: brew install mingw-w64"
	exit 1
fi

mkdir -p "${out_dir}"
"${cxx}" 	-std=c++17 	-O2 	-Wall 	-Wextra 	-Wpedantic 	-static 	-static-libgcc 	-static-libstdc++ 	-o "${output}" 	"${source_file}" 	-ld3d11 	-ldxgi 	-ldxguid

description=$(file "${output}")
if [[ ${description} != *"PE32+ executable"*"x86-64"* ]]; then
	print -u2 "Unexpected producer architecture:"
	print -u2 "  ${description}"
	exit 1
fi

print "${output}"
