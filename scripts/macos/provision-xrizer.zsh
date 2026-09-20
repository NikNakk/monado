#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
xrizer_dir=${MONADO_XRIZER_ROOT:-${root}/xrizer}
out=${xrizer_dir}/openvr_api.dll
runtime_bin=${xrizer_dir}/bin
runtime_out=${runtime_bin}/vrclient_x64.dll
loader_out=${xrizer_dir}/openxr_loader.dll
source=${MONADO_XRIZER_DLL_SOURCE:-}
repository=${MONADO_XRIZER_REPOSITORY:-NikNakk/xrizer}
branch=${MONADO_XRIZER_BRANCH:-windows-openxr-d3d11}
artifact=${MONADO_XRIZER_ARTIFACT:-xrizer-windows-x64-gnu}
loader_version=${MONADO_OPENXR_LOADER_VERSION:-1.1.63}
loader_package=${MONADO_OPENXR_LOADER_PACKAGE:-https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-${loader_version}/OpenXR.Loader.${loader_version}.nupkg}
loader_expected_sha=${MONADO_OPENXR_LOADER_SHA256:-}
resolved_source=
revision=

mkdir -p "${xrizer_dir}" "${runtime_bin}"
rm -f "${out}.partial"

if [[ -n ${source} && -f ${source} ]]; then
	cp -f "${source}" "${out}"
	resolved_source=${source}
elif [[ -n ${source} ]]; then
	print "Downloading explicitly requested xrizer DLL..."
	curl --fail --location --progress-bar --output "${out}.partial" "${source}"
	mv "${out}.partial" "${out}"
	resolved_source=${source}
else
	if ! command -v gh >/dev/null 2>&1; then
		print -u2 "GitHub CLI is required to fetch the xrizer CI artifact."
		print -u2 "Alternatively set MONADO_XRIZER_DLL_SOURCE to a local Win64 openvr_api.dll."
		exit 1
	fi

	run_info=$(gh run list \
		--repo "${repository}" \
		--workflow 'Windows OpenXR' \
		--branch "${branch}" \
		--status success \
		--limit 1 \
		--json databaseId,headSha,url \
		--jq '.[0] | [.databaseId, .headSha, .url] | @tsv')
	if [[ -z ${run_info} ]]; then
		print -u2 "No successful Windows OpenXR workflow found for ${repository}:${branch}."
		exit 1
	fi

	run_id=$(print -r -- "${run_info}" | cut -f1)
	revision=$(print -r -- "${run_info}" | cut -f2)
	run_url=$(print -r -- "${run_info}" | cut -f3)
	tmp_dir=$(mktemp -d /private/tmp/monado-xrizer.XXXXXX)
	trap 'rm -rf -- "${tmp_dir}"' EXIT INT TERM

	print "Downloading xrizer Windows x64 artifact from ${repository}:${branch}..."
	gh run download "${run_id}" \
		--repo "${repository}" \
		--name "${artifact}" \
		--dir "${tmp_dir}"

	if [[ ! -f "${tmp_dir}/openvr_api.dll" ]]; then
		print -u2 "xrizer artifact did not contain openvr_api.dll."
		exit 1
	fi
	cp -f "${tmp_dir}/openvr_api.dll" "${out}"
	resolved_source=${run_url}
fi

description=$(file "${out}")
if [[ ${description} != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
	print -u2 "xrizer artifact is not an x86-64 PE DLL:"
	print -u2 "  ${description}"
	exit 1
fi

# Valve's OpenVR loader looks for this name under <runtime>/bin. Keep the
# openvr_api.dll copy too: it is useful for applications which support a
# direct replacement, but Alyx must use Valve's own loader because it imports
# Valve-private symbols such as VRControlPanel.
cp -f "${out}" "${runtime_out}"

package_tmp=${xrizer_dir}/OpenXR.Loader.nupkg.partial
rm -f "${package_tmp}"
if [[ -f ${loader_package} ]]; then
	cp -f "${loader_package}" "${package_tmp}"
else
	print "Downloading Khronos OpenXR loader ${loader_version}..."
	curl --fail --location --progress-bar --output "${package_tmp}" "${loader_package}"
fi

if [[ -n ${loader_expected_sha} ]]; then
	loader_package_sha=$(shasum -a 256 "${package_tmp}" | awk '{print $1}')
	if [[ ${loader_package_sha} != ${loader_expected_sha} ]]; then
		print -u2 "OpenXR loader package SHA-256 mismatch."
		exit 1
	fi
fi

unzip -jo "${package_tmp}" 'native/x64/release/bin/openxr_loader.dll' -d "${xrizer_dir}" >/dev/null
rm -f "${package_tmp}"
cp -f "${loader_out}" "${runtime_bin}/openxr_loader.dll"

loader_description=$(file "${loader_out}")
if [[ ${loader_description} != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
	print -u2 "OpenXR loader is not an x86-64 PE DLL:"
	print -u2 "  ${loader_description}"
	exit 1
fi

sha=$(shasum -a 256 "${out}" | awk '{print $1}')
loader_sha=$(shasum -a 256 "${loader_out}" | awk '{print $1}')
cat > "${xrizer_dir}/manifest.txt" <<EOF
xrizer Windows x64 external artifact
SHA-256: ${sha}
Source: ${resolved_source}
Repository: ${repository}
Branch: ${branch}
Revision: ${revision:-unknown}
Installed: ${out}
OpenVR runtime DLL: ${runtime_out}
Licence: xrizer GPL-3.0-or-later; kept external to Monado.
OpenXR loader: Khronos ${loader_version}
OpenXR loader SHA-256: ${loader_sha}
OpenXR loader source: ${loader_package}
EOF

print "Provisioned xrizer:"
print "  DLL:      ${out}"
print "  runtime:  ${runtime_out}"
print "  SHA-256:  ${sha}"
print "  revision: ${revision:-unknown}"
print "  loader:   ${loader_out} (${loader_version})"
