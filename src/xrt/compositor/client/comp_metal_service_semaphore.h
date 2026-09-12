// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Service-mode Metal shared-event semaphore pair creation.
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the Metal client compositor and its application-side MTLDevice. */
void
client_metal_service_semaphore_register_compositor(struct xrt_compositor *xc, void *metal_device);

/*
 * Service-mode replacement for comp_metal_semaphore_create_client_pair().
 * The compositor semaphore lives in monado-service while the returned
 * MTLSharedEvent is reconstructed on the application's MTLDevice through XPC.
 */
xrt_result_t
client_metal_service_semaphore_create_pair(struct xrt_compositor_semaphore **out_xcsem,
                                           void **out_mtl_shared_event);

#ifdef __cplusplus
}
#endif
