// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_device.h"
struct ipc_client_xdev;
struct ipc_client_tracking_share;
void
ipc_client_tracking_share_create(struct ipc_client_xdev *icx);
void
ipc_client_tracking_share_destroy(struct ipc_client_tracking_share **ptr);
/* False requests the historical/unsupported RPC path. Once active, unavailable
 * or stale future tracking returns validity loss locally rather than blocking. */
bool
ipc_client_tracking_share_pose(struct ipc_client_xdev *icx,
                               int64_t timestamp_ns,
                               struct xrt_space_relation *out_relation,
                               float *out_ipd_m);
