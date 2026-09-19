#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
wine=${wine_root}/bin/wine-dxmt
runtime_build=${MONADO_WINE_OPENXR_BUILD_DIR:-${repo_root}/build-wine-openxr}
hello_build=${MONADO_WINE_HELLO_XR_BUILD_DIR:-${repo_root}/build-wine-hello-xr}
port=${MONADO_WINE_TCP_PORT:-4242}
trace_host=${MONADO_WINE_TIMING_TRACE_HOST:-/tmp/monado_wine_d3d11_timing.csv}
trace_windows="Z:${trace_host//\//\\}"

if [[ ! -x "${wine}" ]]; then
	print -u2 "Missing pinned Wine/DXMT stack. Run:"
	print -u2 "  scripts/macos/provision-wine-dxmt.zsh"
	exit 1
fi

if [[ ! -f "${runtime_build}/openxr_monado-dev.json" ]]; then
	"${script_dir}/build-wine-openxr-d3d11.zsh"
fi

if [[ ! -x "${hello_build}/khr_hello_xr_d3d11.exe" ]]; then
	"${script_dir}/build-wine-hello-xr-d3d11.zsh" >/dev/null
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

# Do not use nc -z here: the service interprets every accepted TCP socket as
# a Monado IPC client, so a port probe creates an unnecessary connect/teardown
# immediately before the real Wine client. Inspect LISTEN state without connecting.
if command -v lsof >/dev/null 2>&1; then
	if ! lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
		print -u2 "Native Monado is not listening on 127.0.0.1:${port}."
		print -u2 ""
		print -u2 "Start the matching native service with the Wine bridge enabled:"
		print -u2 "  IPC_WINE_TCP_PORT=${port} IPC_EXIT_WHEN_IDLE=0 <native-build>/src/xrt/targets/service/monado-service-xpc-control bootstrap"
		print -u2 "  launchctl kickstart -k gui/\$(id -u)/org.freedesktop.monado.service"
		exit 1
	fi
fi

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

#
# Wine currently presents this process to the Khronos Windows OpenXR loader as
# high integrity. The loader deliberately ignores XR_RUNTIME_JSON in that
# context, so install ActiveRuntime in HKLM inside our private Wine prefix
# instead. This modifies only build-wine-dxmt/prefix (or MONADO_WINEPREFIX).
#
openxr_registry_key='HKLM\SOFTWARE\Khronos\OpenXR\1'

if ! DXMT_BASALT_IOSURFACE=1 "${wine}" reg.exe add "${openxr_registry_key}" \
	/v ActiveRuntime /t REG_SZ /d "${windows_manifest}" /f >/dev/null; then
	print -u2 "Failed to register the Wine OpenXR ActiveRuntime."
	exit 1
fi

registered_manifest=$(DXMT_BASALT_IOSURFACE=1 "${wine}" reg.exe query "${openxr_registry_key}" \
	/v ActiveRuntime 2>/dev/null || true)

if [[ "${registered_manifest}" != *"${windows_manifest}"* ]]; then
	print -u2 "Wine OpenXR ActiveRuntime registry verification failed."
	print -u2 "${registered_manifest}"
	exit 1
fi

print "Launching pinned Khronos hello_xr D3D11 against Wine Monado runtime"
print "  runtime: ${runtime_dll}"
print "  manifest: ${windows_manifest}"
print "  service: 127.0.0.1:${port}"
print "  timing trace: ${trace_host}"
print ""

MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE="${trace_windows}" \
DXMT_BASALT_IOSURFACE=1 \
	"${wine}" "${hello_build}/khr_hello_xr_d3d11.exe" \
		--graphics D3D11 --space Local --verbose
