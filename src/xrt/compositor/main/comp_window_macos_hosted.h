// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
struct u_macos_hosted_client;
struct comp_target_factory;
//! The returned factory must outlive its system compositor. NULL on allocation failure.
struct comp_target_factory *
comp_window_macos_hosted_factory_create(struct u_macos_hosted_client *client);
void
comp_window_macos_hosted_factory_destroy(struct comp_target_factory *factory);
#ifdef __cplusplus
}
#endif
