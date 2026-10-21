#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: MIT
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$1
mkdir -p "$out"
glslangValidator -V "$here/fullscreen.vert" -o "$out/fullscreen.vert.spv"
glslangValidator -V "$here/fill.frag" -o "$out/fill.frag.spv"
glslangValidator -V -DSAMPLES=1 "$here/resolve.frag" -o "$out/resolve1.frag.spv"
glslangValidator -V -DSAMPLES=4 "$here/resolve.frag" -o "$out/resolve4.frag.spv"
for shader in "$out"/*.spv; do spirv-val "$shader"; done
clang -std=c11 -Wall -Wextra -O0 -g -I/opt/homebrew/include \
  -Wl,-export_dynamic "$here/probe.c" -o "$out/probe"
if [ "$#" -ge 2 ]; then
 kl=$2
 clang -dynamiclib -O1 -I/opt/homebrew/include -I"$kl/runtime" -I"$kl/runtime/gfx" \
  -I"$kl/runtime/guest" "$kl/runtime/gfx/kl_vulkan.c" "$kl/runtime/kl_env.c" \
  -Wl,-undefined,dynamic_lookup -lz -o "$out/shim.dylib"
fi
