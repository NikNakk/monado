// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "m_metal_foveation_angular.h"
#include <pthread.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
@interface MonadoFoveationAngularMapEntry : NSObject {
@public
	id<MTLRasterizationRateMap> map;
@public
	struct u_foveation_angular_recipe recipe;
}
@end
@implementation MonadoFoveationAngularMapEntry
- (void)dealloc
{
	[map release];
	[super dealloc];
}
@end
struct m_metal_foveation_angular
{
	id<MTLDevice> device;
	NSMutableDictionary *maps;
	id<MTLRasterizationRateMap> current;
	pthread_mutex_t mutex;
	struct u_foveation_angular_config config;
	struct u_foveation_angular_history history;
	uint32_t revision, builds;
};
struct m_metal_foveation_angular *
m_metal_foveation_angular_create(void *device, const struct u_foveation_angular_config *config)
{
	if (!u_foveation_angular_config_valid(config) ||
	    ![(id<MTLDevice>)device supportsRasterizationRateMapWithLayerCount:1])
		return NULL;
	struct m_metal_foveation_angular *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->device = [(id<MTLDevice>)device retain];
	c->config = *config;
	c->maps = [[NSMutableDictionary alloc] init];
	if (!c->maps || pthread_mutex_init(&c->mutex, NULL)) {
		[c->maps release];
		[c->device release];
		free(c);
		return NULL;
	}
	return c;
}
void
m_metal_foveation_angular_destroy(struct m_metal_foveation_angular *c)
{
	if (!c)
		return;
	[c->current release];
	[c->maps release];
	[c->device release];
	pthread_mutex_destroy(&c->mutex);
	free(c);
}
bool
m_metal_foveation_angular_lookup(struct m_metal_foveation_angular *c,
                                 const struct u_foveation_angular_target *t,
                                 bool valid,
                                 float yaw,
                                 float pitch,
                                 bool diagnostics,
                                 struct m_metal_foveation_angular_result *out)
{
	if (!c || !out || !u_foveation_angular_target_valid(t))
		return false;
	pthread_mutex_lock(&c->mutex);
	@autoreleasepool {
		struct u_foveation_angular_history h = c->history;
		u_foveation_angular_update(&c->config, valid, yaw, pitch, &h);
		/* Full float FOVs prevent the legacy key's vertical-FOV alias. No rate math on hits. */
		struct
		{
			uint32_t width, height;
			struct u_foveation_angular_view views[2];
			int32_t yaw, pitch, have;
		} key;
		memset(&key, 0, sizeof(key));
		key.width = t->width;
		key.height = t->height;
		memcpy(key.views, t->views, sizeof(key.views));
		key.yaw = h.have_gaze ? (int32_t)lroundf(h.yaw * 8) : 0;
		key.pitch = h.have_gaze ? (int32_t)lroundf(h.pitch * 8) : 0;
		key.have = h.have_gaze;
		NSData *data = [NSData dataWithBytes:&key length:sizeof(key)];
		MonadoFoveationAngularMapEntry *entry = [c->maps objectForKey:data];
		if (!entry) {
			entry = [[MonadoFoveationAngularMapEntry alloc] init];
			u_foveation_angular_build_recipe(&c->config, t, &h, &entry->recipe);
			MTLRasterizationRateLayerDescriptor *layer =
			    [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(64, 32, 1)];
			for (unsigned i = 0; i < 64; ++i)
				layer.horizontal[i] = @(entry->recipe.horizontal[i]);
			for (unsigned j = 0; j < 32; ++j)
				layer.vertical[j] = @(entry->recipe.vertical[j]);
			MTLRasterizationRateMapDescriptor *desc = [MTLRasterizationRateMapDescriptor
			    rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(t->width, t->height, 1)
			                                           layer:layer];
			entry->map = [c->device newRasterizationRateMapWithDescriptor:desc];
			[layer release];
			if (!entry->map) {
				[entry release];
				pthread_mutex_unlock(&c->mutex);
				return false;
			}
			if (c->maps.count >= 256)
				[c->maps removeAllObjects];
			[c->maps setObject:entry forKey:data];
			[entry release];
			++c->builds;
		}
		if (c->current != entry->map) {
			[entry->map retain];
			[c->current release];
			c->current = entry->map;
			++c->revision;
		}
		c->history = h;
		MTLSize size = [c->current physicalSizeForLayer:0];
		out->map = (void *)c->current;
		out->width = (uint32_t)size.width;
		out->height = (uint32_t)size.height;
		out->revision = c->revision;
		if (diagnostics) {
			out->history = h;
			out->builds = c->builds;
			out->cache_count = (uint32_t)c->maps.count;
			out->recipe = entry->recipe;
		}
	}
	pthread_mutex_unlock(&c->mutex);
	return true;
}
