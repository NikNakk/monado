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

/*!
 * Calibrated generic Vulkan WSI. The native CAMetalLayer presenter remains the
 * explicit fallback (XRT_MACOS_PRESENTER=native), and the only one with PS VR2
 * camera passthrough unless XRT_MACOS_WSI_PASSTHROUGH=1 is also set.
 */
#define COMP_MACOS_PRESENTER_DEFAULT COMP_MACOS_PRESENTER_WSI

//! NULL means unset and selects the default. Unknown/empty values select the default and return false.
static inline bool
comp_macos_presenter_parse(const char *value, enum comp_macos_presenter *presenter)
{
	*presenter = COMP_MACOS_PRESENTER_DEFAULT;
	if (value == NULL)
		return true;
	if (strcmp(value, "native") == 0) {
		*presenter = COMP_MACOS_PRESENTER_NATIVE;
		return true;
	}
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
