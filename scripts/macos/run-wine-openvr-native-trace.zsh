#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
native_build=${MONADO_NATIVE_BUILD_DIR:-${repo_root}/build-wine}
control=${native_build}/src/xrt/targets/service/monado-service-xpc-control
service=${native_build}/src/xrt/targets/service/monado-service
port=${MONADO_WINE_TCP_PORT:-4242}
frames=${MONADO_OPENVR_SMOKE_FRAMES:-3600}
stamp=$(date +%Y%m%d-%H%M%S)
trace_dir=${MONADO_WINE_NATIVE_TRACE_DIR:-/tmp/monado-wine-openvr-${stamp}}
label=org.freedesktop.monado.service
target="gui/$(id -u)/${label}"
service_pid=

if [[ ! -x "${control}" || ! -x "${service}" ]]; then
	print -u2 "Missing native service/control helper under:"
	print -u2 "  ${native_build}/src/xrt/targets/service"
	print -u2 "Build the native tree first, e.g. cmake --build ${native_build} --parallel"
	exit 1
fi

mkdir -p "${trace_dir}"

print "Preparing direct traced native Monado service"
print "  native build: ${native_build}"
print "  trace dir:    ${trace_dir}"
print "  TCP port:     ${port}"
print "  OpenVR frames:${frames}"
print ""

wait_for_listener()
{
	for _ in {1..100}; do
		if command -v lsof >/dev/null 2>&1 && \
		   lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

wait_for_listener_gone()
{
	for _ in {1..100}; do
		if ! command -v lsof >/dev/null 2>&1 || \
		   ! lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

restore_service()
{
	local status=$?
	trap - EXIT INT TERM

	if [[ -n "${service_pid}" ]] && kill -0 "${service_pid}" 2>/dev/null; then
		print ""
		print "Stopping direct traced service to flush CSVs..."
		kill -TERM "${service_pid}" 2>/dev/null || true
		wait_for_listener_gone || true
		wait "${service_pid}" 2>/dev/null || true
	fi

	print "Restoring ordinary development LaunchAgent registration..."
	IPC_WINE_TCP_PORT="${port}" \
	IPC_EXIT_WHEN_IDLE=0 \
		"${control}" bootstrap >/dev/null

	# Match the ordinary Wine smoke setup: leave the TCP listener available.
	launchctl kickstart -k "${target}" >/dev/null 2>&1 || true

	print ""
	print "Trace directory:"
	print "  ${trace_dir}"
	return ${status}
}
trap restore_service EXIT INT TERM

# Unload the launchd-owned service so this directly-launched process owns both
# the Unix/TCP IPC endpoints. Running it from this shell also keeps it in the
# same bootstrap namespace as Wine/DXMT, which is required by the current
# bootstrap-name MTLSharedEvent bridge.
"${control}" bootout >/dev/null 2>&1 || true
wait_for_listener_gone || {
	print -u2 "Existing Monado TCP listener on port ${port} did not exit."
	exit 1
}

print "Starting traced monado-service directly in the Wine bootstrap namespace..."
PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR="${trace_dir}" \
PSVR2_TIMING_TRACE_FULLY_BUFFERED=1 \
IPC_WINE_TCP_PORT="${port}" \
IPC_EXIT_WHEN_IDLE=0 \
XRT_NO_STDIN=1 \
XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1 \
	"${service}" >"${trace_dir}/service.out.log" 2>"${trace_dir}/service.err.log" &
service_pid=$!

if ! wait_for_listener; then
	print -u2 "Direct traced Monado did not listen on 127.0.0.1:${port}."
	print -u2 "Service stderr:"
	tail -n 100 "${trace_dir}/service.err.log" >&2 2>/dev/null || true
	exit 1
fi

print "Direct traced service is listening."
print ""

set +e
MONADO_OPENVR_SMOKE_FRAMES="${frames}" \
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/wine.csv" \
	"${script_dir}/run-wine-openvr-opencomposite-smoke.zsh" 2>&1 | tee "${trace_dir}/smoke.log"
smoke_status=${pipestatus[1]}
set -e

if [[ -f "${trace_dir}/wine.csv" ]]; then
	gpu_frames=$(awk -F, 'NR > 1 && $3 == 1 {n++} END {print n+0}' "${trace_dir}/wine.csv")
	total_frames=$(awk -F, 'NR > 1 {n++} END {print n+0}' "${trace_dir}/wine.csv")
	print ""
	print "Wine GPU-sync frames: ${gpu_frames}/${total_frames}"
	if (( total_frames > 0 && gpu_frames == 0 )); then
		print -u2 "WARNING: capture ran entirely on the CPU fence fallback path."
		print -u2 "Relevant GPU-sync initialization messages:"
		grep -E 'GPU-only|shared-event|shared D3D11 fence|CPU fence fallback|MONADO_WINE_GPU_SYNC' \
			"${trace_dir}/smoke.log" >&2 || true
		print -u2 "Native service messages:"
		grep -E 'bootstrap|shared-event|semaphore' "${trace_dir}/service.err.log" >&2 || true
	fi
fi

if (( smoke_status != 0 )); then
	print -u2 "OpenVR/OpenComposite smoke exited with status ${smoke_status}."
	exit ${smoke_status}
fi

print ""
print "Capture complete. Stopping the traced service so fully-buffered CSVs are flushed..."
kill -TERM "${service_pid}" 2>/dev/null || true
wait_for_listener_gone || true
wait "${service_pid}" 2>/dev/null || true
service_pid=
sleep 0.25

submit_files=("${trace_dir}"/monado_psvr2_*_wine_submit.csv(N))
if (( ${#submit_files[@]} == 0 )); then
	print -u2 "Native Wine submit trace was not created."
	print -u2 "Trace directory contents:"
	ls -la "${trace_dir}" >&2 || true
	print -u2 "Relevant smoke messages:"
	grep -E 'GPU-only|shared-event|shared D3D11 fence|CPU fence fallback|MONADO_WINE_GPU_SYNC' \
		"${trace_dir}/smoke.log" >&2 || true
	exit 1
fi

print "Native Wine submit trace:"
print "  ${submit_files[1]}"
