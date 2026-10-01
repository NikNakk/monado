// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "xrt/xrt_config_os.h"
#include "shared/ipc_socket_security.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef XRT_OS_OSX
#include <sys/ucred.h>
#endif

bool
ipc_socket_get_peer_identity(int fd, uid_t *uid, pid_t *pid)
{
#ifdef XRT_OS_OSX
	gid_t gid;
	socklen_t size = sizeof(*pid);
	return getpeereid(fd, uid, &gid) == 0 && getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, pid, &size) == 0 &&
	       size == sizeof(*pid) && *pid > 0;
#elif defined(XRT_OS_LINUX)
	struct
	{
		pid_t pid;
		uid_t uid;
		gid_t gid;
	} credentials;
	socklen_t size = sizeof(credentials);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials)) {
		return false;
	}
	*uid = credentials.uid;
	*pid = credentials.pid;
	return *pid > 0;
#else
	(void)fd;
	(void)uid;
	(void)pid;
	return false;
#endif
}

int
ipc_socket_bind_exclusive(int fd, const char *path, int *out_lock_fd)
{
	*out_lock_fd = -1;
	struct sockaddr_un addr = {0};
	if (strlen(path) >= sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path) + 1);
	char lock_path[PATH_MAX];
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >= (int)sizeof(lock_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lock_fd < 0)
		return -1;
	struct stat st;
	if (fstat(lock_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0 ||
	    flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		close(lock_fd);
		errno = EADDRINUSE;
		return -1;
	}
	int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0 && errno == EADDRINUSE && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == getuid()) {
		// Also protect live services that predate the lock-file protocol.
		int probe = socket(AF_UNIX, SOCK_STREAM, 0);
		if (probe >= 0) {
			int flags = fcntl(probe, F_GETFL);
			int result = flags < 0 || fcntl(probe, F_SETFL, flags | O_NONBLOCK) < 0
			                 ? -1
			                 : connect(probe, (struct sockaddr *)&addr, sizeof(addr));
			int probe_error = errno;
			close(probe);
			if (result < 0 && probe_error == ECONNREFUSED && unlink(path) == 0) {
				ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
			} else {
				errno = EADDRINUSE;
			}
		}
	}
	if (ret < 0) {
		int saved_errno = errno;
		close(lock_fd);
		errno = saved_errno;
		return -1;
	}
	*out_lock_fd = lock_fd;
	return 0;
}
