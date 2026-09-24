#!/bin/zsh
#
# Build the pinned MIT DXMT v0.80 + Basalt IOSurface patches + Monado shared
# D3D11 fence/texture metadata patches, then overlay the matched artifacts into the
# private Wine tree provisioned by provision-wine-dxmt.zsh.
#
# The Basalt build helper pins DXMT v0.80, LLVM 15.0.7, and its Wine build
# tools. Nothing outside MONADO_WINE_DXMT_ROOT is modified.
#

set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}

readonly basalt_commit=6da06cd3fb391bbe1b430015146ba5df76cb78d6
readonly basalt_repo=https://github.com/frenchbaguetteman/BasaltVR.git
readonly monado_patch_glob="${repo_root}/scripts/macos/dxmt-patches/*.patch"

wine_root=${root}/engine/wine-11.10/Wine\ Devel.app/Contents/Resources/wine
prefix=${MONADO_WINEPREFIX:-${root}/prefix}
basalt_checkout=${root}/sources/BasaltVR-${basalt_commit[1,12]}
build_dir=${root}/dxmt-gpu-sync-build
patch_sources=(${~monado_patch_glob})

if [[ ! -x ${wine_root}/bin/wine ]]; then
	print "Private Wine/DXMT stack is not provisioned yet; provisioning it first."
	"${script_dir}/provision-wine-dxmt.zsh"
fi

for tool in git meson ninja cmake curl shasum tar x86_64-w64-mingw32-g++ cmp file; do
	if ! command -v "${tool}" >/dev/null 2>&1; then
		print -u2 "Missing required tool: ${tool}"
		print -u2 "Install the DXMT build prerequisites (for Homebrew: cmake mingw-w64 meson ninja)."
		exit 1
	fi
done

if (( ${#patch_sources[@]} == 0 )); then
	print -u2 "No Monado DXMT patches found under scripts/macos/dxmt-patches."
	exit 1
fi
for patch_source in "${patch_sources[@]}"; do
	if [[ ! -f ${patch_source} ]]; then
		print -u2 "Missing Monado DXMT patch: ${patch_source}"
		exit 1
	fi
done

mkdir -p "${root}/sources"

if [[ ! -d ${basalt_checkout}/.git ]]; then
	print "Cloning pinned BasaltVR build harness..."
	git clone "${basalt_repo}" "${basalt_checkout}"
fi

if [[ -n $(git -C "${basalt_checkout}" status --porcelain --untracked-files=no) ]]; then
	print -u2 "BasaltVR checkout has tracked modifications; refusing to reset:"
	git -C "${basalt_checkout}" status --short >&2
	exit 1
fi

git -C "${basalt_checkout}" fetch --quiet origin "${basalt_commit}"
git -C "${basalt_checkout}" checkout --quiet --detach "${basalt_commit}"
if [[ $(git -C "${basalt_checkout}" rev-parse HEAD) != ${basalt_commit} ]]; then
	print -u2 "Failed to select pinned BasaltVR commit ${basalt_commit}"
	exit 1
fi

for patch_source in "${patch_sources[@]}"; do
	patch_target=${basalt_checkout}/runtime/dxmt-fork/patches/${patch_source:t}
	if [[ ! -f ${patch_target} ]] || ! cmp -s "${patch_source}" "${patch_target}"; then
		cp -f "${patch_source}" "${patch_target}"
	fi
done

print "Building matched DXMT v0.80 + Basalt IOSurface + Monado compatibility/GPU-sync patches..."
BUILD_DIR="${build_dir}" "${basalt_checkout}/runtime/scripts/build-dxmt-fork.zsh"

install_dir=${build_dir}/dxmt/fork-install
windows_dxmt=${install_dir}/x86_64-windows
unix_dxmt=${install_dir}/x86_64-unix

for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
	artifact=${windows_dxmt}/${dll}
	if [[ ! -s ${artifact} ]]; then
		print -u2 "Missing rebuilt DXMT artifact: ${artifact}"
		exit 1
	fi
	description=$(file "${artifact}")
	if [[ ${description} != *"PE32+ executable (DLL)"*"x86-64"* ]]; then
		print -u2 "Unexpected architecture for ${artifact}:"
		print -u2 "  ${description}"
		exit 1
	fi
done

if [[ ! -s ${unix_dxmt}/winemetal.so ]]; then
	print -u2 "Missing rebuilt DXMT artifact: ${unix_dxmt}/winemetal.so"
	exit 1
fi

# Stop only our private Wine server before replacing DLLs that it may have loaded.
if [[ -x ${root}/bin/wineserver-dxmt ]]; then
	"${root}/bin/wineserver-dxmt" -k >/dev/null 2>&1 || true
fi

print "Overlaying GPU-sync-capable matched DXMT set into private Wine..."
mkdir -p "${wine_root}/lib/wine/x86_64-windows" "${wine_root}/lib/wine/x86_64-unix"
for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
	cp -f "${windows_dxmt}/${dll}" "${wine_root}/lib/wine/x86_64-windows/${dll}"
done
cp -f "${unix_dxmt}/winemetal.so" "${wine_root}/lib/wine/x86_64-unix/winemetal.so"

system32=${prefix}/drive_c/windows/system32
if [[ -d ${system32} ]]; then
	for dll in d3d10core.dll d3d11.dll dxgi.dll winemetal.dll; do
		cp -f "${windows_dxmt}/${dll}" "${system32}/${dll}"
	done
fi

stamp=${root}/dxmt-gpu-sync.txt
cat > "${stamp}" <<EOF
DXMT base: v0.80 (MIT)
BasaltVR build harness: ${basalt_commit}
Basalt patches: 0001-0004 from BasaltVR v0.1.0
Monado patches: ${patch_sources:t}
Monado shared-fence GUID: 8a1e78d5-9762-4f7a-b0ad-1d62f6a49d31
Monado shared-texture GUID: 6f5ee9b2-e42a-4f4d-9782-1c730f5464b9
Build output: ${install_dir}
EOF

print ""
print "Installed GPU-sync-capable DXMT into the private Monado Wine stack."
print "  Wine root: ${wine_root}"
print "  Prefix:    ${prefix}"
print "  Stamp:     ${stamp}"
print ""
print "The OpenXR runtime will use GPU-only MTLSharedEvent/Vulkan timeline sync"
print "automatically. Set MONADO_WINE_GPU_SYNC=0 to force the CPU-fence fallback."
