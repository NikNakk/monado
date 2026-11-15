#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: 2026, NVIDIA CORPORATION.
set -e
set -x

for example in "$@"; do
	rm -rf "build-examples/${example}"
	cmake -GNinja -S "examples/${example}" -B "build-examples/${example}" -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
	ninja -C "build-examples/${example}"
done
