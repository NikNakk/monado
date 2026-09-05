// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"
#include <stdint.h>

/* Native diagnostic format: replay rejects incompatible layouts/endianness. */
#define PSVR2_CAPTURE_MAGIC "PSV2PR01"
#define PSVR2_CAPTURE_MAX_SAMPLES 1024
#define PSVR2_CAPTURE_MAX_RECORDS 4096

struct psvr2_capture_header
{
	char magic[8];
	uint32_t endian;
	uint32_t relation_size;
	uint32_t record_size;
	uint32_t sample_size;
};

struct psvr2_capture_sample
{
	uint64_t timestamp_ns;
	struct xrt_vec3 gyro;
};

struct psvr2_capture_record
{
	int64_t query_ns;
	int64_t requested_ns;
	int64_t target_ns;
	int64_t base_ns;
	uint32_t sample_count;
	uint32_t explicit_integration;
	struct xrt_space_relation base;
	struct xrt_space_relation result;
};
