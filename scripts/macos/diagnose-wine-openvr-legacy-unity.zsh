#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
wine_root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
wine=${wine_root}/bin/wine-dxmt

if (( $# != 1 )); then
    print -u2 "Usage: $0 <path/to/InMind.exe>"
    exit 2
fi

game=${1:A}
game_dir=${game:h}
plugin=$(find "${game_dir}" -maxdepth 6 -type f -iname 'openvr_api.dll' -print -quit)
[[ -n "${plugin}" ]] || { print -u2 "Could not find openvr_api.dll beneath ${game_dir}"; exit 1; }

original=${plugin}.monado-original
real_oc=${plugin:h}/openvr_api_opencomposite.dll

print "== Files =="
for f in "${game}" "${plugin}" "${original}" "${real_oc}"; do
    if [[ -f "${f}" ]]; then
        print ""
        print "${f}"
        file "${f}"
    else
        print ""
        print "MISSING: ${f}"
    fi
done

objdump_cmd=${OBJDUMP_MINGW:-$(command -v x86_64-w64-mingw32-objdump || true)}
if [[ -n "${objdump_cmd}" && -f "${plugin}" ]]; then
    print ""
    print "== Proxy imports =="
    "${objdump_cmd}" -p "${plugin}" | awk '/DLL Name:/ {print $3}'
    print ""
    print "== Proxy exports (OpenVR / Unity hooks) =="
    "${objdump_cmd}" -p "${plugin}" | grep -E 'VR_|UnityHooks_|UnitySetGraphicsDevice' || true
fi

print ""
print "== Wine-visible files =="
win_plugin="Z:${plugin//\//\\}"
win_real="Z:${real_oc//\//\\}"
"${wine}" cmd /c dir "${win_plugin}" || true
"${wine}" cmd /c dir "${win_real}" || true

print ""
print "== Native Wine DLL availability =="
for dll in kernel32.dll msvcrt.dll d3d11.dll dxgi.dll ole32.dll user32.dll advapi32.dll; do
    if "${wine}" cmd /c "where ${dll}" >/dev/null 2>&1; then
        print "present: ${dll}"
    else
        print "check:   ${dll}"
    fi
done

print ""
print "If game/proxy bitness match and every proxy import is a Wine/system DLL,"
print "the next useful test is a direct LoadLibrary probe."
