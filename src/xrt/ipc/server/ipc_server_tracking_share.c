// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "ipc_server.h"
#include "ipc_server_objects.h"
#include "ipc_server_generated.h"
#include "shared/ipc_shmem.h"
#include "../../drivers/psvr2/psvr2_tracking_share.h"
#ifdef XRT_IPC_PSVR2_TRACKING_SHARE
#include "../../drivers/psvr2/psvr2_interface.h"
#endif

void
ipc_server_tracking_share_init(struct ipc_server *s)
{
	os_mutex_init(&s->tracking_share.lock);
	s->tracking_share.handle = XRT_SHMEM_HANDLE_INVALID;
}
void
ipc_server_tracking_share_fini(struct ipc_server *s)
{
#ifdef XRT_IPC_PSVR2_TRACKING_SHARE
	// Detach waits for the USB producer before its mapping is destroyed.
	if (s->tracking_share.xdev)
		psvr2_set_tracking_share(s->tracking_share.xdev, NULL);
#endif
	ipc_shmem_destroy(&s->tracking_share.handle, &s->tracking_share.mem, sizeof(struct psvr2_tracking_share));
	os_mutex_destroy(&s->tracking_share.lock);
}
void
ipc_server_tracking_share_client_gone(volatile struct ipc_client_state *ics)
{
	struct ipc_server *s = ics->server;
	os_mutex_lock(&s->tracking_share.lock);
	if (ics->shared_tracking_user) {
		ics->shared_tracking_user = false;
		if (--s->tracking_share.users == 0) {
#ifdef XRT_IPC_PSVR2_TRACKING_SHARE
			psvr2_set_tracking_share(s->tracking_share.xdev, NULL);
#endif
			s->tracking_share.xdev = NULL;
			ipc_shmem_destroy(&s->tracking_share.handle, &s->tracking_share.mem,
			                  sizeof(struct psvr2_tracking_share));
		}
	}
	os_mutex_unlock(&s->tracking_share.lock);
}
xrt_result_t
ipc_handle_device_tracking_share_get(volatile struct ipc_client_state *ics,
                                     uint32_t id,
                                     uint32_t version,
                                     uint64_t expected_size,
                                     uint64_t *out_size,
                                     uint32_t max_handle_capacity,
                                     xrt_shmem_handle_t *out_handles,
                                     uint32_t *out_handle_count)
{
	*out_size = 0;
	*out_handle_count = 0;
#ifndef XRT_IPC_PSVR2_TRACKING_SHARE
	(void)ics;
	(void)id;
	(void)version;
	(void)expected_size;
	(void)max_handle_capacity;
	(void)out_handles;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
#else
	if (version != PSVR2_TRACKING_SHARE_VERSION || expected_size != sizeof(struct psvr2_tracking_share))
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	struct xrt_device *xdev = NULL;
	xrt_result_t xret = ipc_server_objects_get_xdev_and_validate(ics, id, &xdev);
	if (xret != XRT_SUCCESS)
		return xret;
	struct ipc_server *s = ics->server;
	if (xdev != s->xsysd->static_roles.head || xdev->name != XRT_DEVICE_PSVR2)
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	if (max_handle_capacity < 1)
		return XRT_ERROR_IPC_FAILURE;
	os_mutex_lock(&s->tracking_share.lock);
	if (s->tracking_share.mem == NULL) {
		xret = ipc_shmem_create_private_readonly("tracking", sizeof(struct psvr2_tracking_share),
		                                         &s->tracking_share.handle, &s->tracking_share.mem);
		if (xret == XRT_SUCCESS) {
			if (psvr2_set_tracking_share(xdev, s->tracking_share.mem)) {
				s->tracking_share.xdev = xdev;
				IPC_INFO(s, "PS VR2 shared tracking producer attached at USB ingestion");
			} else {
				ipc_shmem_destroy(&s->tracking_share.handle, &s->tracking_share.mem,
				                  sizeof(struct psvr2_tracking_share));
				xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
			}
		}
	}
	if (xret == XRT_SUCCESS) {
		if (!ics->shared_tracking_user) {
			ics->shared_tracking_user = true;
			++s->tracking_share.users;
		}
		out_handles[0] = s->tracking_share.handle;
		*out_handle_count = 1;
		*out_size = sizeof(struct psvr2_tracking_share);
	}
	os_mutex_unlock(&s->tracking_share.lock);
	return xret;
#endif
}
