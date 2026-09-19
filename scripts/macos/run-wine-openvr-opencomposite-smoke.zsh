#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
wine=${wine_root}/bin/wine-dxmt
runtime_build=${MONADO_WINE_OPENXR_BUILD_DIR:-${repo_root}/build-wine-openxr}
smoke_build=${MONADO_OPENVR_SMOKE_BUILD:-${repo_root}/build-wine-openvr}
graphics_frames=${MONADO_OPENVR_SMOKE_FRAMES:-360}
oc_root=${MONADO_OPENCOMPOSITE_ROOT:-${wine_root}/opencomposite}
oc_dll=${MONADO_OPENCOMPOSITE_DLL:-${oc_root}/openvr_api.dll}
port=${MONADO_WINE_TCP_PORT:-4242}
trace_host=${MONADO_WINE_TIMING_TRACE_HOST:-/tmp/monado_wine_opencomposite_timing.csv}
trace_windows="Z:${trace_host//\//\\}"

if [[ ! -x "${wine}" ]]; then
	print -u2 "Missing private Wine/DXMT stack. Run scripts/macos/provision-wine-dxmt.zsh"
	exit 1
fi
if [[ ! -f "${runtime_build}/openxr_monado-dev.json" ]]; then
	"${script_dir}/build-wine-openxr-d3d11.zsh"
fi
if [[ ! -x "${smoke_build}/openvr_opencomposite_smoke.exe" || \
      ! -x "${smoke_build}/openvr_opencomposite_d3d11_smoke.exe" ]]; then
	"${script_dir}/build-wine-openvr-smoke.zsh"
fi
if [[ ! -f "${oc_dll}" ]]; then
	"${script_dir}/provision-opencomposite.zsh"
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
	print -u2 "Could not find the Wine Monado OpenXR runtime DLL."
	exit 1
fi

case "${port}" in
	''|*[!0-9]*)
		print -u2 "MONADO_WINE_TCP_PORT must be an integer from 1 to 65535."
		exit 2
		;;
esac
if (( port < 1 || port > 65535 )); then
	print -u2 "MONADO_WINE_TCP_PORT must be an integer from 1 to 65535."
	exit 2
fi

if command -v lsof >/dev/null 2>&1; then
	if ! lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
		print -u2 "Native Monado is not listening on 127.0.0.1:${port}."
		exit 1
	fi
fi

run_dir=${runtime_build}/wine-run
mkdir -p "${run_dir}"
manifest=${run_dir}/openxr_monado-wine.json
windows_runtime="Z:${runtime_dll//\//\\}"
windows_manifest="Z:${manifest//\//\\}"
windows_oc_dll="Z:${oc_dll//\//\\}"
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

print "Launching OpenVR/OpenComposite smoke test"
print "  OpenComposite: ${oc_dll}"
print "  OpenXR runtime: ${runtime_dll}"
print "  service: 127.0.0.1:${port}"
print ""

(
	cd "${oc_root}"
	MONADO_WINE_TCP_PORT="${port}" \
	MONADO_WINE_TIMING_TRACE="${trace_windows}" \
	DXMT_BASALT_IOSURFACE=1 \
		"${wine}" "${smoke_build}/openvr_opencomposite_smoke.exe" "${windows_oc_dll}"

	print ""
	print "Running visible D3D11 OpenVR submission smoke (${graphics_frames} frames)..."
	MONADO_WINE_TCP_PORT="${port}" \
	MONADO_WINE_TIMING_TRACE="${trace_windows}" \
	DXMT_BASALT_IOSURFACE=1 \
		"${wine}" "${smoke_build}/openvr_opencomposite_d3d11_smoke.exe" \
			"${windows_oc_dll}" "${graphics_frames}"
)
