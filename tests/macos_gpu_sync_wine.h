// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "macos_gpu_sync_protocol.h"
enum
{
	WINE_GPU_INIT,
	WINE_GPU_NEXT,
	WINE_GPU_SENT,
	WINE_GPU_ACK,
	WINE_GPU_JOIN,
	WINE_GPU_BROKEN_SIGNAL,
	WINE_GPU_COMPLETE
};
struct WineGPUParams
{
	uint64_t device, textures[3], event, gate, join;
	uint32_t width;
	char service[160], socket[160];
	struct Command command;
};

#ifdef __cplusplus
static_assert(sizeof(Command) == 32 && sizeof(Receipt) == 16);
static_assert(sizeof(WineGPUParams) == 416, "PE/unixlib parameter ABI");
#endif
