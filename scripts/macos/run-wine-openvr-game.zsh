#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
wine=${wine_root}/bin/wine-dxmt
runtime_build=${MONADO_WINE_OPENXR_BUILD_DIR:-${repo_root}/build-wine-openxr}
port=${MONADO_WINE_TCP_PORT:-4242}
trace_host=${MONADO_WINE_TIMING_TRACE_HOST:-/tmp/monado_wine_openvr_timing.csv}
trace_windows="Z:${trace_host//\//\\}"

if (( $# < 1 )); then
	print -u2 "Usage: $0 <Windows VR game.exe> [arguments...]"
	exit 2
fi

game=${1:A}
shift
if [[ ! -f "${game}" ]]; then
	print -u2 "Game executable not found: ${game}"
	exit 1
fi
if [[ ! -x "${wine}" ]]; then
	print -u2 "Missing private Wine/DXMT stack."
	exit 1
fi
if [[ ! -f "${game:h}/openvr_api.dll.monado-original" ]]; then
	print -u2 "This game directory is not configured by the Monado OpenComposite helper."
	print -u2 "Run:"
	print -u2 "  scripts/macos/install-opencomposite-game.zsh install '${game:h}/openvr_api.dll'"
	exit 1
fi

runtime_dll=
for candidate in \
	"${runtime_build}/src/xrt/targets/openxr/libopenxr_monado.dll" \
	"${runtime_build}/src/xrt/targets/openxr/openxr_monado.dll"
do
	if [[ -f "${candidate}" ]]; then
		runtime_dll=${candidate}
		break
	fi
done
if [[ -z "${runtime_dll}" ]]; then
	"${script_dir}/build-wine-openxr-d3d11.zsh"
	for candidate in \
		"${runtime_build}/src/xrt/targets/openxr/libopenxr_monado.dll" \
		"${runtime_build}/src/xrt/targets/openxr/openxr_monado.dll"
	do
		if [[ -f "${candidate}" ]]; then runtime_dll=${candidate}; break; fi
	done
fi
[[ -n "${runtime_dll}" ]] || { print -u2 "Wine Monado OpenXR DLL not found."; exit 1; }

run_dir=${runtime_build}/wine-run
mkdir -p "${run_dir}"
manifest=${run_dir}/openxr_monado-wine.json
windows_runtime="Z:${runtime_dll//\//\\}"
windows_manifest="Z:${manifest//\//\\}"
escaped_runtime=${windows_runtime//\\/\\\\}
cat > "${manifest}" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "library_path": "${escaped_runtime}"
    }
}
EOF

openxr_registry_key='HKLM\SOFTWARE\Khronos\OpenXR\1'
DXMT_BASALT_IOSURFACE=1 "${wine}" reg.exe add "${openxr_registry_key}" \
	/v ActiveRuntime /t REG_SZ /d "${windows_manifest}" /f >/dev/null

if command -v lsof >/dev/null 2>&1 && \
   ! lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
	print -u2 "Native Monado is not listening on 127.0.0.1:${port}."
	exit 1
fi

print "Launching OpenVR game through OpenComposite -> Monado OpenXR"
print "  game:    ${game}"
print "  runtime: ${runtime_dll}"
print "  service: 127.0.0.1:${port}"
print "  trace:   ${trace_host}"
print ""

cd "${game:h}"
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE="${trace_windows}" \
DXMT_BASALT_IOSURFACE=1 \
	"${wine}" "${game}" "$@"
