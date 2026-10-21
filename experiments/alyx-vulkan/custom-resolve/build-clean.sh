#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
set -eu
root=${ALYX_NATIVE_ROOT:-"$HOME/Code/alyx-native"}
build=${1:-"$root/build/kk-custom-resolve-clean"}
source="$root/workspace/kk-custom-resolve"
venv="$root/workspace/alyx-mesa-py314"
if [ -e "$build" ]; then
 echo "Use a new build directory; rebuild an existing one with ninja -C." >&2
 exit 2
fi
export PATH="/opt/homebrew/opt/llvm/bin:$venv/bin:$PATH"
export PKG_CONFIG_PATH="/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# Invoking the module avoids stale absolute shebangs in the relocated venv.
"$venv/bin/python3.14" -m mesonbuild.mesonmain setup "$build" "$source" \
 -Dbuildtype=release -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
 -Dgallium-drivers= -Dopengl=false -Dzstd=disabled -Dprefer_static=true
ninja -C "$build" -j6 src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib \
 src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json
