// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sharing the latest frames of a few streams through shared memory.
 * @ingroup aux_util
 */

#include "util/u_frame_share.h"
#include "util/u_format.h"
#include "util/u_frame.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define U_FRAME_SHARE_MAGIC 0x53465255u // "URFS"
#define U_FRAME_SHARE_VERSION 1u
#define U_FRAME_SHARE_ALIGN 64u

struct u_frame_share_stream
{
	//! Sequence number of the newest complete frame, 0 for none.
	_Atomic uint64_t latest;
	uint64_t pad[7];
};

struct u_frame_share_slot
{
	//! The frame's sequence number once complete, 0 while being written.
	_Atomic uint64_t sequence;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint64_t size;
	int64_t timestamp;
	int64_t source_timestamp;
	uint64_t pad[2];
};

struct u_frame_share_header
{
	uint32_t magic;
	uint32_t version;
	uint32_t stream_count;
	uint32_t slot_count;
	uint64_t max_frame_size;
	uint64_t data_offset;
	uint64_t total_size;
	uint64_t pad[3];

	struct u_frame_share_stream streams[U_FRAME_SHARE_MAX_STREAMS];
	struct u_frame_share_slot slots[U_FRAME_SHARE_MAX_STREAMS][U_FRAME_SHARE_SLOTS];
};

static size_t
align_up(size_t v)
{
	return (v + U_FRAME_SHARE_ALIGN - 1) & ~(size_t)(U_FRAME_SHARE_ALIGN - 1);
}

static uint8_t *
slot_data(struct u_frame_share_header *h, uint32_t stream, uint32_t slot)
{
	size_t index = (size_t)stream * U_FRAME_SHARE_SLOTS + slot;
	return (uint8_t *)h + h->data_offset + index * h->max_frame_size;
}

size_t
u_frame_share_size(uint32_t stream_count, size_t max_frame_size)
{
	if (stream_count == 0 || stream_count > U_FRAME_SHARE_MAX_STREAMS || max_frame_size == 0) {
		return 0;
	}
	return align_up(sizeof(struct u_frame_share_header)) +
	       (size_t)stream_count * U_FRAME_SHARE_SLOTS * align_up(max_frame_size);
}

bool
u_frame_share_init(void *mem, size_t mem_size, uint32_t stream_count, size_t max_frame_size)
{
	size_t needed = u_frame_share_size(stream_count, max_frame_size);
	if (mem == NULL || needed == 0 || mem_size < needed) {
		return false;
	}

	memset(mem, 0, sizeof(struct u_frame_share_header));
	struct u_frame_share_header *h = mem;
	h->stream_count = stream_count;
	h->slot_count = U_FRAME_SHARE_SLOTS;
	h->max_frame_size = align_up(max_frame_size);
	h->data_offset = align_up(sizeof(struct u_frame_share_header));
	h->total_size = needed;
	h->version = U_FRAME_SHARE_VERSION;

	// Publish the layout last.
	atomic_thread_fence(memory_order_release);
	h->magic = U_FRAME_SHARE_MAGIC;
	return true;
}

bool
u_frame_share_is_valid(const void *mem, size_t mem_size)
{
	const struct u_frame_share_header *h = mem;
	if (mem == NULL || mem_size < sizeof(*h) || h->magic != U_FRAME_SHARE_MAGIC ||
	    h->version != U_FRAME_SHARE_VERSION) {
		return false;
	}
	atomic_thread_fence(memory_order_acquire);
	return h->stream_count > 0 && h->stream_count <= U_FRAME_SHARE_MAX_STREAMS &&
	       h->slot_count == U_FRAME_SHARE_SLOTS && h->total_size <= mem_size && h->data_offset >= sizeof(*h) &&
	       h->data_offset + (uint64_t)h->stream_count * U_FRAME_SHARE_SLOTS * h->max_frame_size <= h->total_size;
}

bool
u_frame_share_write(void *mem, uint32_t stream, const struct xrt_frame *frame)
{
	struct u_frame_share_header *h = mem;
	if (h == NULL || frame == NULL || frame->data == NULL || stream >= h->stream_count ||
	    !u_format_is_blocks(frame->format) || frame->width == 0 || frame->height == 0) {
		return false;
	}

	// Pack to the format's natural layout; the source may be a region of a larger frame.
	size_t stride = 0;
	size_t size = 0;
	u_format_size_for_dimensions(frame->format, frame->width, frame->height, &stride, &size);
	if (stride == 0 || size == 0 || size > h->max_frame_size || stride > frame->stride) {
		return false;
	}

	uint64_t sequence = atomic_load_explicit(&h->streams[stream].latest, memory_order_relaxed) + 1;
	uint32_t slot_index = (uint32_t)(sequence % U_FRAME_SHARE_SLOTS);
	struct u_frame_share_slot *slot = &h->slots[stream][slot_index];

	// Mark the slot as being written before touching its contents.
	atomic_store_explicit(&slot->sequence, 0, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);

	slot->format = (uint32_t)frame->format;
	slot->width = frame->width;
	slot->height = frame->height;
	slot->stride = (uint32_t)stride;
	slot->size = size;
	slot->timestamp = (int64_t)frame->timestamp;
	slot->source_timestamp = (int64_t)frame->source_timestamp;

	uint8_t *dst = slot_data(h, stream, slot_index);
	size_t rows = size / stride;
	for (size_t row = 0; row < rows; row++) {
		memcpy(dst + row * stride, frame->data + row * frame->stride, stride);
	}

	atomic_store_explicit(&slot->sequence, sequence, memory_order_release);
	atomic_store_explicit(&h->streams[stream].latest, sequence, memory_order_release);
	return true;
}

bool
u_frame_share_read(const void *mem, uint32_t stream, uint64_t *inout_sequence, struct xrt_frame **out_frame)
{
	struct u_frame_share_header *h = (struct u_frame_share_header *)mem;
	if (h == NULL || inout_sequence == NULL || out_frame == NULL || stream >= h->stream_count) {
		return false;
	}

	uint64_t latest = atomic_load_explicit(&h->streams[stream].latest, memory_order_acquire);
	if (latest == 0 || latest == *inout_sequence) {
		return false;
	}

	uint32_t slot_index = (uint32_t)(latest % U_FRAME_SHARE_SLOTS);
	struct u_frame_share_slot *slot = &h->slots[stream][slot_index];
	if (atomic_load_explicit(&slot->sequence, memory_order_acquire) != latest) {
		return false;
	}

	enum xrt_format format = (enum xrt_format)slot->format;
	uint32_t width = slot->width;
	uint32_t height = slot->height;
	uint64_t size = slot->size;
	uint64_t timestamp = (uint64_t)slot->timestamp;
	uint64_t source_timestamp = (uint64_t)slot->source_timestamp;

	// Validate before allocating: the fields may be torn if the slot is being rewritten.
	size_t expected_stride = 0;
	size_t expected_size = 0;
	if (!u_format_is_blocks(format) || width == 0 || height == 0) {
		return false;
	}
	u_format_size_for_dimensions(format, width, height, &expected_stride, &expected_size);
	if (expected_size != size || size > h->max_frame_size) {
		return false;
	}

	struct xrt_frame *xf = NULL;
	u_frame_create_one_off(format, width, height, &xf);
	if (xf == NULL || xf->data == NULL || xf->size != size) {
		xrt_frame_reference(&xf, NULL);
		return false;
	}
	memcpy(xf->data, slot_data(h, stream, slot_index), size);

	// Discard the copy if the writer started on this slot meanwhile.
	atomic_thread_fence(memory_order_acquire);
	if (atomic_load_explicit(&slot->sequence, memory_order_relaxed) != latest) {
		xrt_frame_reference(&xf, NULL);
		return false;
	}

	xf->timestamp = timestamp;
	xf->source_timestamp = source_timestamp;
	*inout_sequence = latest;
	*out_frame = xf;
	return true;
}


/*
 *
 * Process-wide passthrough share.
 *
 */

static atomic_bool g_passthrough_source_available;
static pthread_mutex_t g_passthrough_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *g_passthrough_target;
static atomic_bool g_passthrough_has_target;

void
u_passthrough_share_set_source_available(bool available)
{
	atomic_store(&g_passthrough_source_available, available);
}

bool
u_passthrough_share_source_available(void)
{
	return atomic_load(&g_passthrough_source_available);
}

void
u_passthrough_share_set_target(void *mem)
{
	pthread_mutex_lock(&g_passthrough_mutex);
	g_passthrough_target = mem;
	atomic_store(&g_passthrough_has_target, mem != NULL);
	pthread_mutex_unlock(&g_passthrough_mutex);
}

void
u_passthrough_share_push(uint32_t eye, const struct xrt_frame *frame)
{
	if (!atomic_load_explicit(&g_passthrough_has_target, memory_order_relaxed)) {
		return;
	}

	// The lock keeps the target mapped while writing; one writer per eye.
	pthread_mutex_lock(&g_passthrough_mutex);
	if (g_passthrough_target != NULL) {
		(void)u_frame_share_write(g_passthrough_target, eye, frame);
	}
	pthread_mutex_unlock(&g_passthrough_mutex);
}
