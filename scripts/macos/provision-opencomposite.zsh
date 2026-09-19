#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
oc_dir=${MONADO_OPENCOMPOSITE_ROOT:-${root}/opencomposite}
out=${oc_dir}/openvr_api.dll

mkdir -p "${oc_dir}"

# Current OpenComposite's Windows CI remains AppVeyor-based. Allow a local
# artifact or URL to be pinned explicitly; otherwise request the latest x64
# artifact from the project's openxr branch.
source=${MONADO_OPENCOMPOSITE_DLL_SOURCE:-}
default_url='https://ci.appveyor.com/api/projects/ZNix/openovr/artifacts/x64/openvr_api.dll?branch=openxr&job=Platform%3A+x64&pr=false'

if [[ -n ${source} && -f ${source} ]]; then
	cp -f "${source}" "${out}"
elif [[ -n ${source} ]]; then
	curl --fail --location --progress-bar --output "${out}.partial" "${source}"
	mv "${out}.partial" "${out}"
else
	print "Downloading current OpenComposite x64 openxr-branch artifact from AppVeyor..."
	if ! curl --fail --location --progress-bar --output "${out}.partial" "${default_url}"; then
		rm -f "${out}.partial"
		print -u2 "The AppVeyor latest-artifact endpoint was unavailable."
		print -u2 "Set MONADO_OPENCOMPOSITE_DLL_SOURCE to a local x64 openvr_api.dll"
		print -u2 "or to a specific OpenComposite artifact URL and rerun."
		exit 1
	fi
	mv "${out}.partial" "${out}"
fi

description=$(file "${out}")
if [[ ${description} != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
	print -u2 "OpenComposite artifact is not an x86-64 PE DLL:"
	print -u2 "  ${description}"
	exit 1
fi

sha=$(shasum -a 256 "${out}" | awk '{print $1}')
cat > "${oc_dir}/manifest.txt" <<EOF
OpenComposite Windows x64 external artifact
SHA-256: ${sha}
Source: ${source:-${default_url}}
Installed: ${out}
Licence: OpenComposite GPLv3; kept external to Monado.
EOF

# This option keeps the initial OpenComposite graphics probe on D3D11, matching
# the currently-supported Windows graphics path in the Monado Wine bridge.
cat > "${oc_dir}/opencomposite.ini" <<'EOF'
initUsingVulkan=false
EOF

print "Provisioned OpenComposite:"
print "  DLL:      ${out}"
print "  SHA-256:  ${sha}"
print "  config:   ${oc_dir}/opencomposite.ini"
