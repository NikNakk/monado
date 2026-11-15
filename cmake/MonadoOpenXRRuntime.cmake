# Copyright 2019, Collabora, Ltd.
# Copyright 2025-2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

#.rst:
# MonadoOpenXRRuntime
# -------------------
#
# Provides a function to build a loadable OpenXR runtime library out of
# Monado's OpenXR state tracker, generate its manifests and install it.
#
# Monado's own libopenxr_monado is built with it, projects that embed Monado
# can use it to build their own runtimes. The st_oxr target must exist when
# the function is called, so call it after add_subdirectory() of Monado::
#
#   monado_add_openxr_runtime(
#       <target>                         # Library lib<target> (<target>.dll with MSVC), manifest <target>.json
#       SOURCES <src>...                 # Together with LINK_LIBRARIES must provide xrt_instance_create()
#       [LINK_LIBRARIES <lib>...]        # Such as ipc_client
#       [LIBMONADO <target>]             # Add the libmonado path, to all manifests but the installed Windows one
#       [MANIFEST_TEMPLATE <file>]       # Instead of openxr_runtime/openxr_manifest.in.json, a relative path
#                                        # is relative to the current source dir
#       [DEV_MANIFEST_OUTPUT <file>]     # Instead of <build-dir>[/<config>]/<target>-dev.json
#       [INSTALL]                        # Install the library and manifest, ignored on Android
#       [INSTALL_ACTIVE_RUNTIME]         # With INSTALL, also make it the active runtime, not on Windows
#   )
#
# Only xrNegotiateLoaderRuntimeInterface is exported from the library.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/GenerateOpenXRRuntimeManifest.cmake")

function(monado_add_openxr_runtime target)
	set(options INSTALL INSTALL_ACTIVE_RUNTIME)
	set(oneValueArgs MANIFEST_TEMPLATE DEV_MANIFEST_OUTPUT LIBMONADO)
	set(multiValueArgs SOURCES LINK_LIBRARIES)
	cmake_parse_arguments(_oxr "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

	if(_oxr_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "monado_add_openxr_runtime: unknown arguments ${_oxr_UNPARSED_ARGUMENTS}")
	endif()
	if(_oxr_INSTALL_ACTIVE_RUNTIME AND NOT _oxr_INSTALL)
		message(FATAL_ERROR "monado_add_openxr_runtime: INSTALL_ACTIVE_RUNTIME requires INSTALL")
	endif()
	if(NOT TARGET st_oxr)
		message(FATAL_ERROR "monado_add_openxr_runtime requires the st_oxr target")
	endif()
	if(NOT _oxr_SOURCES)
		message(FATAL_ERROR "monado_add_openxr_runtime requires SOURCES")
	endif()

	# Monado finds Python in its own directory scope, which a parent project can't see.
	if(NOT Python3_EXECUTABLE)
		find_package(Python3 REQUIRED Interpreter)
	endif()

	set(_support_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/openxr_runtime")

	# OpenXR 1.0
	set(XR_API_MAJOR "1")

	if(APPLE)
		# Needed to make a dynlib.
		add_library(${target} SHARED ${_oxr_SOURCES})
	else()
		add_library(${target} MODULE ${_oxr_SOURCES} "${_support_dir}/openxr_runtime.def")
	endif()

	# Note: Order may matter in these lists!
	# TODO: aux_vk is linked even when Monado is built without Vulkan, where the target doesn't exist.
	target_link_libraries(
		${target}
		PUBLIC aux_os aux_util aux_math st_oxr ${_oxr_LINK_LIBRARIES} comp_client
		)

	if(XRT_HAVE_VULKAN)
		target_link_libraries(${target} PUBLIC aux_vk)
	endif()

	if(APPLE)
		# Only expose the negotiate symbol.
		set_property(
			TARGET ${target}
			APPEND_STRING
			PROPERTY LINK_FLAGS "-Wl,-exported_symbol -Wl,_xrNegotiateLoaderRuntimeInterface"
			)
	elseif(NOT WIN32)
		# Force the main "negotiate" symbol's inclusion
		# and use a version script to ensure that's the only one we expose.
		set_property(
			TARGET ${target}
			APPEND_STRING
			PROPERTY
				LINK_FLAGS
				"-u xrNegotiateLoaderRuntimeInterface -Wl,--version-script=\"${_support_dir}/openxr_runtime.version\""
			)
		# Re-link if the version script changes.
		set_property(
			TARGET ${target}
			APPEND
			PROPERTY LINK_DEPENDS "${_support_dir}/openxr_runtime.version"
			)
		# Monado's own linker flags don't reach targets created in a parent project.
		target_link_options(${target} PRIVATE "LINKER:--no-undefined")
	endif()

	set(_libmonado_args)
	if(_oxr_LIBMONADO)
		set(_libmonado_args LIBMONADO ${_oxr_LIBMONADO})
	endif()

	set(_template_args)
	if(_oxr_MANIFEST_TEMPLATE)
		get_filename_component(
			_template "${_oxr_MANIFEST_TEMPLATE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
			)
		set(_template_args MANIFEST_TEMPLATE "${_template}")
	endif()

	###
	# Generate runtime manifest with absolute path to runtime intended for development without installing

	if(_oxr_DEV_MANIFEST_OUTPUT)
		set(_dev_manifest "${_oxr_DEV_MANIFEST_OUTPUT}")
	else()
		# 3.9+ have a global property with the truth
		get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

		if(IS_MULTI_CONFIG)
			set(_dev_manifest "${CMAKE_BINARY_DIR}/$<CONFIG>/${target}-dev.json")
		else()
			set(_dev_manifest "${CMAKE_BINARY_DIR}/${target}-dev.json")
		endif()
	endif()

	generate_openxr_runtime_manifest_buildtree(
		${_libmonado_args} ${_template_args}
		RUNTIME_TARGET ${target}
		OUT_FILE ${_dev_manifest}
		)

	if(NOT _oxr_INSTALL OR ANDROID)
		return()
	endif()

	###
	# Prepare the installable manifest for the configured install layout.
	if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
		# Windows: install manifest to top level
		set(_extra_args)
		if(XRT_OPENXR_INSTALL_ABSOLUTE_RUNTIME_PATH)
			set(_extra_args ABSOLUTE_RUNTIME_PATH)
		elseif(XRT_OPENXR_INSTALL_MANIFEST_RELATIVE_RUNTIME_PATH)
			set(_extra_args RUNTIME_DIR_RELATIVE_TO_MANIFEST ${CMAKE_INSTALL_BINDIR})
		endif()
		generate_openxr_runtime_manifest_at_install(
			${_extra_args} ${_template_args}
			RUNTIME_TARGET ${target}
			DESTINATION .
			RELATIVE_RUNTIME_DIR ${CMAKE_INSTALL_BINDIR}
			)
		install(
			TARGETS ${target}
			LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
			RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
			)
		# This generator expression is easier to control than install(RUNTIME_DEPENDENCY_SETS),
		# which is prone to trying to distribute system DLLs.
		install(
			FILES $<TARGET_RUNTIME_DLLS:${target}>
			DESTINATION ${CMAKE_INSTALL_BINDIR}
			COMPONENT Runtime
			)
	else()
		# Linux, BSD, etc.
		set(MANIFEST_RELATIVE_DIR share/openxr/${XR_API_MAJOR}/)
		if(_oxr_INSTALL_ACTIVE_RUNTIME)
			set(RUNTIME_TARGET ${target})
			set(ACTIVE_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/${target}-active-runtime")
			configure_file(
				"${_support_dir}/active_runtime.cmake.in"
				"${CMAKE_CURRENT_BINARY_DIR}/${target}-active-runtime.cmake" @ONLY
				)
			install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/${target}-active-runtime.cmake")
		endif()

		set(_extra_args)
		if(XRT_OPENXR_INSTALL_ABSOLUTE_RUNTIME_PATH)
			set(_extra_args ABSOLUTE_RUNTIME_PATH)
		elseif(XRT_OPENXR_INSTALL_MANIFEST_RELATIVE_RUNTIME_PATH)
			set(_extra_args RUNTIME_DIR_RELATIVE_TO_MANIFEST ../../../${CMAKE_INSTALL_LIBDIR})
		endif()
		generate_openxr_runtime_manifest_at_install(
			${_extra_args} ${_libmonado_args} ${_template_args}
			RUNTIME_TARGET ${target}
			DESTINATION share/openxr/${XR_API_MAJOR}
			RELATIVE_RUNTIME_DIR ${CMAKE_INSTALL_LIBDIR}
			)
		install(
			TARGETS ${target} #
			LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
			RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
			)
	endif()
endfunction()
