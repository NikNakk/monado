#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
game=${ALYX_GAME_ROOT:-${wine_root}/games/Alyx}
exe=${game}/game/bin/win64/hlvr.exe
game_openvr=${game}/game/bin/win64/openvr_api.dll
xrizer_root=${MONADO_XRIZER_ROOT:-${wine_root}/xrizer}
xrizer_runtime=${MONADO_XRIZER_RUNTIME_DLL:-${xrizer_root}/bin/vrclient_x64.dll}
xrizer_loader=${MONADO_XRIZER_OPENXR_LOADER:-${MONADO_XRIZER_ROOT:-${wine_root}/xrizer}/openxr_loader.dll}
game_loader=${game}/game/bin/win64/openxr_loader.dll
game_openvr_backup=${game_openvr}.monado-original
trace_dir=${ALYX_TRACE_DIR:-/private/tmp/alyx-first-run/xrizer}

if [[ ! -f "${exe}" ]]; then
	print -u2 "Half-Life: Alyx executable not found: ${exe}"
	exit 1
fi
if [[ ! -f "${xrizer_runtime}" ]]; then
	print -u2 "Provisioned xrizer runtime DLL not found: ${xrizer_runtime}"
	print -u2 "Run scripts/macos/provision-xrizer.zsh first."
	exit 1
fi
if [[ ! -f "${game_openvr_backup}" ]] || [[ ! -f "${game_openvr}" ]] || ! cmp -s "${game_openvr_backup}" "${game_openvr}"; then
	print -u2 "Alyx is not configured to use Valve's loader with xrizer."
	print -u2 "Run: scripts/macos/install-xrizer-game.zsh install '${game_openvr}'"
	exit 1
fi
if [[ ! -f "${game_loader}" ]] || ! cmp -s "${xrizer_loader}" "${game_loader}"; then
	print -u2 "Alyx does not have the provisioned Windows OpenXR loader."
	print -u2 "Run: scripts/macos/install-xrizer-game.zsh install '${game_openvr}'"
	exit 1
fi

mkdir -p "${trace_dir}"
trace_windows="Z:${trace_dir//\//\\}"
xrizer_windows="Z:${xrizer_root//\//\\}"

print "Alyx xrizer logs: ${trace_dir}"
RUST_LOG=${RUST_LOG:-xrizer=info,openvr=warn,tracked_property=warn,unknown_interfaces=info} \
XRIZER_ALYX_INPUT_DIAGNOSTICS=${XRIZER_ALYX_INPUT_DIAGNOSTICS:-1} \
XDG_STATE_HOME="${trace_windows}" \
VR_OVERRIDE="${xrizer_windows}" \
WINEDEBUG=${WINEDEBUG:--all} \
XRIZER_PREFER_APPLICATION_PROJECTION=${XRIZER_PREFER_APPLICATION_PROJECTION:-1} \
MONADO_WINE_TIMING_TRACE_HOST="${trace_dir}/alyx-wine.csv" \
	"${script_dir}/run-wine-openvr-game.zsh" \
	"${exe}" \
	-vr \
	-steam \
	-noasserts \
	-nopassiveasserts \
	+map startup \
	-novid -nowindow -console -vconsole +vr_fidelity_level_auto 0 +vr_fidelity_level 3 \
	"$@" 2>&1 | tee "${trace_dir}/alyx-launch.log"

exit ${pipestatus[1]}
