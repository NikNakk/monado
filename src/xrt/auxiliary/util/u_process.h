// Copyright 2020, Collabora, Ltd.
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simple process handling
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_process;

/*!
 * Creates a handle for this process that is unique to the operating system user and @p pid_filename. Returns NULL if
 * another process holding the same pidfile is already running.
 *
 * @todo If built without libbsd support, a placeholder value is returned that needs to be handled by the caller.
 *
 * @param pid_filename Name of the pidfile in the runtime dir, processes using the same name exclude each other.
 *                     Ignored when built without libbsd, must not be NULL otherwise.
 * @return a new u_process handle, or NULL if another process holds the pidfile or the pidfile can't be created.
 * @ingroup aux_util
 */
struct u_process *
u_process_create_if_not_running(const char *pid_filename);

/*!
 * Releases the unique handle of the operating system user.
 *
 * @ingroup aux_util
 */
void
u_process_destroy(struct u_process *proc);

#ifdef __cplusplus
}
#endif
