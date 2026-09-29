# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

# Standalone probe comparing CAMetalLayer present timing when rendered directly
# versus hosted through the private CAContext/CALayerHost remote-layer API.
# See doc/macos-remote-layer-hosting.md.
add_executable(macos-layer-host-probe macos_layer_host_probe.m)

# Unlike the rest of the macOS code, this standalone tool uses ARC.
target_compile_options(macos-layer-host-probe PRIVATE -fobjc-arc)
target_link_libraries(
	macos-layer-host-probe
	PRIVATE
		"-framework AppKit"
		"-framework CoreVideo"
		"-framework Metal"
		"-framework QuartzCore"
	)
