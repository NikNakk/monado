// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Passthrough camera frames from the service, for clients that
 *         composite in-process.
 *
 * The service's compositor publishes each camera frame into a u_frame_share
 * in shared memory. Here a thread picks up the newest frame of each eye and
 * pushes it into this process's compositor, as the driver would in-process.
 *
 * @ingroup ipc_client
 */

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_frame_share.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "shared/ipc_shmem.h"
#include "client/ipc_client.h"
#include "client/ipc_client_passthrough.h"

#include "ipc_client_generated.h"

//! How often to look for new frames; the cameras run far slower than this.
#define POLL_INTERVAL_NS (2 * 1000 * 1000)

struct ipc_client_passthrough
{
	struct ipc_connection *ipc_c;
	uint32_t device_id;

	struct os_thread_helper oth;

	//! Protects the sinks, which the reader thread snapshots per frame.
	struct os_mutex sink_lock;
	struct xrt_frame_sink *sinks[2];

	xrt_shmem_handle_t handle;
	void *mem;
	size_t size;
};

static void *
reader_thread(void *ptr)
{
	struct ipc_client_passthrough *icp = ptr;
	uint64_t sequences[2] = {0, 0};

	os_thread_helper_name(&icp->oth, "IPC passthrough");

	os_thread_helper_lock(&icp->oth);
	while (os_thread_helper_is_running_locked(&icp->oth)) {
		os_thread_helper_unlock(&icp->oth);

		for (uint32_t eye = 0; eye < 2; eye++) {
			struct xrt_frame *xf = NULL;
			if (!u_frame_share_read(icp->mem, eye, &sequences[eye], &xf)) {
				continue;
			}

			os_mutex_lock(&icp->sink_lock);
			struct xrt_frame_sink *sink = icp->sinks[eye];
			if (sink != NULL) {
				xrt_sink_push_frame(sink, xf);
			}
			os_mutex_unlock(&icp->sink_lock);

			xrt_frame_reference(&xf, NULL);
		}

		os_nanosleep(POLL_INTERVAL_NS);
		os_thread_helper_lock(&icp->oth);
	}
	os_thread_helper_unlock(&icp->oth);

	return NULL;
}

static xrt_result_t
map_share(struct ipc_client_passthrough *icp)
{
	xrt_shmem_handle_t handle = XRT_SHMEM_HANDLE_INVALID;
	uint64_t size = 0;
	xrt_result_t xret = ipc_call_device_passthrough_share_get(icp->ipc_c, icp->device_id, &size, &handle, 1);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	void *mem = NULL;
	xret = ipc_shmem_map(handle, (size_t)size, &mem);
	if (xret != XRT_SUCCESS || !u_frame_share_is_valid(mem, (size_t)size)) {
		ipc_shmem_destroy(&handle, &mem, (size_t)size);
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_IPC_FAILURE;
	}

	icp->handle = handle;
	icp->mem = mem;
	icp->size = (size_t)size;
	return XRT_SUCCESS;
}

struct ipc_client_passthrough *
ipc_client_passthrough_create(struct ipc_connection *ipc_c, uint32_t device_id)
{
	struct ipc_client_passthrough *icp = U_TYPED_CALLOC(struct ipc_client_passthrough);
	if (icp == NULL) {
		return NULL;
	}
	icp->ipc_c = ipc_c;
	icp->device_id = device_id;
	icp->handle = XRT_SHMEM_HANDLE_INVALID;

	if (os_thread_helper_init(&icp->oth) != 0 || os_mutex_init(&icp->sink_lock) != 0) {
		free(icp);
		return NULL;
	}
	return icp;
}

xrt_result_t
ipc_client_passthrough_set_sinks(struct ipc_client_passthrough *icp,
                                 struct xrt_frame_sink *left,
                                 struct xrt_frame_sink *right)
{
	if (icp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// Stop reading, but keep the mapping, when both sinks go away.
	if (left == NULL && right == NULL) {
		os_thread_helper_stop_and_wait(&icp->oth);
		os_mutex_lock(&icp->sink_lock);
		icp->sinks[0] = NULL;
		icp->sinks[1] = NULL;
		os_mutex_unlock(&icp->sink_lock);
		return XRT_SUCCESS;
	}

	if (icp->mem == NULL) {
		xrt_result_t xret = map_share(icp);
		if (xret != XRT_SUCCESS) {
			U_LOG_W("The service cannot share passthrough camera frames (%d)", xret);
			return xret;
		}
	}

	os_mutex_lock(&icp->sink_lock);
	icp->sinks[0] = left;
	icp->sinks[1] = right;
	os_mutex_unlock(&icp->sink_lock);

	if (!os_thread_helper_is_running(&icp->oth)) {
		if (os_thread_helper_start(&icp->oth, reader_thread, icp) != 0) {
			return XRT_ERROR_THREADING_INIT_FAILURE;
		}
		U_LOG_I("Receiving passthrough camera frames from the service");
	}
	return XRT_SUCCESS;
}

void
ipc_client_passthrough_destroy(struct ipc_client_passthrough **icp_ptr)
{
	struct ipc_client_passthrough *icp = *icp_ptr;
	if (icp == NULL) {
		return;
	}

	os_thread_helper_destroy(&icp->oth);
	os_mutex_destroy(&icp->sink_lock);
	if (icp->mem != NULL) {
		ipc_shmem_destroy(&icp->handle, &icp->mem, icp->size);
	}
	free(icp);
	*icp_ptr = NULL;
}
