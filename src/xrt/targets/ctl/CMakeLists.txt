# Copyright 2020-2021, Collabora, Ltd.
# Copyright 2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

add_executable(monado-ctl main.c)
add_sanitizers(monado-ctl)

target_include_directories(monado-ctl PRIVATE ipc)

target_link_libraries(monado-ctl PRIVATE aux_util aux_math aux_os ipc_client target_ipc_defaults)

install(TARGETS monado-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
