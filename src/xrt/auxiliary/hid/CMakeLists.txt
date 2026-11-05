# Copyright 2026, Finn Zeruhn
# SPDX-License-Identifier: BSL-1.0

####
# HID library
#
# Kept out of aux_os, as backends here may bring in DSOs.
#

add_library(aux_hid STATIC h_hid.h h_hid_hidraw.c)
target_link_libraries(aux_hid PUBLIC aux-includes aux_os)

# hidapi backs HID where there is no hidraw
if(XRT_HAVE_HIDAPI
   AND NOT XRT_HAVE_LINUX
   AND NOT ANDROID
	)
	target_sources(aux_hid PRIVATE h_hid_hidapi.c)
	target_link_libraries(aux_hid PRIVATE HIDAPI::hidapi)
endif()
