// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cross-process CALayer hosting (CAContext / CALayerHost) for macOS.
 *
 * These are private QuartzCore interfaces. They are declared and checked at
 * runtime exactly as Chromium does in ui/base/cocoa/remote_layer_api, and only
 * the calls Chromium uses are used. A process renders into a layer attached to
 * a CAContext and sends its contextId to another process, which shows it
 * through a CALayerHost. See doc/macos-remote-layer-hosting.md.
 *
 * Objective-C only.
 *
 * @ingroup comp_main
 */

#pragma once

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <dlfcn.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t CGSConnectionID;
typedef uint32_t CAContextID;

@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(CAContextID)contextId options:(NSDictionary *)optionsDict;
@property(readonly) CAContextID contextId;
@property(retain) CALayer *layer;
@end

@interface CALayerHost : CALayer
@property CAContextID contextId;
@end

//! True if every interface used here is present (Chromium's RemoteLayerAPISupported()).
static inline bool
comp_macos_remote_layer_supported(void)
{
	Class context_class = NSClassFromString(@"CAContext");
	if (context_class == nil || ![context_class respondsToSelector:@selector(contextWithCGSConnection:options:)] ||
	    class_getProperty(context_class, "contextId") == NULL ||
	    class_getProperty(context_class, "layer") == NULL) {
		return false;
	}

	Class host_class = NSClassFromString(@"CALayerHost");
	if (host_class == nil || ![host_class instancesRespondToSelector:@selector(contextId)] ||
	    ![host_class instancesRespondToSelector:@selector(setContextId:)]) {
		return false;
	}

	return dlsym(RTLD_DEFAULT, "CGSMainConnectionID") != NULL;
}

/*!
 * Create a CAContext on this process's window server connection, showing
 * @p layer. Returns a retained object, or nil. The caller sends contextId to
 * the hosting process.
 */
static inline CAContext *
comp_macos_remote_layer_create_context(CALayer *layer)
{
	if (layer == nil || !comp_macos_remote_layer_supported()) {
		return nil;
	}
	typedef CGSConnectionID (*main_connection_fn)(void);
	main_connection_fn main_connection = (main_connection_fn)dlsym(RTLD_DEFAULT, "CGSMainConnectionID");
	Class context_class = NSClassFromString(@"CAContext");
	if (main_connection == NULL || context_class == nil) {
		return nil;
	}
	CAContext *context = [[context_class contextWithCGSConnection:main_connection() options:@{}] retain];
	context.layer = layer;
	return context;
}

/*!
 * Create a CALayerHost showing the CAContext @p context_id from another
 * process, anchored at the origin. Returns a retained object, or nil.
 */
static inline CALayerHost *
comp_macos_remote_layer_create_host(CAContextID context_id)
{
	if (context_id == 0 || !comp_macos_remote_layer_supported()) {
		return nil;
	}
	Class host_class = NSClassFromString(@"CALayerHost");
	if (host_class == nil) {
		return nil;
	}
	CALayerHost *host = [[host_class alloc] init];
	host.anchorPoint = CGPointZero;
	host.position = CGPointZero;
	host.contextId = context_id;
	return host;
}
