// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "comp_macos_presenter.h"
#include "util/u_debug.h"
#include <pthread.h>

static pthread_once_t presenter_once = PTHREAD_ONCE_INIT;
static enum comp_macos_presenter presenter;

static void
read_presenter(void)
{
	char storage[DEBUG_CHAR_STORAGE_SIZE];
	const char *value = debug_get_option(storage, sizeof(storage), "XRT_MACOS_PRESENTER", "native");
	if (!comp_macos_presenter_parse(value, &presenter)) {
		U_LOG_W("Invalid XRT_MACOS_PRESENTER='%s': expected wsi, wsi-adaptive or native; using native", value);
	}
}

enum comp_macos_presenter
comp_macos_presenter_get(void)
{
	pthread_once(&presenter_once, read_presenter);
	return presenter;
}
