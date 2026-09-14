// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Route monado-service Metal transport calls to its in-process registry.
 * @ingroup ipc_server
 */

#pragma once

#include "shared/ipc_metal_xpc_service.h"

/*
 * ipc_server_metal.c runs inside monado-service itself. Once the service hosts
 * the Metal XPC registry there is no reason for it to open an XPC connection
 * back to its own Mach service. Keep the existing server implementation and
 * substitute only the resource-registry operations.
 */
#define ipc_metal_xpc_take_textures ipc_metal_xpc_service_take_textures
#define ipc_metal_xpc_publish_shared_event ipc_metal_xpc_service_publish_shared_event
#define ipc_metal_xpc_discard_token ipc_metal_xpc_service_discard_token
