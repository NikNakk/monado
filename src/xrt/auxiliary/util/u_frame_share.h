// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sharing the latest frames of a few streams through shared memory.
 *
 * A writer in one process publishes frames; readers in other processes pick
 * up the newest one of each stream. Each stream has three slots, so a reader
 * copying the newest frame is not overwritten unless it is two frames behind,
 * and a per-slot sequence number detects that case. There are no locks, only
 * atomics in the shared memory, so a stalled reader never blocks the writer.
 *
 * Used to give clients that composite in-process the service's passthrough
 * camera frames. See doc/macos-client-compositor-design.md.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Streams in a share, for example one per eye.
#define U_FRAME_SHARE_MAX_STREAMS 2

//! Slots per stream.
#define U_FRAME_SHARE_SLOTS 3

/*!
 * Total shared memory needed for @p stream_count streams of frames up to
 * @p max_frame_size bytes each.
 */
size_t
u_frame_share_size(uint32_t stream_count, size_t max_frame_size);

/*!
 * Lay out a share in @p mem, which is @p mem_size bytes as returned by
 * u_frame_share_size(). Called once, by the writer, before anyone reads.
 */
bool
u_frame_share_init(void *mem, size_t mem_size, uint32_t stream_count, size_t max_frame_size);

/*!
 * True if @p mem holds a share laid out by u_frame_share_init() that fits in
 * @p mem_size bytes. Readers check this before anything else.
 */
bool
u_frame_share_is_valid(const void *mem, size_t mem_size);

/*!
 * Publish @p frame as the newest of @p stream. The pixels are copied, row by
 * row, packed to the format's natural stride. Frames larger than the share's
 * frame size, and formats without a known layout, are dropped.
 *
 * Only one thread may write to a given stream.
 *
 * @return true if published.
 */
bool
u_frame_share_write(void *mem, uint32_t stream, const struct xrt_frame *frame);

/*!
 * If @p stream has a frame newer than @p *inout_sequence, copy it into a new
 * frame and update @p *inout_sequence. Start with a sequence of 0.
 *
 * @return true and a new frame in @p out_frame, or false if there is nothing
 *         new or the writer overwrote the slot while it was being copied (try
 *         again later).
 */
bool
u_frame_share_read(const void *mem, uint32_t stream, uint64_t *inout_sequence, struct xrt_frame **out_frame);


/*
 *
 * Process-wide passthrough share: in the service, the compositor's camera
 * sink publishes into the share that the IPC server created for clients,
 * without either linking against the other.
 *
 */

//! The compositor has camera frames to share (its sinks are attached).
void
u_passthrough_share_set_source_available(bool available);

bool
u_passthrough_share_source_available(void);

/*!
 * The IPC server's share, or NULL to stop publishing. The memory must stay
 * mapped until this is called again with NULL.
 */
void
u_passthrough_share_set_target(void *mem);

/*!
 * Called by the compositor's camera sink for each frame of @p eye. Cheap when
 * no client has asked for frames.
 */
void
u_passthrough_share_push(uint32_t eye, const struct xrt_frame *frame);


#ifdef __cplusplus
}
#endif
