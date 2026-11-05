# Copyright 2026, Finn Zeruhn
# SPDX-License-Identifier: BSL-1.0

####
# HID library
#
# Kept out of aux_os, as backends here may bring in DSOs.
#

add_library(aux_hid STATIC h_hid.h h_hid_hidraw.c)
target_link_libraries(aux_hid PUBLIC aux-includes aux_os)
