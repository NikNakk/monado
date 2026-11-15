// Copyright 2019-2021, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Very simple file opening functions, mostly using std::filesystem for portability.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"
#include "util/u_file.h"
#include "util/u_logging.h"

#ifndef XRT_OS_LINUX

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
namespace fs = std::filesystem;


/*
 *
 * Helpers
 *
 */

#ifdef XRT_OS_OSX
static inline errno_t
fopen_s(FILE **f, const char *name, const char *mode)
{
	FILE *file = nullptr;
	errno_t ret = 0;

	assert(f);

	file = fopen(name, mode);

	if (file == nullptr) {
		ret = errno;
	}

	*f = file;

	return ret;
}
#endif // XRT_OS_OSX

static inline fs::path
get_config_path()
{
#ifdef XRT_OS_WINDOWS
	char *buffer = nullptr;
	errno_t ret = _dupenv_s(&buffer, nullptr, "LOCALAPPDATA");
	if (ret != 0 || buffer == nullptr) {
		return {};
	}

	auto local_app_data = fs::path{buffer};
	free(buffer);

	return local_app_data / "monado";
#else
	const char *xdg_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg_home != NULL) {
		return fs::path(xdg_home) / "monado";
	}
	if (home != NULL) {
		return fs::path(home) / "monado";
	}
	return {};
#endif
}


/*
 *
 * 'Exported' functions.
 *
 */

int
u_file_get_config_dir(char *out_path, size_t out_path_size)
{
	auto config_path = get_config_path();
	if (config_path.empty()) {
		return -1;
	}
	auto config_path_string = config_path.string();
	return snprintf(out_path, out_path_size, "%s", config_path_string.c_str());
}

int
u_file_get_path_in_config_dir(const char *filename, char *out_path, size_t out_path_size)
{
	auto config_path = get_config_path();
	if (config_path.empty()) {
		return -1;
	}
	auto path_string = (config_path / filename).string();
	return snprintf(out_path, out_path_size, "%s", path_string.c_str());
}

FILE *
u_file_open_file_in_config_dir(const char *filename, const char *mode)
{
	auto config_path = get_config_path();
	if (config_path.empty()) {
		return NULL;
	}

	auto file_path_string = (config_path / filename).string();
	FILE *file = nullptr;
	errno_t ret = fopen_s(&file, file_path_string.c_str(), mode);
	if (ret == 0) {
		return file;
	}

	// Try creating the path, using error_code so it doesn't throw into C callers.
	auto directory = (config_path / filename).parent_path();
	std::error_code ec;
	fs::create_directories(directory, ec);
	if (ec) {
		U_LOG_E("Failed to create '%s': %s", directory.string().c_str(), ec.message().c_str());
	}

	// Do not report open error.
	ret = fopen_s(&file, file_path_string.c_str(), mode);
	if (ret == 0) {
		return file;
	}

	return nullptr;
}

FILE *
u_file_open_file_in_config_dir_subpath(const char *subpath, const char *filename, const char *mode)
{
	auto path_string = (fs::path(subpath) / filename).string();
	return u_file_open_file_in_config_dir(path_string.c_str(), mode);
}

#endif // !XRT_OS_LINUX
