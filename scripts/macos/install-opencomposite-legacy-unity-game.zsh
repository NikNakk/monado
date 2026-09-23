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
custom_bindings_dir=${target:h}/OpenComposite
custom_touch_bindings=${custom_bindings_dir}/oculus_touch.json
custom_touch_backup=${custom_touch_bindings}.monado-original
custom_touch_marker=${custom_touch_bindings}.monado-generated
alyx_cfg_dir=${target:h:h:h}/hlvr/cfg
alyx_actions=${alyx_cfg_dir}/actions.json
alyx_touch_bindings=${alyx_cfg_dir}/bindings_touch.json

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

    # Half-Life: Alyx's Touch bindings use trigger/value for several boolean
    # actions (including menuinteract/menudismiss). OpenComposite's
    # khr/simple_controller fallback cannot bind trigger/value, although it can
    # translate trigger/click to the real simple-controller select/click path.
    # Generate a per-game override that changes only boolean trigger value/pull
    # bindings to click; analogue trigger actions are deliberately preserved.
    if [[ -f "${alyx_actions}" && -f "${alyx_touch_bindings}" ]]; then
        mkdir -p "${custom_bindings_dir}"
        if [[ -f "${custom_touch_bindings}" && ! -f "${custom_touch_marker}" && ! -f "${custom_touch_backup}" ]]; then
            cp -p "${custom_touch_bindings}" "${custom_touch_backup}"
        fi

        changed=$(python3 - "${alyx_actions}" "${alyx_touch_bindings}" "${custom_touch_bindings}" <<'PY'
import json
import sys

actions_path, bindings_path, output_path = sys.argv[1:4]
with open(actions_path, "r", encoding="utf-8-sig") as f:
    manifest = json.load(f)
with open(bindings_path, "r", encoding="utf-8-sig") as f:
    bindings = json.load(f)

action_types = {
    str(item.get("name", "")).lower(): str(item.get("type", "")).lower()
    for item in manifest.get("actions", [])
}

changed = []

# Preserve the generic conversion for boolean trigger value/pull bindings.
for action_set in bindings.get("bindings", {}).values():
    sources = action_set.get("sources", [])
    extra_sources = []

    for source in list(sources):
        path = str(source.get("path", "")).lower()
        if not path.endswith("/input/trigger"):
            continue
        inputs = source.get("inputs")
        if not isinstance(inputs, dict):
            continue

        for source_name in ("value", "pull"):
            item = inputs.get(source_name)
            if not isinstance(item, dict):
                continue
            output = str(item.get("output", "")).lower()
            if action_types.get(output) != "boolean":
                continue

            extra_sources.append({
                "path": source.get("path"),
                "mode": "button",
                "inputs": {"click": dict(item)},
            })
            changed.append(f"trigger-click:{output}")

    sources.extend(extra_sources)

# Alyx's dev menu is the immediate compatibility requirement. OpenComposite
# loads the Touch binding file as its backup for khr/simple_controller, so add
# native simple-controller select/click mappings explicitly instead of relying
# on profile translation or the schema of Alyx's trigger source.
dev = bindings.setdefault("bindings", {}).setdefault("/actions/dev", {})
dev_sources = dev.setdefault("sources", [])

required = []
for hand in ("left", "right"):
    for action in ("menuinteract", "menudismiss"):
        output = f"/actions/dev/in/{action}"
        entry = {
            "path": f"/user/hand/{hand}/input/select",
            "mode": "button",
            "inputs": {"click": {"output": output}},
        }
        dev_sources.append(entry)
        required.append((entry["path"], output))
        changed.append(f"select-click:{hand}:{action}")

# Validate exactly what OpenComposite's KHR simple profile needs. Fail the
# installer rather than silently launching with another ineffective override.
seen = set()
for source in dev_sources:
    path = str(source.get("path", "")).lower()
    item = source.get("inputs", {}).get("click")
    if isinstance(item, dict):
        output = str(item.get("output", "")).lower()
        seen.add((path, output))

missing = [(path, output) for path, output in required if (path, output) not in seen]
if missing:
    raise SystemExit(f"failed to generate Alyx simple-controller menu bindings: {missing}")

with open(output_path, "w", encoding="utf-8") as f:
    json.dump(bindings, f, indent=2)
    f.write("\n")

print(",".join(changed))
PY
)
        : > "${custom_touch_marker}"
        if [[ -n "${changed}" ]]; then
            print "  Alyx input:     ${custom_touch_bindings}"
            print "                  added trigger/click fallbacks for: ${changed}"
        else
            print -u2 "Warning: no Alyx boolean trigger/value bindings were found to adapt."
        fi
    fi

    objdump_cmd=${OBJDUMP_MINGW:-$(command -v x86_64-w64-mingw32-objdump || true)}
    if [[ -n "${objdump_cmd}" ]]; then
        exports=$("${objdump_cmd}" -p "${target}" 2>/dev/null || true)
        if [[ "${exports}" != *"UnityHooks_SetSubmitParams"* ||
              "${exports}" != *"UnityHooks_GetRenderEventFunc"* ||
              "${exports}" != *"UnitySetGraphicsDevice"* ||
              "${exports}" != *"VRControlPanel"* ]]; then
            print -u2 "Installed openvr_api.dll is not the expected legacy compatibility proxy:"
            print -u2 "  ${target}"
            print -u2 "Expected UnityHooks_SetSubmitParams, UnityHooks_GetRenderEventFunc, UnitySetGraphicsDevice and VRControlPanel exports."
            exit 1
        fi
    fi
    trace_openvr=${MONADO_OPENCOMPOSITE_TRACE:-0}
    case "${trace_openvr:l}" in
    1|true|yes|on)
        log_all=true
        log_props=true
        ;;
    *)
        log_all=false
        log_props=false
        ;;
    esac

    cat > "${config}" <<EOF
; Managed by Monado Wine/OpenComposite legacy Unity helper.
initUsingVulkan=false
; Set MONADO_OPENCOMPOSITE_TRACE=1 when running this installer to capture
; the OpenVR API call stream for comparison with xrizer.
logAllOpenVRCalls=${log_all}
logGetTrackedProperty=${log_props}
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

    if [[ -f "${custom_touch_marker}" ]]; then
        if [[ -f "${custom_touch_backup}" ]]; then
            mv -f "${custom_touch_backup}" "${custom_touch_bindings}"
        else
            rm -f "${custom_touch_bindings}"
        fi
        rm -f "${custom_touch_marker}"
    fi

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
