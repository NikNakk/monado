#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: MIT
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$1
mkdir -p "$out"
glslangValidator -V --target-env vulkan1.1 "$here/fullscreen.vert" -o "$out/fullscreen.vert.spv"
glslangValidator -V --target-env vulkan1.1 "$here/fill.frag" -o "$out/fill.frag.spv"
glslangValidator -V --target-env vulkan1.1 -DSAMPLES=1 "$here/resolve.frag" -o "$out/resolve1.frag.spv"
glslangValidator -V --target-env vulkan1.1 -DSAMPLES=4 "$here/resolve.frag" -o "$out/resolve4.frag.spv"
glslangValidator -V --target-env vulkan1.1 -DSAMPLES=2 "$here/resolve.frag" -o "$out/resolve2.frag.spv"
for samples in 1 2 4; do
 glslangValidator -V --target-env vulkan1.1 -DREAD_DEPTH=1 -DSAMPLES="$samples" "$here/resolve.frag" -o "$out/resolve-depth$samples.frag.spv"
done
for shader in "$out"/*.spv; do spirv-val --target-env vulkan1.1 "$shader"; done
clang -std=c11 -Wall -Wextra -O0 -g -I/opt/homebrew/include \
  -Wl,-export_dynamic "$here/probe.c" -o "$out/probe"
if [ "$#" -ge 2 ]; then
 kl=$2
 clang -dynamiclib -O1 -I/opt/homebrew/include -I"$kl/runtime" -I"$kl/runtime/gfx" \
  -I"$kl/runtime/guest" "$kl/runtime/gfx/kl_vulkan.c" "$kl/runtime/kl_env.c" \
  -Wl,-undefined,dynamic_lookup -lz -o "$out/shim.dylib"
fi
