#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
source_dir=${repo_root}/tests/windows/hello_xr
build_dir=${MONADO_WINE_HELLO_XR_BUILD_DIR:-${repo_root}/build-wine-hello-xr}

cc=${CC_MINGW:-$(command -v x86_64-w64-mingw32-gcc || true)}
cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++ || true)}
windres=${WINDRES_MINGW:-$(command -v x86_64-w64-mingw32-windres || true)}

if [[ -z "${cc}" || -z "${cxx}" || -z "${windres}" ]]; then
	print -u2 "MinGW-w64 cross compiler is required."
	print -u2 "Install it with: brew install mingw-w64"
	exit 1
fi

cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_SYSTEM_PROCESSOR=x86_64 \
	-DCMAKE_C_COMPILER="${cc}" \
	-DCMAKE_CXX_COMPILER="${cxx}" \
	-DCMAKE_RC_COMPILER="${windres}" \
	-DCMAKE_BUILD_TYPE=Release

cmake --build "${build_dir}" --target khr_hello_xr_d3d11 --parallel

exe=${build_dir}/khr_hello_xr_d3d11.exe
if [[ ! -f "${exe}" ]]; then
	print -u2 "hello_xr build completed but ${exe} was not found."
	exit 1
fi

description=$(file "${exe}")
if [[ "${description}" != *"PE32+ executable"*"x86-64"* ]]; then
	print -u2 "Unexpected hello_xr architecture:"
	print -u2 "  ${description}"
	exit 1
fi

print "${exe}"
