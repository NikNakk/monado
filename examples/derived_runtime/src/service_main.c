// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0

#include "xrt/xrt_config_os.h"

#include "util/u_debug.h"
#include "util/u_trace_marker.h"

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#endif

#include "server/ipc_server_interface.h"

DEBUG_GET_ONCE_BOOL_OPTION(exit_on_disconnect, "IPC_EXIT_ON_DISCONNECT", false)

U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

int
main(int argc, char *argv[])
{
#ifdef XRT_OS_WINDOWS
	u_win_try_privilege_or_priority_from_args(U_LOGGING_INFO, argc, argv);
#endif

	u_trace_marker_init();

	struct ipc_server_main_info ismi = {
	    .udgci =
	        {
	            .window_title = "Derived Monado service",
	            .open = U_DEBUG_GUI_OPEN_AUTO,
	        },
	    .exit_on_disconnect = debug_get_bool_option_exit_on_disconnect(),
	    .socket_filename = "derived_comp_ipc",
	    .pid_filename = "derived.pid",
	};

	return ipc_server_main(argc, argv, &ismi);
}
