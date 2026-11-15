// Copyright 2020, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main file for Monado service.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc
 */

#include "xrt/xrt_config_os.h"

#include "util/u_debug.h"
#include "util/u_metrics.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#endif

#include <stdlib.h>

#include "server/ipc_server_interface.h"

#include "target_lists.h"

DEBUG_GET_ONCE_BOOL_OPTION(exit_on_disconnect, "IPC_EXIT_ON_DISCONNECT", false)


// Insert the on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)


int
main(int argc, char *argv[])
{
#ifdef XRT_OS_WINDOWS
	u_win_try_privilege_or_priority_from_args(U_LOGGING_INFO, argc, argv);
#endif

#ifdef XRT_OS_OSX
	/*
	 * Give the compositor's KosmicKrisp GPU queues the highest Metal priority
	 * unless KK_GPU_QUEUE_PRIORITY is already set (-1 keeps the driver
	 * default). Only this process: applications keep their own priority. With
	 * Alyx on the PS VR2 this cut the compositor's GPU-time tail by about two
	 * thirds at no cost to the game's frame rate. MoltenVK ignores it.
	 */
	const char *kk_priority = getenv("KK_GPU_QUEUE_PRIORITY");
	if (kk_priority == NULL || kk_priority[0] == '\0') {
		setenv("KK_GPU_QUEUE_PRIORITY", "0", 1);
	}
#endif

	u_trace_marker_init();
	u_metrics_init();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "Monado! ✨⚡🔥",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	    .exit_on_disconnect = debug_get_bool_option_exit_on_disconnect(),
	};

	int ret = ipc_server_main(argc, argv, &ismi);

	u_metrics_close();

	return ret;
}
