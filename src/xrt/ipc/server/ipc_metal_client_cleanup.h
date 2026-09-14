// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS service cleanup hook for per-client IPC teardown.
 * @ingroup ipc_server
 */

#pragma once

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void
ipc_metal_client_message_channel_close(struct ipc_message_channel *imc);

#ifdef __cplusplus
}
#endif

/*
 * common_shutdown() still owns normal socket teardown. In a macOS service
 * build, intercept only that close call so we can release any unconsumed Metal
 * XPC registry entries while the ipc_client_state still contains the PID.
 */
#define ipc_message_channel_close(IMC) ipc_metal_client_message_channel_close((IMC))
