// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>
struct Command
{
	uint64_t frame, value;
	uint32_t image, producer, mode, delay_ms;
};
struct Receipt
{
	uint64_t commit_ns;
	uint64_t gpu_status;
};
