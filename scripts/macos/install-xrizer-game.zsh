#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
xrizer_root=${MONADO_XRIZER_ROOT:-${wine_root}/xrizer}
xrizer_dll=${MONADO_XRIZER_DLL:-${xrizer_root}/openvr_api.dll}
xrizer_runtime=${MONADO_XRIZER_RUNTIME_DLL:-${xrizer_root}/bin/vrclient_x64.dll}
xrizer_loader=${MONADO_XRIZER_OPENXR_LOADER:-${xrizer_root}/openxr_loader.dll}
wine_prefix=${WINEPREFIX:-${wine_root}/prefix}
openvr_paths=${wine_prefix}/drive_c/users/${USER}/AppData/Local/openvr/openvrpaths.vrpath

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

	mkdir -p "${openvr_paths:h}"
	runtime_windows="Z:${xrizer_root//\//\\}"
	runtime_windows_json=${runtime_windows//\\/\\\\}
	cat > "${openvr_paths}" <<EOF
{
	"jsonid": "vrpathreg",
	"runtime": [
		"${runtime_windows_json}"
	],
	"version": 1
}
EOF
	print "Configured xrizer as the OpenVR runtime for this Wine prefix:"
	print "  Valve loader: ${target}"
	print "  original:     ${backup}"
	print "  xrizer:       ${xrizer_runtime}"
	print "  XR loader:    ${loader}"
	print "  OpenVR paths: ${openvr_paths}"
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
	;;
*)
	usage
	;;
esac
