// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum comp_macos_presenter
{
	COMP_MACOS_PRESENTER_NATIVE,
	COMP_MACOS_PRESENTER_WSI,
	COMP_MACOS_PRESENTER_WSI_ADAPTIVE,
};

//! Unknown/empty values fail safely to native. NULL means the unset default.
static inline bool
comp_macos_presenter_parse(const char *value, enum comp_macos_presenter *presenter)
{
	*presenter = COMP_MACOS_PRESENTER_NATIVE;
	if (value == NULL || strcmp(value, "native") == 0)
		return true;
	if (strcmp(value, "wsi") == 0) {
		*presenter = COMP_MACOS_PRESENTER_WSI;
		return true;
	}
	if (strcmp(value, "wsi-adaptive") == 0) {
		*presenter = COMP_MACOS_PRESENTER_WSI_ADAPTIVE;
		return true;
	}
	return false;
}

//! Process-wide selection, shared by service/hosted factories and settings.
enum comp_macos_presenter
comp_macos_presenter_get(void);

static inline bool
comp_macos_presenter_is_wsi(void)
{
	return comp_macos_presenter_get() != COMP_MACOS_PRESENTER_NATIVE;
}

#ifdef __cplusplus
}
#endif
