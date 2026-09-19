#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
native_build=${MONADO_NATIVE_BUILD_DIR:-${repo_root}/build-wine}
control=${native_build}/src/xrt/targets/service/monado-service-xpc-control
port=${MONADO_WINE_TCP_PORT:-4242}
frames=${MONADO_OPENVR_SMOKE_FRAMES:-3600}
stamp=$(date +%Y%m%d-%H%M%S)
trace_dir=${MONADO_WINE_NATIVE_TRACE_DIR:-/tmp/monado-wine-openvr-${stamp}}
label=org.freedesktop.monado.service

if [[ ! -x "${control}" ]]; then
	print -u2 "Missing native service control helper:"
	print -u2 "  ${control}"
	print -u2 "Build the native tree first, e.g. cmake --build ${native_build} --parallel"
	exit 1
fi

mkdir -p "${trace_dir}"

print "Preparing traced native Monado service"
print "  native build: ${native_build}"
print "  trace dir:    ${trace_dir}"
print "  TCP port:     ${port}"
print "  OpenVR frames:${frames}"
print ""

"${control}" bootout >/dev/null 2>&1 || true

PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR="${trace_dir}" \
PSVR2_TIMING_TRACE_FULLY_BUFFERED=1 \
IPC_WINE_TCP_PORT="${port}" \
IPC_EXIT_WHEN_IDLE=0 \
	"${control}" bootstrap

launchctl kickstart -k "gui/$(id -u)/${label}"

restore_service()
{
	print ""
	print "Stopping traced service to flush CSVs..."
	"${control}" bootout >/dev/null 2>&1 || true

	print "Restoring normal development service registration..."
	IPC_WINE_TCP_PORT="${port}" \
	IPC_EXIT_WHEN_IDLE=0 \
		"${control}" bootstrap >/dev/null
}
trap restore_service EXIT

MONADO_OPENVR_SMOKE_FRAMES="${frames}" \
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/wine.csv" \
	"${script_dir}/run-wine-openvr-opencomposite-smoke.zsh"

# The EXIT trap stops launchd's traced instance so fully-buffered streams close.
print ""
print "Capture complete; trace files will be flushed into:"
print "  ${trace_dir}"
