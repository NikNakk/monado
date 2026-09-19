#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
build_dir=${MONADO_WINE_OPENXR_BUILD_DIR:-${repo_root}/build-wine-openxr}

cc=${CC_MINGW:-$(command -v x86_64-w64-mingw32-gcc || true)}
cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++ || true)}
windres=${WINDRES_MINGW:-$(command -v x86_64-w64-mingw32-windres || true)}

if [[ -z "${cc}" || -z "${cxx}" || -z "${windres}" ]]; then
	print -u2 "MinGW-w64 cross compiler is required."
	print -u2 "Install it with: brew install mingw-w64"
	exit 1
fi

# Older revisions of this branch forced the Wine D3D capability flags into
# CMakeCache.txt. -U removes those stale entries so an existing build directory
# can be reconfigured safely after updating.
cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
	-UXRT_HAVE_DXGI \
	-UXRT_HAVE_D3D11 \
	-UXRT_HAVE_D3D12 \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_SYSTEM_PROCESSOR=x86_64 \
	-DCMAKE_C_COMPILER="${cc}" \
	-DCMAKE_CXX_COMPILER="${cxx}" \
	-DCMAKE_RC_COMPILER="${windres}" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DXRT_FEATURE_WINE_D3D11_BRIDGE=ON \
	-DXRT_FEATURE_SERVICE=OFF \
	-DXRT_FEATURE_CLIENT_WITHOUT_SERVICE=ON \
	-DXRT_MODULE_COMPOSITOR=ON \
	-DXRT_MODULE_COMPOSITOR_CLIENT=ON \
	-DXRT_MODULE_COMPOSITOR_MAIN=OFF \
	-DXRT_MODULE_COMPOSITOR_MULTI=OFF \
	-DXRT_MODULE_COMPOSITOR_NULL=OFF \
	-DXRT_MODULE_COMPOSITOR_RENDER=OFF \
	-DXRT_MODULE_COMPOSITOR_SHADERS=OFF \
	-DXRT_MODULE_COMPOSITOR_UTIL=OFF \
	-DXRT_MODULE_COMPOSITOR_MOCK=OFF \
	-DXRT_HAVE_VULKAN=OFF \
	-DXRT_HAVE_OPENGL=OFF \
	-DXRT_HAVE_OPENGLES=OFF \
	-DXRT_HAVE_SDL2=OFF \
	-DXRT_MODULE_MONADO_CLI=OFF \
	-DXRT_MODULE_MONADO_GUI=OFF \
	-DXRT_BUILD_SAMPLES=OFF \
	-DBUILD_TESTING=OFF \
	-DXRT_FEATURE_DEBUG_GUI=OFF \
	-DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
	-DXRT_FEATURE_TRACING=OFF

cmake --build "${build_dir}" --target openxr_monado --parallel

runtime=
for candidate in \
	"${build_dir}/src/xrt/targets/openxr/openxr_monado.dll" \
	"${build_dir}/src/xrt/targets/openxr/libopenxr_monado.dll"
do
	if [[ -f "${candidate}" ]]; then
		runtime=${candidate}
		break
	fi
done

if [[ -z "${runtime}" ]]; then
	print -u2 "Cross-build completed but openxr_monado.dll was not found."
	exit 1
fi

manifest=${build_dir}/openxr_monado-dev.json
if [[ ! -f "${manifest}" ]]; then
	print -u2 "Cross-build completed but ${manifest} was not generated."
	exit 1
fi

description=$(file "${runtime}")
if [[ "${description}" != *"PE32+"*"x86-64"* ]]; then
	print -u2 "Unexpected runtime architecture:"
	print -u2 "  ${description}"
	exit 1
fi

print "Wine OpenXR runtime built successfully:"
print "  DLL:      ${runtime}"
print "  manifest: ${manifest}"
