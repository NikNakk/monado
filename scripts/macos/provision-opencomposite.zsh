#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
oc_dir=${MONADO_OPENCOMPOSITE_ROOT:-${root}/opencomposite}
out=${oc_dir}/openvr_api.dll

mkdir -p "${oc_dir}"

# OpenComposite's own RuntimeSwitcher downloads the Windows DLLs from the
# project's build server via znix.xyz. The old public AppVeyor artifact URLs
# now return 404, so use the same upstream feed as the launcher.
#
# MONADO_OPENCOMPOSITE_DLL_SOURCE remains available for a local DLL or a
# specific URL when a fully pinned artifact is desired.
source=${MONADO_OPENCOMPOSITE_DLL_SOURCE:-}
upstream_url='https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr'
resolved_source=

rm -f "${out}.partial"

if [[ -n ${source} && -f ${source} ]]; then
	cp -f "${source}" "${out}"
	resolved_source=${source}
elif [[ -n ${source} ]]; then
	print "Downloading explicitly requested OpenComposite DLL..."
	curl --fail --location --progress-bar --output "${out}.partial" "${source}"
	mv "${out}.partial" "${out}"
	resolved_source=${source}
else
	print "Downloading current OpenComposite x64 openxr build using the upstream launcher feed..."
	if ! curl --fail --location --progress-bar --output "${out}.partial" "${upstream_url}"; then
		rm -f "${out}.partial"
		print -u2 "The OpenComposite launcher download feed was unavailable."
		print -u2 "Set MONADO_OPENCOMPOSITE_DLL_SOURCE to a local x64 openvr_api.dll"
		print -u2 "or to a specific OpenComposite artifact URL and rerun."
		exit 1
	fi
	mv "${out}.partial" "${out}"
	resolved_source=${upstream_url}
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
Source: ${resolved_source}
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
