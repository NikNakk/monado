#!/bin/sh
# Build the interop test and run it on one or more Vulkan ICDs.
# usage: run.sh OUTPUT_DIR ICD_JSON [ICD_JSON ...]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$1; shift
mkdir -p "$out"
clang -fobjc-arc -O1 -g -I/opt/homebrew/include -L/opt/homebrew/lib \
  -Wl,-rpath,/opt/homebrew/lib -lvulkan \
  -framework Foundation -framework Metal -framework IOSurface \
  "$here/kk_metal_objects_test.m" -o "$out/kk_metal_objects_test"
status=0
for icd in "$@"; do
  name=$(basename "$(dirname "$(dirname "$(dirname "$icd")")")")-$(basename "$icd" .json)
  echo "== $icd"
  if VK_DRIVER_FILES="$icd" MTL_DEBUG_LAYER=1 "$out/kk_metal_objects_test" \
       > "$out/$name.log" 2>&1; then :; else status=1; fi
  cat "$out/$name.log"
done
exit $status
