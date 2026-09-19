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
domain="gui/$(id -u)"
target="${domain}/${label}"

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

# A previous version of this helper could boot the job out and then lose a
# bootstrap race. Repair that state if necessary, but never replace a healthy
# registration merely to enable tracing.
if ! launchctl print "${target}" >/dev/null 2>&1; then
	print "No loaded Monado LaunchAgent found; restoring the development registration..."
	IPC_WINE_TCP_PORT="${port}" \
	IPC_EXIT_WHEN_IDLE=0 \
		"${control}" bootstrap
fi

typeset -A previous_env
typeset -A previous_env_set
trace_keys=(
	PSVR2_TIMING_TRACE
	PSVR2_TIMING_TRACE_DIR
	PSVR2_TIMING_TRACE_FULLY_BUFFERED
	IPC_WINE_TCP_PORT
)

for key in "${trace_keys[@]}"; do
	value=$(launchctl getenv "${key}" 2>/dev/null || true)
	if [[ -n "${value}" ]]; then
		previous_env_set[${key}]=1
		previous_env[${key}]="${value}"
	else
		previous_env_set[${key}]=0
		previous_env[${key}]=""
	fi
done

restore_launchd_env()
{
	for key in "${trace_keys[@]}"; do
		if [[ "${previous_env_set[${key}]}" == 1 ]]; then
			launchctl setenv "${key}" "${previous_env[${key}]}" >/dev/null
		else
			launchctl unsetenv "${key}" >/dev/null 2>&1 || true
		fi
	done
}

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

cleanup()
{
	local status=$?
	trap - EXIT INT TERM

	print ""
	print "Stopping traced LaunchAgent process to flush CSVs..."
	launchctl kill SIGTERM "${target}" >/dev/null 2>&1 || true

	# The macOS service handles SIGTERM cooperatively, so allow teardown_all()
	# and atexit trace writers to close their streams before restarting it.
	for _ in {1..100}; do
		if ! command -v lsof >/dev/null 2>&1 || \
		   ! lsof -nP -iTCP@"127.0.0.1:${port}" -sTCP:LISTEN 2>/dev/null | grep -q LISTEN; then
			break
		fi
		sleep 0.1
	done
	sleep 0.25

	restore_launchd_env

	print "Restarting Monado with its previous launchd environment..."
	launchctl kickstart -k "${target}" >/dev/null 2>&1 || true

	print ""
	print "Trace directory:"
	print "  ${trace_dir}"
	return ${status}
}
trap cleanup EXIT INT TERM

# launchctl's per-user environment is inherited by a newly started LaunchAgent.
# The service's normal registration remains loaded throughout.
launchctl setenv PSVR2_TIMING_TRACE 1
launchctl setenv PSVR2_TIMING_TRACE_DIR "${trace_dir}"
launchctl setenv PSVR2_TIMING_TRACE_FULLY_BUFFERED 1
launchctl setenv IPC_WINE_TCP_PORT "${port}"

print "Restarting the existing LaunchAgent with tracing enabled..."
launchctl kickstart -k "${target}"

if ! wait_for_listener; then
	print -u2 "Traced Monado did not listen on 127.0.0.1:${port}."
	print -u2 "Inspect the service log from your existing LaunchAgent registration."
	exit 1
fi

print "Traced service is listening."
print ""

MONADO_OPENVR_SMOKE_FRAMES="${frames}" \
MONADO_WINE_TCP_PORT="${port}" \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/wine.csv" \
	"${script_dir}/run-wine-openvr-opencomposite-smoke.zsh"

print ""
print "Capture complete. The traced service will now be stopped cleanly so fully-buffered CSVs are flushed."
