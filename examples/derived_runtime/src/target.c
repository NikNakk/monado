// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0

#include "util/u_trace_marker.h"

U_TRACE_TARGET_SETUP(U_TRACE_WHICH_OPENXR)

#include "xrt/xrt_instance.h"
#include "client/ipc_client_interface.h"

xrt_result_t
xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst)
{
	u_trace_marker_init();

	XRT_TRACE_MARKER();

	struct ipc_client_connection_info icci = {
	    .socket_filename = "derived_comp_ipc",
	};

	return ipc_instance_create(&icci, ii, out_xinst);
}
