#!/bin/zsh
#
# Provision the pinned Windows-PCVR D3D11 test stack used by the macOS Monado
# Wine bridge work.
#
# This does not modify /Applications, Whisky, CrossOver, GPTK, or any existing
# Wine prefix. Everything lives below build-wine-dxmt/ by default.
#
# BasaltVR attribution:
#   https://github.com/frenchbaguetteman/BasaltVR
#   MIT license. Its v0.1.0 developer-preview archive contains the matched
#   v0.80-basalt.1 DXMT artifacts built from MIT-licensed DXMT v0.80.
#

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}

readonly wine_version=11.10
readonly wine_archive_name=wine-devel-11.10-osx64.tar.xz
readonly wine_url=https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.10/${wine_archive_name}
readonly wine_sha256=6d0637c3526fbe7051a5f8968dbde68fbc3ac417717648b9331c46642b52173b

readonly basalt_version=0.1.0
readonly basalt_release_commit=6da06cd3fb391bbe1b430015146ba5df76cb78d6
readonly basalt_archive_name=BasaltVR-0.1.0-macos-arm64.zip
readonly basalt_url=https://github.com/frenchbaguetteman/BasaltVR/releases/download/v0.1.0/${basalt_archive_name}
readonly basalt_sha256=2c46277a0ce0a589142c7327f2d38b724d303333fa799d163a6d7625fa543bca

artifact_dir=${root}/artifacts
engine_dir=${root}/engine/wine-${wine_version}
basalt_dir=${root}/basalt-${basalt_version}
prefix=${MONADO_WINEPREFIX:-${root}/prefix}
bin_dir=${root}/bin

wine_root=${engine_dir}/Wine\ Devel.app/Contents/Resources/wine
wine_bin=${wine_root}/bin/wine
wineserver_bin=${wine_root}/bin/wineserver
basalt_support=${basalt_dir}/BasaltVR.app/Contents/Resources/BasaltVR
dxmt_source=${basalt_support}/runtime/build/dxmt/fork-install

for tool in curl shasum tar unzip file grep awk; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		print -u2 "Missing required tool: ${tool}"
		exit 1
	fi
done

if [[ $(uname -s) != Darwin ]]; then
	print -u2 "This provisioning script is macOS-only."
	exit 1
fi

if [[ $(uname -m) == arm64 ]] && ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
	print -u2 "Rosetta 2 is required for the pinned x86-64 Wine engine."
	print -u2 "Install Rosetta, then rerun this script."
	exit 1
fi

sha256_file()
{
	shasum -a 256 "$1" | awk '{print $1}'
}

download_verified()
{
	local url=$1
	local destination=$2
	local expected=$3

	if [[ -f ${destination} ]]; then
		local actual
		actual=$(sha256_file "${destination}")
		if [[ ${actual} == ${expected} ]]; then
			print "Using verified ${destination:t}"
			return
		fi
		print -u2 "Existing artifact checksum mismatch: ${destination}"
		print -u2 "  expected: ${expected}"
		print -u2 "  actual:   ${actual}"
		exit 1
	fi

	local partial=${destination}.partial.$$
	rm -f "${partial}"
	curl --fail --location --progress-bar --output "${partial}" "${url}"

	local actual
	actual=$(sha256_file "${partial}")
	if [[ ${actual} != ${expected} ]]; then
		rm -f "${partial}"
		print -u2 "Downloaded artifact checksum mismatch: ${destination:t}"
		print -u2 "  expected: ${expected}"
		print -u2 "  actual:   ${actual}"
		exit 1
	fi
	mv "${partial}" "${destination}"
}

mkdir -p "${artifact_dir}" "${bin_dir}"

wine_archive=${artifact_dir}/${wine_archive_name}
basalt_archive=${artifact_dir}/${basalt_archive_name}

download_verified "${wine_url}" "${wine_archive}" "${wine_sha256}"
download_verified "${basalt_url}" "${basalt_archive}" "${basalt_sha256}"

if [[ ! -x ${wine_bin} ]]; then
	rm -rf "${engine_dir}"
	mkdir -p "${engine_dir}"
	print "Extracting Gcenx Wine ${wine_version}..."
	tar -xJf "${wine_archive}" -C "${engine_dir}"
fi

reported_version=$("${wine_bin}" --version)
if [[ ${reported_version} != wine-${wine_version} ]]; then
	print -u2 "Unexpected Wine version: ${reported_version}"
	print -u2 "Expected wine-${wine_version}"
	exit 1
fi

if [[ ! -f ${dxmt_source}/x86_64-unix/winemetal.so ]]; then
	rm -rf "${basalt_dir}"
	mkdir -p "${basalt_dir}"
	print "Extracting BasaltVR ${basalt_version} DXMT artifacts..."
	unzip -q "${basalt_archive}" -d "${basalt_dir}"
fi

support_manifest=${basalt_support}/BasaltVR.support.json
if [[ ! -f ${support_manifest} ]] || ! grep -Eq '"dxmtIncluded"[[:space:]]*:[[:space:]]*true' "${support_manifest}"; then
	print -u2 "BasaltVR release does not report bundled DXMT artifacts."
	exit 1
fi

windows_dxmt=${dxmt_source}/x86_64-windows
unix_dxmt=${dxmt_source}/x86_64-unix

for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
	artifact=${windows_dxmt}/${dll}
	if [[ ! -s ${artifact} ]]; then
		print -u2 "Missing Basalt DXMT artifact: ${artifact}"
		exit 1
	fi
	description=$(file "${artifact}")
	if [[ ${description} != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
		print -u2 "Unexpected architecture for ${dll}:"
		print -u2 "  ${description}"
		exit 1
	fi
done

if [[ ! -s ${unix_dxmt}/winemetal.so ]]; then
	print -u2 "Missing Basalt DXMT unix artifact: ${unix_dxmt}/winemetal.so"
	exit 1
fi
unix_description=$(file "${unix_dxmt}/winemetal.so")
if [[ ${unix_description} != *"Mach-O 64-bit"*"x86_64"* ]]; then
	print -u2 "Unexpected architecture for winemetal.so:"
	print -u2 "  ${unix_description}"
	exit 1
fi

print "Overlaying Basalt DXMT v0.80-basalt.1 into the private Wine engine..."
mkdir -p "${wine_root}/lib/wine/x86_64-windows" "${wine_root}/lib/wine/x86_64-unix"
for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
	cp -f "${windows_dxmt}/${dll}" "${wine_root}/lib/wine/x86_64-windows/${dll}"
done
cp -f "${unix_dxmt}/winemetal.so" "${wine_root}/lib/wine/x86_64-unix/winemetal.so"

mkdir -p "${prefix}"
if [[ -x ${wine_root}/bin/wineboot ]]; then
	WINEPREFIX="${prefix}" WINEARCH=win64 WINEDEBUG=-all 		"${wine_root}/bin/wineboot" -u
else
	WINEPREFIX="${prefix}" WINEARCH=win64 WINEDEBUG=-all 		"${wine_bin}" wineboot -u
fi

system32=${prefix}/drive_c/windows/system32
if [[ -d ${system32} ]]; then
	for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
		cp -f "${windows_dxmt}/${dll}" "${system32}/${dll}"
	done
fi

wrapper=${bin_dir}/wine-dxmt
cat > "${wrapper}" <<'WRAPPER'
#!/bin/zsh
set -euo pipefail
root=${0:A:h:h}
wine_root=${root}/engine/wine-11.10/Wine\ Devel.app/Contents/Resources/wine
export WINEPREFIX=${MONADO_WINEPREFIX:-${root}/prefix}
export WINEARCH=win64
export DXMT_BASALT_IOSURFACE=${DXMT_BASALT_IOSURFACE:-1}
export WINEDEBUG=${WINEDEBUG:--all}
export MVK_CONFIG_LOG_LEVEL=${MVK_CONFIG_LOG_LEVEL:-0}
exec "${wine_root}/bin/wine" "$@"
WRAPPER
chmod +x "${wrapper}"

server_wrapper=${bin_dir}/wineserver-dxmt
cat > "${server_wrapper}" <<'WRAPPER'
#!/bin/zsh
set -euo pipefail
root=${0:A:h:h}
wine_root=${root}/engine/wine-11.10/Wine\ Devel.app/Contents/Resources/wine
export WINEPREFIX=${MONADO_WINEPREFIX:-${root}/prefix}
exec "${wine_root}/bin/wineserver" "$@"
WRAPPER
chmod +x "${server_wrapper}"

env_file=${root}/env.zsh
cat > "${env_file}" <<EOF
# Generated by scripts/macos/provision-wine-dxmt.zsh
export MONADO_WINE_DXMT_ROOT="${root}"
export MONADO_WINEPREFIX="${prefix}"
export WINEPREFIX="${prefix}"
export WINEARCH=win64
export DXMT_BASALT_IOSURFACE=1
export WINE="${wrapper}"
EOF

manifest=${root}/manifest.txt
cat > "${manifest}" <<EOF
Monado Windows-PCVR Wine/DXMT toolchain
Wine: Gcenx macOS Wine ${wine_version} x86_64
Wine archive SHA-256: ${wine_sha256}
BasaltVR: ${basalt_version}
BasaltVR release commit: ${basalt_release_commit}
BasaltVR archive SHA-256: ${basalt_sha256}
DXMT: v0.80-basalt.1 (bundled by BasaltVR release)
DXMT gate: DXMT_BASALT_IOSURFACE=1
Prefix: ${prefix}
EOF

WINEPREFIX="${prefix}" WINEARCH=win64 WINEDEBUG=-all 	"${wine_bin}" cmd /c ver >/dev/null

print ""
print "Provisioned private Wine/DXMT stack successfully."
print "  root:       ${root}"
print "  Wine:       ${reported_version}"
print "  DXMT:       v0.80-basalt.1"
print "  prefix:     ${prefix}"
print "  wrapper:    ${wrapper}"
print ""
print "Use it directly:"
print "  ${wrapper} program.exe [args...]"
print ""
print "or load the generated environment:"
print "  source ${env_file}"
print ""
print "No system Wine, Whisky, CrossOver, GPTK, or existing prefix was modified."
