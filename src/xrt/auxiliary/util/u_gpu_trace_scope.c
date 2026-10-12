// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace.h"
#include <stdatomic.h>

uint64_t
u_gpu_trace_new_scope(void)
{
	static _Atomic uint64_t next = 1;
	return atomic_fetch_add_explicit(&next, 1, memory_order_relaxed);
}
