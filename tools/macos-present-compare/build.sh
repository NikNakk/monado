#!/bin/sh
# SPDX-License-Identifier: BSL-1.0
set -eu
cd "$(dirname "$0")"
prefix=${VULKAN_PREFIX:-/opt/homebrew}
out=${BUILD_DIR:-.build}
mkdir -p "$out"
"${GLSLC:-$prefix/bin/glslc}" -O shader.vert -o "$out/vert.spv"
"${GLSLC:-$prefix/bin/glslc}" -O shader.frag -o "$out/frag.spv"
xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Werror \
  -Wno-missing-field-initializers -Wno-deprecated-declarations \
  -DVK_USE_PLATFORM_METAL_EXT -DVK_ENABLE_BETA_EXTENSIONS \
  -I"$prefix/include" main.mm -L"$prefix/lib" -lvulkan \
  -Wl,-rpath,"$prefix/lib" -framework AppKit -framework Metal \
  -framework QuartzCore -framework CoreVideo -framework IOSurface \
  -o "$out/present-compare"
echo "Built $PWD/$out/present-compare"
