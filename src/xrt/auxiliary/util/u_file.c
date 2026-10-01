// Copyright 2019-2025, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Very simple file opening functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_windows.h"
#include "util/u_file.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_OSX
// For PATH_MAX
#include <sys/syslimits.h>
#endif

#if defined(XRT_OS_WINDOWS) && !defined(XRT_ENV_MINGW)
#define PATH_MAX 4096
#endif

#ifdef XRT_OS_LINUX
#include <linux/limits.h>
#endif

#ifdef XRT_OS_UNIX
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int
u_file_write_private_atomic(const char *path, const void *data, size_t size)
{
#ifdef XRT_OS_UNIX
	char temporary[PATH_MAX];
	if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = mkstemp(temporary); // Created privately from the first write.
	if (fd < 0)
		return -1;
	if (fchmod(fd, S_IRUSR | S_IWUSR) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
		int saved_errno = errno;
		close(fd);
		unlink(temporary);
		errno = saved_errno;
		return -1;
	}
	size_t offset = 0;
	while (offset < size) {
		ssize_t count = write(fd, (const char *)data + offset, size - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int saved_errno = count == 0 ? EIO : errno;
			close(fd);
			unlink(temporary);
			errno = saved_errno;
			return -1;
		}
		offset += (size_t)count;
	}
	int result = fsync(fd);
	int saved_errno = errno;
	if (close(fd) != 0 && result == 0) {
		result = -1;
		saved_errno = errno;
	}
	if (result == 0 && rename(temporary, path) == 0)
		return 0;
	if (result == 0)
		saved_errno = errno;
	unlink(temporary);
	errno = saved_errno;
	return -1;
#else
	(void)path;
	(void)data;
	(void)size;
	errno = ENOSYS;
	return -1;
#endif
}

#ifdef XRT_OS_WINDOWS
typedef DWORD(WINAPI *PFN_GetTempPath2A)(DWORD, LPSTR);
typedef DWORD(WINAPI *PFN_GetTempPath2W)(DWORD, LPWSTR);

static DWORD
get_temp_path_a(DWORD out_path_size, LPSTR out_path)
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	PFN_GetTempPath2A get_temp_path2 =
	    kernel32 != NULL ? (PFN_GetTempPath2A)GetProcAddress(kernel32, "GetTempPath2A") : NULL;
	if (get_temp_path2 != NULL) {
		return get_temp_path2(out_path_size, out_path);
	}

	return GetTempPathA(out_path_size, out_path);
}

static DWORD
get_temp_path_w(DWORD out_path_size, LPWSTR out_path)
{
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	PFN_GetTempPath2W get_temp_path2 =
	    kernel32 != NULL ? (PFN_GetTempPath2W)GetProcAddress(kernel32, "GetTempPath2W") : NULL;
	if (get_temp_path2 != NULL) {
		return get_temp_path2(out_path_size, out_path);
	}

	return GetTempPathW(out_path_size, out_path);
}
#endif

#if defined(XRT_OS_LINUX) || defined(XRT_OS_OSX)
#include <sys/stat.h>
#endif

#ifdef XRT_OS_LINUX
#include <linux/limits.h>
#endif

#if defined(XRT_OS_LINUX) || defined(XRT_OS_OSX)
static int
mkpath(const char *path)
{
	char tmp[PATH_MAX];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp) - 1;
	if (tmp[len] == '/') {
		tmp[len] = 0;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}

	if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST) {
		return -1;
	}

	return 0;
}

#ifdef XRT_OS_LINUX
static bool
is_dir(const char *path)
{
	struct stat st = {0};
	if (!stat(path, &st)) {
		return S_ISDIR(st.st_mode);
	} else {
		return false;
	}
}
#endif

int
u_file_get_config_dir(char *out_path, size_t out_path_size)
{
#ifdef XRT_OS_OSX
	const char *home = getenv("HOME");
	if (home != NULL) {
		return snprintf(out_path, out_path_size, "%s/Library/Application Support/monado", home);
	}
	return -1;
#else
	const char *xdg_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg_home != NULL) {
		return snprintf(out_path, out_path_size, "%s/monado", xdg_home);
	}
	if (home != NULL) {
		return snprintf(out_path, out_path_size, "%s/.config/monado", home);
	}
	return -1;
#endif
}

int
u_file_get_path_in_config_dir(const char *suffix, char *out_path, size_t out_path_size)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return i;
	}

	return snprintf(out_path, out_path_size, "%s/%s", tmp, suffix);
}

FILE *
u_file_open_file_in_config_dir(const char *filename, const char *mode)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return NULL;
	}

	char file_str[PATH_MAX + 15];
	i = snprintf(file_str, sizeof(file_str), "%s/%s", tmp, filename);
	if (i <= 0) {
		return NULL;
	}

	FILE *file = fopen(file_str, mode);
	if (file != NULL) {
		return file;
	}

	// Try creating the path.
	mkpath(tmp);

	// Do not report error.
	return fopen(file_str, mode);
}

FILE *
u_file_open_file_in_config_dir_subpath(const char *subpath, const char *filename, const char *mode)
{
	char tmp[PATH_MAX];
	int i = u_file_get_config_dir(tmp, sizeof(tmp));
	if (i < 0 || i >= (int)sizeof(tmp)) {
		return NULL;
	}

	char fullpath[PATH_MAX];
	i = snprintf(fullpath, sizeof(fullpath), "%s/%s", tmp, subpath);
	if (i < 0 || i >= (int)sizeof(fullpath)) {
		return NULL;
	}

	char file_str[PATH_MAX + 15];
	i = snprintf(file_str, sizeof(file_str), "%s/%s", fullpath, filename);
	if (i < 0 || i >= (int)sizeof(file_str)) {
		return NULL;
	}

	FILE *file = fopen(file_str, mode);
	if (file != NULL) {
		return file;
	}

	// Try creating the path.
	mkpath(fullpath);

	// Do not report error.
	return fopen(file_str, mode);
}

int
u_file_get_hand_tracking_models_dir(char *out_path, size_t out_path_size)
{
#ifdef XRT_OS_OSX
	const char *home = getenv("HOME");
	if (home != NULL) {
		return snprintf(out_path, out_path_size, "%s/Library/Application Support/monado/hand-tracking-models",
		                home);
	}

	if (out_path_size > 0) {
		out_path[0] = '\0';
	}

	return -1;
#else
	const char *suffix = "/monado/hand-tracking-models";
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	int ret = 0;

	if (xdg_data_home != NULL) {
		ret = snprintf(out_path, out_path_size, "%s%s", xdg_data_home, suffix);
		if (ret > 0 && is_dir(out_path)) {
			return ret;
		}
	}

	if (home != NULL) {
		ret = snprintf(out_path, out_path_size, "%s/.local/share%s", home, suffix);
		if (ret > 0 && is_dir(out_path)) {
			return ret;
		}
	}

	ret = snprintf(out_path, out_path_size, "/usr/local/share%s", suffix);
	if (ret > 0 && is_dir(out_path)) {
		return ret;
	}

	ret = snprintf(out_path, out_path_size, "/usr/share%s", suffix);
	if (ret > 0 && is_dir(out_path)) {
		return ret;
	}

	if (out_path_size > 0) {
		out_path[0] = '\0';
	}

	return ret;
#endif
}

#endif /* XRT_OS_LINUX || XRT_OS_OSX */

int
u_file_get_runtime_dir(char *out_path, size_t out_path_size)
{
	const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
	if (xdg_rt != NULL) {
		return snprintf(out_path, out_path_size, "%s", xdg_rt);
	}

#ifdef XRT_OS_OSX
	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL) {
		int ret = snprintf(out_path, out_path_size, "%s/monado", xdg_cache);
		if (ret <= 0 || ret >= (int)out_path_size) {
			return ret;
		}
		return mkpath(out_path) == 0 ? ret : -1;
	}

	const char *home = getenv("HOME");
	if (home != NULL) {
		int ret = snprintf(out_path, out_path_size, "%s/Library/Caches/monado", home);
		if (ret <= 0 || ret >= (int)out_path_size) {
			return ret;
		}
		return mkpath(out_path) == 0 ? ret : -1;
	}
#else
	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL) {
		return snprintf(out_path, out_path_size, "%s", xdg_cache);
	}
#endif

#ifdef XRT_OS_WINDOWS
#ifndef UNICODE // If Unicode support is disabled, use ANSI functions directly into out_path
	return (int)get_temp_path_a((DWORD)out_path_size, out_path);
#else
	WCHAR temp[MAX_PATH] = {0};
	get_temp_path_w((DWORD)(sizeof(temp) / sizeof(temp[0])), temp);
	return wcstombs(out_path, temp, out_path_size);
#endif // UNICODE
#else
#ifdef XRT_OS_OSX
	return -1;
#else
	const char *cache = "~/.cache";
	return snprintf(out_path, out_path_size, "%s", cache);
#endif
#endif
}

int
u_file_get_path_in_runtime_dir(const char *suffix, char *out_path, size_t out_path_size)
{
	char tmp[PATH_MAX];
	int i = u_file_get_runtime_dir(tmp, sizeof(tmp));
	if (i <= 0) {
		return i;
	}

	return snprintf(out_path, out_path_size, "%s/%s", tmp, suffix);
}

char *
u_file_read_content(FILE *file, size_t *out_file_size)
{
	// Go to the end of the file.
	fseek(file, 0L, SEEK_END);
	size_t file_size = ftell(file);

	// Return back to the start of the file.
	fseek(file, 0L, SEEK_SET);

	char *buffer = (char *)calloc(file_size + 1, sizeof(char));
	if (buffer == NULL) {
		return NULL;
	}

	// Do the actual reading.
	size_t ret = fread(buffer, sizeof(char), file_size, file);
	if (ret != file_size) {
		free(buffer);
		return NULL;
	}

	if (out_file_size) {
		*out_file_size = file_size;
	}

	return buffer;
}

char *
u_file_read_content_from_path(const char *path, size_t *out_file_size)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		return NULL;
	}
	char *file_content = u_file_read_content(file, out_file_size);
	int ret = fclose(file);
	// We don't care about the return value since we're just reading
	(void)ret;

	// Either valid non-null or null
	return file_content;
}
