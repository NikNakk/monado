#!/bin/sh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Spike: user-operated PS VR2 check of the unified macOS handle branch.
#
# Run from the spike worktree with the headset connected and no other Monado
# service running. Each case starts a uniquely named service through the
# existing runner, which removes it again afterwards. Nothing is installed or
# registered permanently.
#
#   scripts/macos/spike-headset-check.sh [/path/to/hello_xr]

set -u

BUILD=${BUILD:-build/arm64}
HELLO=${1:-/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-inproc-mvk/build/hello-xr/src/tests/hello_xr/hello_xr}
OUT=${OUT:-build/spike-headset-$(date +%Y%m%d-%H%M%S)}
RUNNER=scripts/macos/run-vulkan-metal-textures.py
# SPIKE_VK=kk runs the service and client on the installed KosmicKrisp build.
VK_ARGS=""
if [ "${SPIKE_VK:-moltenvk}" = "kk" ]; then
	VK_ARGS="--moltenvk $HOME/Code/alyx-native/build/kk-integration/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib"
elif [ "${SPIKE_VK:-moltenvk}" != "moltenvk" ]; then
	echo "SPIKE_VK must be moltenvk or kk"
	exit 1
fi

mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
git log -1 --format='commit %H %s' > "$SUMMARY"
echo "build $BUILD" >> "$SUMMARY"
echo "vulkan ${SPIKE_VK:-moltenvk}" >> "$SUMMARY"

if pgrep -x monado-service > /dev/null; then
	echo "A monado-service is already running. Stop it first so this check can own the headset."
	exit 1
fi
if [ ! -x "$BUILD/src/xrt/targets/service/monado-service" ]; then
	echo "Missing $BUILD/src/xrt/targets/service/monado-service; build the spike first."
	exit 1
fi

run_case() {
	name=$1
	look=$2
	shift 2
	echo
	echo "=== $name"
	echo "Look for: $look"
	printf "Put the headset on, then press Enter to start (or type s to skip): "
	read answer
	if [ "$answer" = "s" ]; then
		echo "$name: skipped" >> "$SUMMARY"
		return
	fi
	# shellcheck disable=SC2086
	python3 "$RUNNER" --build "$BUILD" --output "$OUT/$name" --headset --no-readbacks $VK_ARGS "$@" \
		> "$OUT/$name.runner.txt" 2>&1
	code=$?
	printf "Runner exit %s. What did you see? (one line): " "$code"
	read seen
	echo "$name: exit=$code seen=$seen" >> "$SUMMARY"
}

# 1. Stereo array swapchain sent from the service as Metal texture handles.
run_case service-array "a steady image in both eyes, distinct colour per eye, for about 10 seconds" --frames 900

# 2. Same through the hosted compositor in the client process.
run_case hosted-array "the same as case 1, with no black frames" --hosted --frames 900

# 3. Unmodified hello_xr: per-view colour swapchains plus depth swapchains.
#    Per-view colour uses IOSurfaces sent by ID; see the note printed below.
if [ -x "$HELLO" ]; then
	run_case service-hello "the hello_xr cubes, tracking your head" --mode hello --hello "$HELLO" --seconds 15
	run_case hosted-hello "the hello_xr cubes, tracking your head" --mode hello --hello "$HELLO" --seconds 15 --hosted
else
	echo "hello_xr not found at $HELLO; skipping the hello_xr cases." | tee -a "$SUMMARY"
fi

echo
echo "Results are in $OUT; summary:"
cat "$SUMMARY"
