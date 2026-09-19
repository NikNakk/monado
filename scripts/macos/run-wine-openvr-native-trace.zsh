#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
native_build=${MONADO_NATIVE_BUILD_DIR:-${repo_root}/build-wine}
service=${native_build}/src/xrt/targets/service/monado-service
port=${MONADO_WINE_TCP_PORT:-4242}
frames=${MONADO_OPENVR_SMOKE_FRAMES:-3600}
stamp=$(date +%Y%m%d-%H%M%S)
trace_dir=${MONADO_WINE_NATIVE_TRACE_DIR:-/tmp/monado-wine-openvr-${stamp}}
label=org.freedesktop.monado.service
target="gui/$(id -u)/${label}"
service_pid=
registered=0

if [[ ! -x "${service}" ]]; then
	print -u2 "Missing native monado-service:"
	print -u2 "  ${service}"
	print -u2 "Build the native tree first, e.g. cmake --build ${native_build} --parallel"
	exit 1
fi

mkdir -p "${trace_dir}"

print "Preparing traced native Monado service"
print "  native build: ${native_build}"
print "  service:      ${service}"
print "  trace dir:    ${trace_dir}"
print "  TCP port:     ${port}"
print "  OpenVR frames:${frames}"
print ""

if launchctl print "${target}" >/dev/null 2>&1; then
	registered=1
	print "Existing LaunchAgent registration detected; leaving it registered."

	# Stop only the current process. Unlike bootout/bootstrap, this preserves
	# whichever development or persistent registration the user already has.
	launchctl kill SIGTERM "${target}" >/dev/null 2>&1 || true
fi

port_listener_pid()
{
	if ! command -v lsof >/dev/null 2>&1; then
		return 1
	fi
	lsof -nP -tiTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | head -n 1
}

for _ in {1..50}; do
	if [[ -z "$(port_listener_pid || true)" ]]; then
		break
	fi
	sleep 0.1
done

existing_pid=$(port_listener_pid || true)
if [[ -n "${existing_pid}" ]]; then
	existing_command=$(ps -p "${existing_pid}" -o command= 2>/dev/null || true)
	print -u2 "TCP port ${port} is still occupied after stopping the registered service:"
	print -u2 "  PID ${existing_pid}: ${existing_command:-unknown process}"
	print -u2 "Stop that listener and rerun; the trace helper will not kill an unverified process."
	exit 1
fi

restore_service()
{
	local status=$?
	trap - EXIT INT TERM

	if [[ -n "${service_pid}" ]] && kill -0 "${service_pid}" 2>/dev/null; then
		print ""
		print "Stopping traced monado-service to flush CSVs..."
		kill -TERM "${service_pid}" 2>/dev/null || true
		for _ in {1..50}; do
			if ! kill -0 "${service_pid}" 2>/dev/null; then
				break
			fi
			sleep 0.1
		done
		if kill -0 "${service_pid}" 2>/dev/null; then
			kill -KILL "${service_pid}" 2>/dev/null || true
		fi
		wait "${service_pid}" 2>/dev/null || true
	fi

	if (( registered )); then
		print "Restarting the existing LaunchAgent service..."
		launchctl kickstart "${target}" >/dev/null 2>&1 || true
	fi

	print ""
	print "Trace directory:"
	print "  ${trace_dir}"
	return ${status}
}
trap restore_service EXIT INT TERM

print "Starting traced monado-service directly (LaunchAgent registration unchanged)..."
PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR="${trace_dir}" \
PSVR2_TIMING_TRACE_FULLY_BUFFERED=1 \
IPC_WINE_TCP_PORT="${port}" \
IPC_EXIT_WHEN_IDLE=0 \
XRT_NO_STDIN=1 \
	"${service}" >"${trace_dir}/service.out.log" 2>"${trace_dir}/service.err.log" &
service_pid=$!

ready=0
for _ in {1..100}; do
	if ! kill -0 "${service_pid}" 2>/dev/null; then
		print -u2 "Traced monado-service exited before opening TCP port ${port}."
		print -u2 "See: ${trace_dir}/service.err.log"
		exit 1
	fi
	if [[ -n "$(port_listener_pid || true)" ]]; then
		ready=1
		break
	fi
	sleep 0.1
done

if (( ! ready )); then
	print -u2 "Traced monado-service did not listen on 127.0.0.1:${port}."
	print -u2 "See: ${trace_dir}/service.err.log"
	exit 1
fi

print "Traced service is listening."
print ""

MONADO_OPENVR_SMOKE_FRAMES="${frames}" \
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/wine.csv" \
	"${script_dir}/run-wine-openvr-opencomposite-smoke.zsh"

print ""
print "Capture complete. The service will now be stopped so fully-buffered CSVs are flushed."
