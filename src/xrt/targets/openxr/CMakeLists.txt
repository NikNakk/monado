# Copyright 2019, Collabora, Ltd.
# Copyright 2025-2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

######
# Create a loadable OpenXR driver.

set(RUNTIME_SUFFIX _${XRT_OXR_RUNTIME_SUFFIX})

set(RUNTIME_TARGET
    ${RUNTIME_PREFIX}openxr${RUNTIME_SUFFIX}
    CACHE INTERNAL "" FORCE
	)

if(XRT_FEATURE_IPC_CLIENT)
	set(_libs ipc_client target_ipc_defaults)
else()
	set(_libs st_prober target_lists target_instance comp_main)
endif()

set(_args)
if(TARGET monado)
	list(APPEND _args LIBMONADO monado)
endif()
if(NOT ANDROID)
	list(APPEND _args INSTALL)
	if(XRT_OPENXR_INSTALL_ACTIVE_RUNTIME)
		list(APPEND _args INSTALL_ACTIVE_RUNTIME)
	endif()
endif()

monado_add_openxr_runtime(
	${RUNTIME_TARGET} ${_args}
	SOURCES target.c
	LINK_LIBRARIES ${_libs}
	)
