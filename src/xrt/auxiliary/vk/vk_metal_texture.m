// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Spike: shareable Metal textures for Vulkan images on macOS.
 * @ingroup aux_vk
 */
#import <Metal/Metal.h>

#include "util/u_logging.h"
#include "vk/vk_metal_texture.h"

void *
vk_metal_texture_create_shareable_like(void *metal_texture)
{
	@autoreleasepool {
		id<MTLTexture> source = (id<MTLTexture>)metal_texture;
		if (source == nil) {
			return NULL;
		}

		// MoltenVK is the source of truth for format, usage and shape: copy
		// what it chose instead of keeping a second mapping table here.
		MTLTextureDescriptor *descriptor = [[[MTLTextureDescriptor alloc] init] autorelease];
		descriptor.textureType = source.textureType;
		descriptor.pixelFormat = source.pixelFormat;
		descriptor.width = source.width;
		descriptor.height = source.height;
		descriptor.depth = source.depth;
		descriptor.mipmapLevelCount = source.mipmapLevelCount;
		descriptor.sampleCount = source.sampleCount;
		descriptor.arrayLength = source.arrayLength;
		descriptor.usage = source.usage;
		descriptor.swizzle = source.swizzle;
		descriptor.allowGPUOptimizedContents = source.allowGPUOptimizedContents;
		// Shared textures must use private storage.
		descriptor.storageMode = MTLStorageModePrivate;

		id<MTLTexture> shareable = [source.device newSharedTextureWithDescriptor:descriptor];
		if (shareable == nil) {
			U_LOG_W("Could not create a shareable Metal texture: format=%lu type=%lu %lux%lu layers=%lu "
			        "usage=0x%lx",
			        (unsigned long)source.pixelFormat, (unsigned long)source.textureType,
			        (unsigned long)source.width, (unsigned long)source.height,
			        (unsigned long)source.arrayLength, (unsigned long)source.usage);
			return NULL;
		}
		return (void *)shareable; // +1 from new...
	}
}

bool
vk_metal_texture_is_shareable(void *metal_texture)
{
	id<MTLTexture> texture = (id<MTLTexture>)metal_texture;
	return texture != nil && texture.isShareable;
}
