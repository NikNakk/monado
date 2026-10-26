// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helpers shared between the IPC server handle functions.
 * @author Jakob Bornecrantz <tbornecrantz@nvidia.com>
 * @ingroup ipc_server
 */

#pragma once

#include "util/u_misc.h"

#include "server/ipc_server.h"


#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * IO blocking.
 *
 */

/*!
 * Is the given input blocked for this client by its @ref ipc_client_io_blocks.
 *
 * @ingroup ipc_server
 */
static inline bool
ipc_server_client_is_input_blocked(volatile struct ipc_client_state *ics, enum xrt_input_name name)
{
	volatile struct ipc_client_io_blocks *iob = &ics->client_state.io_blocks;

	switch (XRT_GET_INPUT_TYPE(name)) {
	case XRT_INPUT_TYPE_POSE: return iob->block_poses && name != XRT_INPUT_GENERIC_HEAD_POSE;
	case XRT_INPUT_TYPE_HAND_TRACKING: return iob->block_hand_tracking;
	case XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE:
	case XRT_INPUT_TYPE_VEC1_MINUS_ONE_TO_ONE:
	case XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE:
	case XRT_INPUT_TYPE_VEC3_MINUS_ONE_TO_ONE:
	case XRT_INPUT_TYPE_BOOLEAN: return iob->block_inputs;
	// Don't block face or body tracking.
	case XRT_INPUT_TYPE_BODY_TRACKING:
	case XRT_INPUT_TYPE_FACE_TRACKING: return false;
	}

	return false;
}

/*!
 * Zero out the inputs that are blocked for this client, in place.
 *
 * @ingroup ipc_server
 */
static inline void
ipc_server_client_filter_blocked_inputs(volatile struct ipc_client_state *ics,
                                        struct xrt_input *inputs,
                                        uint32_t input_count)
{
	for (uint32_t i = 0; i < input_count; i++) {
		enum xrt_input_name name = inputs[i].name;
		if (!ipc_server_client_is_input_blocked(ics, name)) {
			continue;
		}

		// Keep the name, clients find inputs by name.
		U_ZERO(&inputs[i]);
		inputs[i].name = name;
	}
}

/*!
 * Zero out the outputs if outputs are blocked for this client, in place.
 *
 * @ingroup ipc_server
 */
static inline void
ipc_server_client_filter_blocked_outputs(volatile struct ipc_client_state *ics,
                                         struct xrt_output *outputs,
                                         uint32_t output_count)
{
	if (!ics->client_state.io_blocks.block_outputs) {
		return;
	}

	for (uint32_t i = 0; i < output_count; i++) {
		enum xrt_output_name name = outputs[i].name;

		// Keep the name, clients find outputs by name.
		U_ZERO(&outputs[i]);
		outputs[i].name = name;
	}
}

#ifdef __cplusplus
}
#endif
