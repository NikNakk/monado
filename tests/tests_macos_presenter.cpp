// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "../src/xrt/compositor/main/comp_macos_presenter.h"
#include "catch_amalgamated.hpp"
#include <cstdlib>
#include <string>

TEST_CASE("macOS presenter selection is explicit and defaults safely", "[macos][presenter]")
{
	comp_macos_presenter mode = COMP_MACOS_PRESENTER_WSI_ADAPTIVE;
	CHECK(comp_macos_presenter_parse(nullptr, &mode));
	CHECK(mode == COMP_MACOS_PRESENTER_NATIVE);
	CHECK(comp_macos_presenter_parse("native", &mode));
	CHECK(mode == COMP_MACOS_PRESENTER_NATIVE);
	CHECK(comp_macos_presenter_parse("wsi", &mode));
	CHECK(mode == COMP_MACOS_PRESENTER_WSI);
	CHECK(comp_macos_presenter_parse("wsi-adaptive", &mode));
	CHECK(mode == COMP_MACOS_PRESENTER_WSI_ADAPTIVE);
	for (auto value : {"", "WSI", "wsi ", "adaptive", "wsi-generic", "0", "1"}) {
		mode = COMP_MACOS_PRESENTER_WSI;
		CAPTURE(value);
		CHECK_FALSE(comp_macos_presenter_parse(value, &mode));
		CHECK(mode == COMP_MACOS_PRESENTER_NATIVE);
	}
}

#ifdef __APPLE__
TEST_CASE("macOS presenter runtime reads the selector once", "[macos][presenter-runtime]")
{
	const char *value = std::getenv("XRT_MACOS_PRESENTER");
	std::string saved = value ? value : "";
	bool was_set = value != nullptr;
	comp_macos_presenter expected;
	(void)comp_macos_presenter_parse(value, &expected);
	CHECK(comp_macos_presenter_get() == expected);
	CHECK(comp_macos_presenter_is_wsi() == (expected != COMP_MACOS_PRESENTER_NATIVE));
	setenv("XRT_MACOS_PRESENTER", expected == COMP_MACOS_PRESENTER_NATIVE ? "wsi" : "native", 1);
	CHECK(comp_macos_presenter_get() == expected);
	if (was_set)
		setenv("XRT_MACOS_PRESENTER", saved.c_str(), 1);
	else
		unsetenv("XRT_MACOS_PRESENTER");
}
#endif
