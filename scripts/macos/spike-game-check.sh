#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Spike: run one Windows Steam game against the spike's Monado, on PS VR2.
#
# Starts the spike's ARM64 service as a private, uniquely named LaunchAgent
# (its own Mach name and socket folder) and points the game's x86_64 client at
# it. The installed service, its LaunchAgent and the rig settings are not
# changed; the private job is removed when the game exits.
#
# Usage: start macOS Steam (Windows Steam must not be running), quit any
# running monado-service, then:
#
#   scripts/macos/spike-game-check.sh APPID [game arguments...]
#
# SPIKE_VK=moltenvk (default) uses Homebrew MoltenVK in the service, as the
# spike was validated. SPIKE_VK=installed keeps the installed service's
# Vulkan driver settings (KosmicKrisp on this rig).

set -euo pipefail
spike=${0:A:h:h:h}
bridge=$HOME/Code/macos-wine-xr
rig=$HOME/Windows
service=${spike}/build/arm64/src/xrt/targets/service/monado-service
client_json=${spike}/build/wine-native/monado-x64/openxr_monado-dev.json
client_lib=${spike}/build/wine-native/monado-x64/src/xrt/targets/openxr/libopenxr_monado.dylib
installed=$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist

[[ $# -ge 1 ]] || { print -u2 'usage: spike-game-check.sh APPID [game arguments...]'; exit 1; }
if pgrep -x monado-service > /dev/null; then
	print -u2 'A monado-service is running (probably the installed one). Quit it first.'
	exit 1
fi
[[ -x ${service} && -f ${client_json} && -f ${installed} ]] || {
	print -u2 'Missing the spike service, its x86_64 client or the installed service plist.'; exit 1
}
service_version=$(strings "${service}" | grep -o 'v25.1.0-[0-9]*-g[0-9a-f]*' | sort -u)
client_version=$(strings "${client_lib}" | grep -o 'v25.1.0-[0-9]*-g[0-9a-f]*' | sort -u)
[[ ${service_version} == ${client_version} ]] || {
	print -u2 "Version mismatch: service ${service_version}, client ${client_version}; rebuild both."; exit 1
}

out=${spike}/build/spike-game-$1-$(date +%Y%m%d-%H%M%S)
mkdir -p "${out}/traces"
label=org.freedesktop.monado.spike-game.${UID}.$$
export XRT_MACOS_METAL_IPC_SERVICE_NAME=org.freedesktop.monado.metal-ipc.spike-game.${UID}.$$
export XDG_RUNTIME_DIR=/private/tmp/monado-spike-game.${UID}.$$
mkdir -p "${XDG_RUNTIME_DIR}"

python3 - "${installed}" "${out}" "${label}" "${service}" "${SPIKE_VK:-moltenvk}" <<'PY'
import os, plistlib, sys
installed, out, label, service, vk = sys.argv[1:]
with open(installed, 'rb') as f:
    base = plistlib.load(f)
env = dict(base.get('EnvironmentVariables', {}))
if vk == 'moltenvk':
    env['VK_DRIVER_FILES'] = '/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json'
    for key in list(env):
        if key.startswith(('KK_', 'MESA_KK', 'MESA_VK_WSI')):
            del env[key]
elif vk != 'installed':
    sys.exit('SPIKE_VK must be moltenvk or installed')
env.update(XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'],
           XRT_MACOS_METAL_IPC_SERVICE_NAME=os.environ['XRT_MACOS_METAL_IPC_SERVICE_NAME'],
           PSVR2_TIMING_TRACE_DIR=out + '/traces', XRT_LOG='info')
plist = dict(Label=label, ProgramArguments=[service], RunAtLoad=False, ProcessType='Interactive',
             MachServices={env['XRT_MACOS_METAL_IPC_SERVICE_NAME']: True}, EnvironmentVariables=env,
             StandardOutPath=out + '/service.log', StandardErrorPath=out + '/service.log')
with open(out + '/service.plist', 'wb') as f:
    plistlib.dump(plist, f)
print('Service Vulkan driver:', env.get('VK_DRIVER_FILES'))
PY

launchctl bootstrap "gui/${UID}" "${out}/service.plist"
cleanup() {
	launchctl bootout "gui/${UID}/${label}" 2>/dev/null || true
	rm -f "${XDG_RUNTIME_DIR}/monado_comp_ipc" "${XDG_RUNTIME_DIR}/monado.pid"
	rmdir "${XDG_RUNTIME_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

print "Spike ${service_version}; logs in ${out}"
print 'Play for a minute or two, then quit the game normally.'

# The rig's settings (~/Windows/run-windows-game.zsh), with the spike's client
# and the private service instead of the installed ones.
source "${bridge}/scripts/steam-play-game-settings.zsh"
unset XR_RUNTIME_JSON IPC_IGNORE_VERSION
export MWXR_IN_PROCESS_PREFIX=${rig}/prefix
export MWXR_IN_PROCESS_WINE=${MWXR_IN_PROCESS_WINE:-$HOME/Code/wine-crossover/runtime-unified}
export MWXR_XRIZER_ROOT=${MWXR_XRIZER_ROOT:-${rig}/openvr-runtimes/xrizer}
export MWXR_NATIVE_RUNTIME_JSON=${client_json}
export MWXR_IN_PROCESS_BUILD=${MWXR_IN_PROCESS_BUILD:-${bridge}/build-in-process/gate}
export GAME_LOG_DIR=${out}
set +e
"${bridge}/scripts/run-windows-game.zsh" "$@"
code=$?
set -e

print
print "Game exit ${code}. Service log: ${out}/service.log"
logs=("${out}"/*.log(N))
(( ${#logs} )) && grep -h -o 'Metal service swapchain backing=[a-z-]*\|Metal XPC published [0-9]* service [A-Za-z]*\|Metal XPC consumed [0-9]* [a-z-]* [A-Za-z]*\|ERROR \[[a-z_]*\]' \
	"${logs[@]}" | sort | uniq -c || true
