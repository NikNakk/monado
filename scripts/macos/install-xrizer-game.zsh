#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
set -euo pipefail

script_dir=${0:A:h}
runtimes_root=${MONADO_OPENVR_RUNTIMES_ROOT:-${HOME}/Windows/openvr-runtimes}
xrizer_root=${MONADO_XRIZER_ROOT:-${runtimes_root}/xrizer}
xrizer_dll=${MONADO_XRIZER_DLL:-${xrizer_root}/openvr_api.dll}
xrizer_runtime=${MONADO_XRIZER_RUNTIME_DLL:-${xrizer_root}/bin/vrclient_x64.dll}
xrizer_loader=${MONADO_XRIZER_OPENXR_LOADER:-${xrizer_root}/openxr_loader.dll}
wine_prefix=${WINEPREFIX:-${HOME}/Windows/prefix}
runtime_windows="Z:${xrizer_root//\//\\}"

register_runtime()
{
	# Valve's loader uses the first "runtime" entry. Put xrizer first (install)
	# or remove it (restore) in every Windows profile, keeping the other entries
	# so SteamVR stays registered. The CrossOver prefix's user is "crossover".
	python3 - "$1" "${wine_prefix}" "${runtime_windows}" <<'PY'
import json, sys
from pathlib import Path
action, prefix, runtime = sys.argv[1:]
profiles = [p for p in Path(prefix, 'drive_c/users').iterdir() if p.is_dir() and p.name != 'Public']
if not profiles:
    sys.exit(f'No Windows user profiles in {prefix}')
for profile in profiles:
    path = profile / 'AppData/Local/openvr/openvrpaths.vrpath'
    if action != 'install' and not path.exists():
        continue
    data = json.loads(path.read_text()) if path.exists() else {'jsonid': 'vrpathreg', 'version': 1}
    runtimes = [r for r in data.get('runtime', []) if r.lower() != runtime.lower()]
    if action == 'install':
        runtimes.insert(0, runtime)
    data['runtime'] = runtimes
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=1))
    print(f'  OpenVR paths: {path}')
PY
}

usage()
{
	print -u2 "Usage:"
	print -u2 "  $0 install <path/to/game/openvr_api.dll>"
	print -u2 "  $0 restore <path/to/game/openvr_api.dll>"
	exit 2
}

[[ $# -eq 2 ]] || usage
action=$1
target=${2:A}
backup=${target}.monado-original
loader=${target:h}/openxr_loader.dll
loader_backup=${loader}.monado-original

if [[ ${target:t} != openvr_api.dll ]]; then
	print -u2 "Target must be the game's openvr_api.dll: ${target}"
	exit 2
fi

case "${action}" in
install)
	if [[ ! -f "${xrizer_dll}" || ! -f "${xrizer_runtime}" ]]; then
		"${script_dir}/provision-xrizer.zsh"
	fi
	if [[ ! -f "${target}" ]]; then
		print -u2 "Game OpenVR DLL not found: ${target}"
		exit 1
	fi
	if [[ ! -f "${xrizer_loader}" ]]; then
		print -u2 "Provisioned Windows OpenXR loader not found: ${xrizer_loader}"
		exit 1
	fi
	if [[ ! -f "${backup}" ]]; then
		cp -p "${target}" "${backup}"
	else
		print "Keeping existing original backup: ${backup}"
	fi
	# Alyx needs Valve's own OpenVR loader in the game directory, with
	# xrizer registered as the active OpenVR runtime. The target may currently
	# be xrizer, OpenComposite, or one of our compatibility proxies, so do not
	# condition restoration on the current replacement being xrizer.
	#
	# The .monado-original backup is deliberately shared by the OpenComposite
	# and xrizer installers: once it exists, it is the authoritative game DLL
	# to restore before configuring xrizer.
	if ! cmp -s "${backup}" "${target}"; then
		cp -p "${backup}" "${target}"
	fi
	if [[ -f "${loader}" && ! -f "${loader_backup}" ]]; then
		cp -p "${loader}" "${loader_backup}"
	fi

	cp -f "${xrizer_loader}" "${loader}"
	if ! cmp -s "${backup}" "${target}"; then
		print -u2 "Valve OpenVR loader restoration failed verification: ${target}"
		exit 1
	fi
	if ! cmp -s "${xrizer_loader}" "${loader}"; then
		print -u2 "Installed OpenXR loader failed verification: ${loader}"
		exit 1
	fi

	print "Configured xrizer as the OpenVR runtime for this Wine prefix:"
	print "  Valve loader: ${target}"
	print "  original:     ${backup}"
	print "  xrizer:       ${xrizer_runtime}"
	print "  XR loader:    ${loader}"
	register_runtime install
	;;
restore)
	if [[ ! -f "${backup}" ]]; then
		print -u2 "No Monado backup found: ${backup}"
		exit 1
	fi
	mv -f "${backup}" "${target}"
	if [[ -f "${loader_backup}" ]]; then
		mv -f "${loader_backup}" "${loader}"
	elif [[ -f "${loader}" ]] && cmp -s "${xrizer_loader}" "${loader}"; then
		rm -f "${loader}"
	fi
	print "Restored original OpenVR DLL: ${target}"
	print "Unregistered xrizer as an OpenVR runtime:"
	register_runtime restore
	;;
*)
	usage
	;;
esac
