// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface for IPC client instance code.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif


struct xrt_instance;
struct xrt_instance_info;

/*!
 * Connection parameters chosen by the target that loads the IPC client.
 *
 * @ingroup ipc_client
 */
struct ipc_client_connection_info
{
	//! Socket filename (named pipe name on Windows) in the runtime dir, required except on Android.
	const char *socket_filename;

	//! Windows only: service executable to launch if no service is running, NULL disables launching.
	const char *service_executable;
};

/*!
 * Create a IPC client instance, connects to a IPC server.
 *
 * @param icci      Which service to connect to, except on Android it must be set and have a socket_filename.
 * @param i_info    Instance info to send to the service.
 * @param out_xinst Pointer to receive the created instance.
 * @return XRT_SUCCESS on success, XRT_ERROR_INVALID_ARGUMENT if @p icci or its socket_filename is missing.
 *
 * @see ipc_design
 * @ingroup ipc_client
 */
xrt_result_t
ipc_instance_create(const struct ipc_client_connection_info *icci,
                    const struct xrt_instance_info *i_info,
                    struct xrt_instance **out_xinst);


#ifdef __cplusplus
}
#endif
