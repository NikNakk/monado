# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

# Standalone probe comparing CAMetalLayer present timing when rendered directly
# versus hosted through the private CAContext/CALayerHost remote-layer API,
# with and without Game Mode. See doc/macos-remote-layer-hosting.md.

function(layer_host_probe_target target)
	# Unlike the rest of the macOS code, this standalone tool uses ARC.
	target_compile_options(${target} PRIVATE -fobjc-arc)
	target_link_libraries(
		${target} PRIVATE "-framework AppKit" "-framework CoreVideo" "-framework Metal"
				  "-framework QuartzCore"
		)
endfunction()

# Command-line tool: timing modes, and the launchd-started host.
add_executable(macos-layer-host-probe macos_layer_host_probe.m)
layer_host_probe_target(macos-layer-host-probe)

# The same program as a games-category app bundle, so Game Mode engages when it
# goes fullscreen. It defaults to the game role.
add_executable(macos-layer-host-probe-game MACOSX_BUNDLE macos_layer_host_probe.m)
layer_host_probe_target(macos-layer-host-probe-game)
target_compile_definitions(macos-layer-host-probe-game PRIVATE LAYER_HOST_PROBE_GAME_BUNDLE=1)
set_target_properties(
	macos-layer-host-probe-game
	PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/game-Info.plist.in"
		   OUTPUT_NAME "macos-layer-host-probe-game"
	)

# Sign the whole bundle ad hoc so its Info.plist is sealed with the binary.
add_custom_command(
	TARGET macos-layer-host-probe-game
	POST_BUILD
	COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:macos-layer-host-probe-game>"
	VERBATIM
	)
