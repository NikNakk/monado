#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
out_dir=${MONADO_OPENVR_SMOKE_BUILD:-${repo_root}/build-wine-openvr}
source_file=${repo_root}/tests/windows/openvr_opencomposite_smoke.cpp
graphics_source=${repo_root}/tests/windows/openvr_opencomposite_d3d11_smoke.cpp
openvr_include=${repo_root}/src/external/openvr_includes

compiler=${CXX_MINGW:-x86_64-w64-mingw32-g++}
if ! command -v "${compiler}" >/dev/null 2>&1; then
	print -u2 "Missing ${compiler}. Install mingw-w64 first."
	exit 1
fi

mkdir -p "${out_dir}"
"${compiler}" -std=c++17 -O2 -static-libgcc -static-libstdc++ \
	"${source_file}" \
	-o "${out_dir}/openvr_opencomposite_smoke.exe"

"${compiler}" -std=c++17 -O2 -static-libgcc -static-libstdc++ \
	-I"${openvr_include}" \
	"${graphics_source}" \
	-ld3d11 -ldxgi -ldxguid \
	-o "${out_dir}/openvr_opencomposite_d3d11_smoke.exe"

file "${out_dir}/openvr_opencomposite_smoke.exe"
file "${out_dir}/openvr_opencomposite_d3d11_smoke.exe"
print "Built: ${out_dir}/openvr_opencomposite_smoke.exe"
print "Built: ${out_dir}/openvr_opencomposite_d3d11_smoke.exe"
