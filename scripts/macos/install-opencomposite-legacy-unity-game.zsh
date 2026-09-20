#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
oc_root=${MONADO_OPENCOMPOSITE_ROOT:-${wine_root}/opencomposite}
oc_dll=${MONADO_OPENCOMPOSITE_DLL:-${oc_root}/openvr_api.dll}
proxy_build=${MONADO_OPENVR_LEGACY_UNITY_BUILD_DIR:-${repo_root}/build-wine-openvr-legacy-unity}
proxy=${proxy_build}/openvr_api.dll

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
real_oc=${target:h}/openvr_api_opencomposite.dll
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
    # This compatibility target is experimental: rebuild on every install so
    # source updates cannot leave a stale proxy DLL in the build directory.
    "${script_dir}/build-wine-openvr-legacy-unity-proxy.zsh"
    if [[ ! -f "${target}" ]]; then
        print -u2 "Game OpenVR DLL not found: ${target}"
        exit 1
    fi

    # If the normal OpenComposite helper was already used, its backup is the
    # original Valve DLL and the current target is OpenComposite. Preserve the
    # existing backup and simply replace the target with our proxy.
    if [[ ! -f "${backup}" ]]; then
        cp -p "${target}" "${backup}"
    else
        print "Keeping existing original backup: ${backup}"
    fi

    if [[ -f "${config}" && ! -f "${config_backup}" ]]; then
        cp -p "${config}" "${config_backup}"
    fi

    cp -f "${oc_dll}" "${real_oc}"
    cp -f "${proxy}" "${target}"

    objdump_cmd=${OBJDUMP_MINGW:-$(command -v x86_64-w64-mingw32-objdump || true)}
    if [[ -n "${objdump_cmd}" ]]; then
        exports=$("${objdump_cmd}" -p "${target}" 2>/dev/null || true)
        if [[ "${exports}" != *"UnityHooks_SetSubmitParams"* ||
              "${exports}" != *"UnityHooks_GetRenderEventFunc"* ||
              "${exports}" != *"UnitySetGraphicsDevice"* ]]; then
            print -u2 "Installed openvr_api.dll is not the legacy Unity compatibility proxy:"
            print -u2 "  ${target}"
            print -u2 "Expected UnityHooks_SetSubmitParams, UnityHooks_GetRenderEventFunc and UnitySetGraphicsDevice exports."
            exit 1
        fi
    fi
    cat > "${config}" <<'EOF'
; Managed by Monado Wine/OpenComposite legacy Unity helper.
initUsingVulkan=false
logAllOpenVRCalls=false
logGetTrackedProperty=false
EOF

    print "Installed legacy Unity OpenVR compatibility proxy:"
    print "  proxy:         ${target}"
    print "  OpenComposite: ${real_oc}"
    print "  original:      ${backup}"
    print "  config:        ${config}"
    ;;
restore)
    if [[ ! -f "${backup}" ]]; then
        print -u2 "No Monado backup found: ${backup}"
        exit 1
    fi

    mv -f "${backup}" "${target}"
    rm -f "${real_oc}"

    if [[ -f "${config_backup}" ]]; then
        mv -f "${config_backup}" "${config}"
    elif [[ -f "${config}" ]] && grep -q "Managed by Monado Wine/OpenComposite legacy Unity helper" "${config}"; then
        rm -f "${config}"
    fi

    print "Restored original OpenVR DLL: ${target}"
    ;;
*)
    usage
    ;;
esac
