// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdbool.h>
#include <stdint.h>

struct comp_compositor;
struct u_macos_hosted_client;
struct comp_macos_frontend;
struct comp_macos_frontend_info
{
	//! Borrowed until frontend destruction.
	CAMetalLayer *metal_layer;
	CGDirectDisplayID display_id;
	uint32_t pixel_width, pixel_height;
	char display_name[128];
};

struct comp_macos_frontend *
comp_macos_frontend_create(struct comp_compositor *c,
                           struct u_macos_hosted_client *client,
                           struct comp_macos_frontend_info *out_info);
void
comp_macos_frontend_show(struct comp_macos_frontend *frontend);
void
comp_macos_frontend_destroy(struct comp_macos_frontend **frontend_ptr);
void
comp_macos_frontend_set_title(struct comp_macos_frontend *frontend, const char *title);
bool
comp_macos_frontend_is_visible(struct comp_macos_frontend *frontend);
const char *
comp_macos_frontend_name(struct comp_macos_frontend *frontend);
//! Capture the handoff now; promotion runs off the presented-handler thread.
void
comp_macos_frontend_note_present(struct comp_macos_frontend *frontend, id<MTLDrawable> drawable);
bool
comp_macos_frontend_detect(void);
CGDirectDisplayID
comp_macos_frontend_display_id(NSScreen *screen);
