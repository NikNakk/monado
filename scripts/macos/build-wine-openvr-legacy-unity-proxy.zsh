#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
build_dir=${MONADO_OPENVR_LEGACY_UNITY_BUILD_DIR:-${repo_root}/build-wine-openvr-legacy-unity}

cxx=${CXX_MINGW:-$(command -v x86_64-w64-mingw32-g++ || true)}
if [[ -z "${cxx}" ]]; then
    print -u2 "MinGW-w64 cross compiler is required."
    print -u2 "Install it with: brew install mingw-w64"
    exit 1
fi

mkdir -p "${build_dir}"
out=${build_dir}/openvr_api.dll

"${cxx}" \
    -std=c++17 \
    -O2 \
    -fno-exceptions \
    -fno-rtti \
    -shared \
    -static-libgcc \
    -static-libstdc++ \
    -I "${repo_root}/src/external/openvr_includes" \
    "${repo_root}/tests/windows/openvr_legacy_unity_proxy.cpp" \
    -o "${out}" \
    -ld3d11 \
    -ldxgi \
    -ldxguid \
    -Wl,-Bstatic \
    -lwinpthread \
    -Wl,-Bdynamic

description=$(file "${out}")
if [[ "${description}" != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
    print -u2 "Unexpected legacy Unity proxy architecture:"
    print -u2 "  ${description}"
    exit 1
fi

objdump_cmd=${OBJDUMP_MINGW:-$(command -v x86_64-w64-mingw32-objdump || true)}
if [[ -n "${objdump_cmd}" ]]; then
    imports=$("${objdump_cmd}" -p "${out}" | awk '/DLL Name:/ {print $3}')
    unexpected=()

    while IFS= read -r dll; do
        [[ -n "${dll}" ]] || continue

        case "${dll:l}" in
            kernel32.dll|user32.dll|d3d11.dll|dxgi.dll|ole32.dll|advapi32.dll|msvcrt.dll)
                ;;
            api-ms-win-crt-*.dll)
                ;;
            *)
                unexpected+=("${dll}")
                ;;
        esac
    done <<< "${imports}"

    if (( ${#unexpected[@]} != 0 )); then
        print -u2 "Legacy proxy has unexpected runtime DLL dependencies:"
        printf '%s\n' "${unexpected[@]}" >&2
        exit 1
    fi
fi

print "Built legacy Unity OpenVR proxy:"
print "  ${out}"
print ""
print "The proxy expects a sibling file named:"
print "  openvr_api_opencomposite.dll"
