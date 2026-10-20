#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
set -eu
root=/Users/nickkennedy/Code/alyx-native
export XRIZER_OPENXR_LOADER=/Users/nickkennedy/Code/OpenXR-SDK/build/macos/src/loader/Release/libopenxr_loader.dylib
export XR_RUNTIME_JSON="$root/build/monado-kk-fd-guard/openxr_monado-dev.json"
export XRIZER_MACOS_TIMELINE_HANDOFF=1
export XRIZER_VULKAN_GPU_TIMING=1
export OXR_DEBUG_FORCE_TIMELINE_SEMAPHORES=1
export MTL_DEBUG_LAYER=1
export XRIZER_GPU_TIMING_TRACE="$KK_GUARD_OUTPUT/gpu.csv"
export XDG_STATE_HOME="$KK_GUARD_OUTPUT/state"
export RUST_LOG=info
exec python3 "$root/workspace/xrizer-dual-vulkan/tools/macos/run-vulkan-backend.py" \
 --driver "$KK_GUARD_DRIVER" --icd "$KK_GUARD_ICD" --output "$KK_GUARD_OUTPUT" -- \
 "$root/build/monado-kk-fd-guard/vulkan_submit_probe_timeline" \
 "$root/build/xrizer-gpu-timing/release/libxrizer.dylib" 180
