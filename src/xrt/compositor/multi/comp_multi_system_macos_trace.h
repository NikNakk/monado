// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 diagnostic mapping from system compositor frames to client frames.
 *
 * Force-included into comp_multi_system.c on Apple builds only. The wrapper leaves
 * the normal multi-compositor latch behaviour unchanged and records which delivered
 * client frame supplied each system-compositor refresh. Joining system_frame_id to
 * late_render.csv and present.csv/timeline_value lets us verify asynchronous
 * 60 -> 120 Hz reprojection directly rather than infer it from cadence.
 */

#pragma once

/*
 * This header is force-included before comp_multi_system.c's normal include list.
 * Bring in xrt_session.h first so union xrt_session_event has file scope before
 * comp_multi_private.h declares multi_compositor_push_event().
 */
#include "xrt/xrt_session.h"
#include "multi/comp_multi_private.h"
#include "os/os_time.h"
#include "util/u_debug.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DEBUG_GET_ONCE_BOOL_OPTION(macos_client_frame_trace, "PSVR2_TIMING_TRACE", false)

static FILE *g_macos_client_frame_trace = NULL;
static uint64_t g_macos_client_frame_trace_rows = 0;
static bool g_macos_client_frame_trace_failed = false;
static bool g_macos_client_frame_trace_atexit_registered = false;
static bool g_macos_client_frame_trace_have_last[MULTI_MAX_CLIENTS];
static int64_t g_macos_client_frame_trace_last_frame[MULTI_MAX_CLIENTS];
static uint32_t g_macos_client_frame_trace_source_use_ordinal[MULTI_MAX_CLIENTS];

static void
macos_client_frame_trace_close(void)
{
	if (g_macos_client_frame_trace == NULL) {
		return;
	}
	fflush(g_macos_client_frame_trace);
	fclose(g_macos_client_frame_trace);
	g_macos_client_frame_trace = NULL;
}

static FILE *
macos_client_frame_trace_get(void)
{
	if (!debug_get_bool_option_macos_client_frame_trace() || g_macos_client_frame_trace_failed) {
		return NULL;
	}
	if (g_macos_client_frame_trace != NULL) {
		return g_macos_client_frame_trace;
	}

	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	char path[1024];
	size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_client_frame_map.csv", dir, separator, (int)getpid());

	g_macos_client_frame_trace = fopen(path, "w");
	if (g_macos_client_frame_trace == NULL) {
		g_macos_client_frame_trace_failed = true;
		return NULL;
	}

	const char *fully_buffered = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
	if (fully_buffered != NULL && strcmp(fully_buffered, "1") == 0) {
		setvbuf(g_macos_client_frame_trace, NULL, _IOFBF, 16u * 1024u * 1024u);
	} else {
		setvbuf(g_macos_client_frame_trace, NULL, _IOFBF, 64u * 1024u);
	}

	fputs("system_frame_id,system_display_time_ns,latch_ns,client_slot,client_frame_id,"
	      "client_display_time_ns,display_time_delta_ns,reused,source_use_ordinal,layer_count,focused,visible\n",
	      g_macos_client_frame_trace);

	if (!g_macos_client_frame_trace_atexit_registered) {
		atexit(macos_client_frame_trace_close);
		g_macos_client_frame_trace_atexit_registered = true;
	}

	return g_macos_client_frame_trace;
}

static inline void
macos_trace_multi_compositor_latch_frame_locked(struct multi_compositor *mc,
                                                 int64_t when_ns,
                                                 int64_t system_frame_id,
                                                 int64_t system_display_time_ns)
{
	/* Preserve the real compositor behaviour exactly. */
	multi_compositor_latch_frame_locked(mc, when_ns, system_frame_id);

	FILE *file = macos_client_frame_trace_get();
	if (file == NULL || mc == NULL || mc->msc == NULL || !mc->delivered.active) {
		return;
	}

	size_t client_slot = MULTI_MAX_CLIENTS;
	for (size_t i = 0; i < MULTI_MAX_CLIENTS; i++) {
		if (mc->msc->clients[i] == mc) {
			client_slot = i;
			break;
		}
	}
	if (client_slot == MULTI_MAX_CLIENTS) {
		return;
	}

	int64_t client_frame_id = mc->delivered.data.frame_id;
	bool reused = g_macos_client_frame_trace_have_last[client_slot] &&
	              g_macos_client_frame_trace_last_frame[client_slot] == client_frame_id;
	if (reused) {
		if (g_macos_client_frame_trace_source_use_ordinal[client_slot] < UINT32_MAX) {
			g_macos_client_frame_trace_source_use_ordinal[client_slot]++;
		}
	} else {
		g_macos_client_frame_trace_source_use_ordinal[client_slot] = 1;
	}
	g_macos_client_frame_trace_have_last[client_slot] = true;
	g_macos_client_frame_trace_last_frame[client_slot] = client_frame_id;

	int64_t client_display_time_ns = mc->delivered.data.display_time_ns;
	int64_t display_time_delta_ns = system_display_time_ns - client_display_time_ns;

	flockfile(file);
	fprintf(file, "%lld,%lld,%lld,%zu,%lld,%lld,%lld,%u,%u,%u,%u,%u\n",
	        (long long)system_frame_id, (long long)system_display_time_ns, (long long)when_ns, client_slot,
	        (long long)client_frame_id, (long long)client_display_time_ns, (long long)display_time_delta_ns,
	        reused ? 1u : 0u, g_macos_client_frame_trace_source_use_ordinal[client_slot], mc->delivered.layer_count,
	        mc->state.focused ? 1u : 0u, mc->state.visible ? 1u : 0u);
	g_macos_client_frame_trace_rows++;

	const char *fully_buffered = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
	bool defer_flush = fully_buffered != NULL && strcmp(fully_buffered, "1") == 0;
	if (!defer_flush && g_macos_client_frame_trace_rows % 512 == 0) {
		fflush(file);
	}
	funlockfile(file);
}

/*
 * comp_multi_system.c has exactly one latch call, inside transfer_layers_locked,
 * where display_time_ns is the target system refresh timestamp. Capture it without
 * changing the public multi-compositor interface.
 */
#define multi_compositor_latch_frame_locked(mc, when_ns, system_frame_id)                                            \
	macos_trace_multi_compositor_latch_frame_locked((mc), (when_ns), (system_frame_id), display_time_ns)
