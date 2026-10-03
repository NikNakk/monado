// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "ipc_client_tracking_share.h"
#include "ipc_client_xdev.h"
#include "ipc_client.h"
#include "ipc_client_generated.h"
#include "shared/ipc_shmem.h"
#include "../../drivers/psvr2/psvr2_tracking_share.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_timing_trace.h"
#include <stdlib.h>
#include <inttypes.h>

DEBUG_GET_ONCE_BOOL_OPTION(macos_shared_tracking, "XRT_MACOS_SHARED_TRACKING", false)

struct ipc_client_tracking_share
{
	struct os_mutex lock;
	xrt_shmem_handle_t handle;
	void *mem;
	struct psvr2_tracking_snapshot cached, scratch;
	struct m_ff_vec3_f32 *gyro;
	uint64_t sequence;
	FILE *trace;
	uint64_t queries, rpc_fallbacks, read_misses;
};

void
ipc_client_tracking_share_create(struct ipc_client_xdev *icx)
{
	if (!debug_get_bool_option_macos_shared_tracking() || icx->tracking_share)
		return;
	if (icx->base.name != XRT_DEVICE_PSVR2) {
		IPC_WARN(icx->ipc_c, "Shared tracking requires a PS VR2; retaining pose IPC");
		return;
	}
	struct ipc_client_tracking_share *t = U_TYPED_CALLOC(struct ipc_client_tracking_share);
	if (!t)
		return;
	t->handle = XRT_SHMEM_HANDLE_INVALID;
	if (os_mutex_init(&t->lock) != 0) {
		free(t);
		return;
	}
	uint64_t size = 0;
	xrt_result_t ret =
	    ipc_call_device_tracking_share_get(icx->ipc_c, icx->device_id, PSVR2_TRACKING_SHARE_VERSION,
	                                       sizeof(struct psvr2_tracking_share), &size, &t->handle, 1);
	if (ret != XRT_SUCCESS || size != sizeof(struct psvr2_tracking_share)) {
		IPC_WARN(icx->ipc_c, "Shared tracking unavailable (%d, size %" PRIu64 "); retaining pose IPC", ret,
		         size);
		ipc_shmem_destroy(&t->handle, &t->mem, sizeof(struct psvr2_tracking_share));
		os_mutex_destroy(&t->lock);
		free(t);
		return;
	}
	ret = ipc_shmem_map_readonly(t->handle, size, &t->mem);
	if (ret != XRT_SUCCESS) {
		ipc_shmem_destroy(&t->handle, &t->mem, size);
		os_mutex_destroy(&t->lock);
		free(t);
		return;
	}
	m_ff_vec3_f32_alloc(&t->gyro, PSVR2_TRACKING_GYRO_CAPACITY);
	if (!t->gyro) {
		ipc_client_tracking_share_destroy(&t);
		return;
	}
	if (u_timing_trace_enabled()) {
		t->trace = u_timing_trace_open("shared_tracking", 256 * 1024);
		if (t->trace)
			fputs(
			    "query_ns,target_ns,end_ns,sequence,published_ns,slam_host_ns,imu_host_ns,gyro_count,flags,"
			    "rpc_fallback,read_miss,slam_vts_ns,imu_vts_ns,hw2mono_vts_ns,imu_callback_ns,imu_"
			    "estimated_sample_ns,slam_received_ns,qw,qx,qy,qz,pos_x,pos_y,pos_z,angvel_x,angvel_y,"
			    "angvel_z\n",
			    t->trace);
	}
	icx->tracking_share = t;
	IPC_INFO(icx->ipc_c, "PS VR2 shared tracking active: read-only snapshot, client-local prediction");
}

void
ipc_client_tracking_share_destroy(struct ipc_client_tracking_share **ptr)
{
	struct ipc_client_tracking_share *t = *ptr;
	if (!t)
		return;
	// Device destruction occurs after the compositor/app's device users stop.
	if (t->trace)
		fclose(t->trace);
	m_ff_vec3_f32_free(&t->gyro);
	ipc_shmem_destroy(&t->handle, &t->mem, sizeof(struct psvr2_tracking_share));
	os_mutex_destroy(&t->lock);
	free(t);
	*ptr = NULL;
}

bool
ipc_client_tracking_share_pose(struct ipc_client_xdev *icx,
                               int64_t timestamp_ns,
                               struct xrt_space_relation *out_relation,
                               float *out_ipd_m)
{
	struct ipc_client_tracking_share *t = icx->tracking_share;
	if (!t)
		return false;
	uint64_t query_ns = os_monotonic_get_ns();
	os_mutex_lock(&t->lock);
	uint64_t sequence = 0;
	bool got = psvr2_tracking_share_read(t->mem, &t->scratch, &sequence);
	if (got && sequence != t->sequence) {
		psvr2_tracking_snapshot_fill_gyro(&t->scratch, t->gyro);
		t->cached = t->scratch;
		t->sequence = sequence;
	}
	const struct psvr2_tracking_state *s = &t->cached.state;
	int64_t target_vts = timestamp_ns - s->hw2mono_vts;
	// Keep history and the driver's first-pose recenter semantics. Calibration
	// startup also uses RPC until the driver has a calibrated valid snapshot.
	bool fallback = t->sequence == 0 || !s->ready || target_vts <= s->slam_ns ||
	                (s->recenter_on_first_pose && !s->recenter_initialized);
	if (!fallback) {
		psvr2_tracking_predict_head(s, t->gyro, target_vts, query_ns, out_relation);
		*out_ipd_m = s->ipd_m;
	}
	++t->queries;
	t->rpc_fallbacks += fallback;
	t->read_misses += !got;
	if (t->trace) {
		int64_t imu_ns = t->cached.gyro_count ? (int64_t)t->cached.gyro[0].timestamp_ns + s->hw2mono_vts : 0;
		struct xrt_space_relation pose;
		memset(&pose, 0, sizeof(pose));
		if (!fallback)
			pose = *out_relation;
		fprintf(t->trace,
		        "%" PRIu64 ",%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64
		        ",%u,%u,%u,%u,%" PRId64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
		        ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
		        query_ns, timestamp_ns, os_monotonic_get_ns(), t->sequence, s->published_ns,
		        s->slam_ns + s->hw2mono_vts, imu_ns, t->cached.gyro_count,
		        fallback ? 0 : (unsigned)out_relation->relation_flags, fallback, !got, s->slam_ns,
		        t->cached.gyro_count ? t->cached.gyro[0].timestamp_ns : 0, s->hw2mono_vts, s->imu_received_ns,
		        s->imu_estimated_ns, s->slam_received_ns, pose.pose.orientation.w, pose.pose.orientation.x,
		        pose.pose.orientation.y, pose.pose.orientation.z, pose.pose.position.x, pose.pose.position.y,
		        pose.pose.position.z, pose.angular_velocity.x, pose.angular_velocity.y,
		        pose.angular_velocity.z);
		if (t->queries % 256 == 0 && !u_timing_trace_fully_buffered())
			fflush(t->trace);
	}
	os_mutex_unlock(&t->lock);
	return !fallback;
}
