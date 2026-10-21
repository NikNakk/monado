#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: MIT
# Build the Alyx-shaped custom resolve probe and its shaders into OUT.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: build.sh OUT}
mkdir -p "$out"
for s in fill.vert fill.frag resolve.frag resolve_nodepth.frag; do
 glslangValidator -V --target-env vulkan1.3 "$here/$s" -o "$out/$s.spv" >/dev/null
 spirv-val --target-env vulkan1.3 "$out/$s.spv"
done
clang -std=c11 -Wall -O0 -g -I/opt/homebrew/include -L/opt/homebrew/lib \
 -Wl,-rpath,/opt/homebrew/lib -lvulkan "$here/probe.c" -o "$out/probe"
