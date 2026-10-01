#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
# Source this before bootstrapping the service or launching a Wine IPC client.
# Both processes inherit the secret; never put it in command-line arguments.

if [[ -z ${IPC_WINE_TCP_TOKEN:-} ]]; then
    IPC_WINE_TCP_TOKEN=$(python3 - <<'PY'
import fcntl
import os
from pathlib import Path
import secrets
import stat

root = Path.home() / "Library/Caches/monado"
root.mkdir(mode=0o700, parents=True, exist_ok=True)
st = root.lstat()
if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
    raise SystemExit("Wine authentication requires a private, owned Monado cache directory")
fd = os.open(root / "wine-tcp-token", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
try:
    fcntl.flock(fd, fcntl.LOCK_EX)
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise SystemExit("Wine authentication requires a private, owned token file")
    if st.st_size == 0:
        os.write(fd, (secrets.token_hex(32) + "\n").encode("ascii"))
        os.fsync(fd)
        os.lseek(fd, 0, os.SEEK_SET)
    token = os.read(fd, 66).decode("ascii").strip()
    if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
        raise SystemExit("Invalid Wine authentication token file")
    print(token)
finally:
    os.close(fd)
PY
    ) || return 1
fi
if [[ ${#IPC_WINE_TCP_TOKEN} -ne 64 || ${IPC_WINE_TCP_TOKEN//[0-9a-f]/} != "" ]]; then
    print -u2 "IPC_WINE_TCP_TOKEN must contain 64 lowercase hexadecimal characters"
    return 1
fi
export IPC_WINE_TCP_TOKEN
