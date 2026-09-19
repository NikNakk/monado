#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
oc_root=${MONADO_OPENCOMPOSITE_ROOT:-${wine_root}/opencomposite}
oc_dll=${MONADO_OPENCOMPOSITE_DLL:-${oc_root}/openvr_api.dll}

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
config=${target:h}/opencomposite.ini
config_backup=${config}.monado-original

if [[ ${target:t} != openvr_api.dll ]]; then
	print -u2 "Target must be the game's openvr_api.dll: ${target}"
	exit 2
fi

case "${action}" in
install)
	if [[ ! -f "${oc_dll}" ]]; then
		"${script_dir}/provision-opencomposite.zsh"
	fi
	if [[ ! -f "${target}" ]]; then
		print -u2 "Game OpenVR DLL not found: ${target}"
		exit 1
	fi
	if [[ ! -f "${backup}" ]]; then
		cp -p "${target}" "${backup}"
	else
		print "Keeping existing backup: ${backup}"
	fi
	if [[ -f "${config}" && ! -f "${config_backup}" ]]; then
		cp -p "${config}" "${config_backup}"
	fi

	cp -f "${oc_dll}" "${target}"
	cat > "${config}" <<'EOF'
; Managed by Monado Wine/OpenComposite helper.
; Keep OpenComposite's bootstrap graphics path on D3D11 for this port.
initUsingVulkan=false
EOF
	print "Installed OpenComposite for this game only:"
	print "  replacement: ${target}"
	print "  original:    ${backup}"
	print "  config:      ${config}"
	;;
restore)
	if [[ ! -f "${backup}" ]]; then
		print -u2 "No Monado backup found: ${backup}"
		exit 1
	fi
	mv -f "${backup}" "${target}"
	if [[ -f "${config_backup}" ]]; then
		mv -f "${config_backup}" "${config}"
	elif [[ -f "${config}" ]] && grep -q "Managed by Monado Wine/OpenComposite helper" "${config}"; then
		rm -f "${config}"
	fi
	print "Restored original OpenVR DLL: ${target}"
	;;
*)
	usage
	;;
esac
