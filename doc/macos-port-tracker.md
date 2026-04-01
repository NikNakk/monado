<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Tracker

## Branch

- `kg/macos-ipc-port-spike`

## Fork

- GitHub working fork: `kzahel/monado`
- Canonical upstream remains GitLab `monado/monado`

## Initial blocker stack

1. Missing local toolchain on the Mac host:
   `pkg-config`, `ninja`, `glslangValidator`
2. SDL2 include-path mismatch in optional desktop helper targets
3. IPC client `wait` symbol collision on BSD/macOS
4. Desktop IPC server explicitly unported on Apple
5. Linux-only `epoll` dependency in server IPC threads

## Changes started in this branch

- renamed the IPC future `wait` callback implementation to avoid the POSIX
  `wait(2)` collision
- added a macOS desktop IPC mainloop source using Unix sockets plus `poll()`
- added an Apple path to IPC CMake source selection
- added a macOS `poll()` path for per-client IPC thread waiting

## Still expected after this patch set

- more IPC/server portability work after the first `poll()` conversion
- validation that `monado-service` and the OpenXR runtime target actually link
- compositor/runtime issues after IPC is no longer the first compile stop

## Recommended workflow

1. Reconfigure with the no-SDL macOS recipe from `doc/macos-port.md`
2. Build with `ninja -k 1`
3. Fix only the first blocker each round
4. Keep notes here so the blocker order stays explicit
