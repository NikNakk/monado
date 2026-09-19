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
target="gui/$(id -u)/${label}"
plist="/tmp/${label}.$(id -u).plist"

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

restore_service()
{
	local status=$?
	trap - EXIT INT TERM

	print ""
	print "Stopping traced service to flush CSVs..."
	"${control}" bootout >/dev/null 2>&1 || true

	print "Restoring ordinary development service registration..."
	IPC_WINE_TCP_PORT="${port}" \
	IPC_EXIT_WHEN_IDLE=0 \
		"${control}" bootstrap >/dev/null

	# Preserve the behaviour expected by the ordinary smoke runner: a TCP
	# listener is ready rather than waiting for a separate XPC activation.
	launchctl kickstart -k "${target}" >/dev/null 2>&1 || true

	print ""
	print "Trace directory:"
	print "  ${trace_dir}"
	return ${status}
}
trap restore_service EXIT INT TERM

# bootstrap captures the invoking shell's XRT_/PSVR2_/IPC_ environment into
# the development LaunchAgent plist. This is deliberately used instead of
# launchctl setenv: an already-loaded job keeps its registered environment
# across kickstart restarts.
PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR="${trace_dir}" \
PSVR2_TIMING_TRACE_FULLY_BUFFERED=1 \
IPC_WINE_TCP_PORT="${port}" \
IPC_EXIT_WHEN_IDLE=0 \
	"${control}" bootstrap

if [[ ! -f "${plist}" ]]; then
	print -u2 "Trace bootstrap succeeded but the development LaunchAgent plist is missing:"
	print -u2 "  ${plist}"
	exit 1
fi

trace_enabled=$(/usr/libexec/PlistBuddy -c 'Print :EnvironmentVariables:PSVR2_TIMING_TRACE' "${plist}" 2>/dev/null || true)
trace_path=$(/usr/libexec/PlistBuddy -c 'Print :EnvironmentVariables:PSVR2_TIMING_TRACE_DIR' "${plist}" 2>/dev/null || true)
trace_buffered=$(/usr/libexec/PlistBuddy -c 'Print :EnvironmentVariables:PSVR2_TIMING_TRACE_FULLY_BUFFERED' "${plist}" 2>/dev/null || true)
trace_port=$(/usr/libexec/PlistBuddy -c 'Print :EnvironmentVariables:IPC_WINE_TCP_PORT' "${plist}" 2>/dev/null || true)

if [[ "${trace_enabled}" != "1" || "${trace_path}" != "${trace_dir}" || "${trace_buffered}" != "1" || "${trace_port}" != "${port}" ]]; then
	print -u2 "Trace LaunchAgent environment verification failed:"
	print -u2 "  PSVR2_TIMING_TRACE=${trace_enabled:-<missing>}"
	print -u2 "  PSVR2_TIMING_TRACE_DIR=${trace_path:-<missing>}"
	print -u2 "  PSVR2_TIMING_TRACE_FULLY_BUFFERED=${trace_buffered:-<missing>}"
	print -u2 "  IPC_WINE_TCP_PORT=${trace_port:-<missing>}"
	exit 1
fi

print "Verified trace environment in LaunchAgent plist."
launchctl kickstart -k "${target}"

ready=0
for _ in {1..100}; do
	if command -v lsof >/dev/null 2>&1 && \
	   lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
		ready=1
		break
	fi
	sleep 0.1
done

if (( ! ready )); then
	print -u2 "Traced Monado did not listen on 127.0.0.1:${port}."
	print -u2 "LaunchAgent plist: ${plist}"
	print -u2 "Service stderr: /tmp/monado-service-launchd.$(id -u).err.log"
	exit 1
fi

print "Traced service is listening."
print ""

MONADO_OPENVR_SMOKE_FRAMES="${frames}" \
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/wine.csv" \
	"${script_dir}/run-wine-openvr-opencomposite-smoke.zsh"

print ""
print "Capture complete. Stopping the traced service so fully-buffered CSVs are flushed..."

# Stop now rather than waiting for EXIT cleanup, so we can validate the trace
# before restoring the ordinary service.
"${control}" bootout >/dev/null 2>&1 || true
sleep 0.5

submit_files=("${trace_dir}"/monado_psvr2_*_wine_submit.csv(N))
if (( ${#submit_files[@]} == 0 )); then
	print -u2 "Native Wine submit trace was not created despite verified trace environment."
	print -u2 "Trace directory contents:"
	ls -la "${trace_dir}" >&2 || true
	print -u2 "Service stderr tail:"
	tail -n 80 "/tmp/monado-service-launchd.$(id -u).err.log" >&2 2>/dev/null || true
	exit 1
fi

print "Native Wine submit trace:"
print "  ${submit_files[1]}"
