// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Helpers for making shared Metal textures compatible with a Vulkan device.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

@interface MTLSharedTextureHandle (MonadoMachPort)
- (instancetype)initWithMachPort:(mach_port_t)port;
@end

#include "util/comp_metal_texture_device.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"
#include "xrt/xrt_compositor.h"

bool
comp_metal_texture_prepare_for_vk_device(struct vk_bundle *vk,
                                         void *source_texture,
                                         void **out_texture,
                                         bool *out_needs_release)
{
	if (vk == NULL || source_texture == NULL || out_texture == NULL || out_needs_release == NULL ||
	    vk->vkExportMetalObjectsEXT == NULL) {
		return false;
	}

	*out_texture = NULL;
	*out_needs_release = false;

	VkExportMetalDeviceInfoEXT device_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
	};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &device_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);

	id<MTLDevice> vk_device = (__bridge id<MTLDevice>)device_info.mtlDevice;
	id<MTLTexture> source = (__bridge id<MTLTexture>)source_texture;
	if (vk_device == nil || source == nil) {
		U_LOG_E("Metal texture device normalization failed: vk_device=%p source=%p",
		        (__bridge void *)vk_device,
		        source_texture);
		return false;
	}

	if (source.device == vk_device) {
		*out_texture = source_texture;
		return true;
	}

	@autoreleasepool {
		MTLSharedTextureHandle *handle = [source newSharedTextureHandle];
		if (handle == nil) {
			U_LOG_E("Metal texture device normalization could not create shared handle: source=%p source_device=%p vk_device=%p",
			        source_texture,
			        (__bridge void *)source.device,
			        (__bridge void *)vk_device);
			return false;
		}

		id<MTLTexture> rebound = [vk_device newSharedTextureWithHandle:handle];
		[handle release];
		if (rebound == nil) {
			U_LOG_E("Metal texture device normalization could not reopen shared texture on Vulkan MTLDevice: source=%p source_device=%p vk_device=%p",
			        source_texture,
			        (__bridge void *)source.device,
			        (__bridge void *)vk_device);
			return false;
		}

		if (rebound.width != source.width || rebound.height != source.height ||
		    rebound.arrayLength != source.arrayLength || rebound.mipmapLevelCount != source.mipmapLevelCount ||
		    rebound.sampleCount != source.sampleCount || rebound.pixelFormat != source.pixelFormat ||
		    rebound.textureType != source.textureType) {
			U_LOG_E("Metal texture device normalization changed texture geometry/format: source=%p rebound=%p",
			        source_texture,
			        (__bridge void *)rebound);
			[rebound release];
			return false;
		}

		U_LOG_I("Metal shared texture rebound to Vulkan MTLDevice: source=%p source_device=%p rebound=%p vk_device=%p",
		        source_texture,
		        (__bridge void *)source.device,
		        (__bridge void *)rebound,
		        (__bridge void *)vk_device);

		*out_texture = (__bridge void *)rebound;
		*out_needs_release = true;
		return true;
	}
}

static bool
get_vk_metal_device(struct vk_bundle *vk, id<MTLDevice> *out_device)
{
	if (vk == NULL || out_device == NULL || vk->vkExportMetalObjectsEXT == NULL) {
		return false;
	}
	VkExportMetalDeviceInfoEXT device_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &device_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
	id<MTLDevice> device = (__bridge id<MTLDevice>)device_info.mtlDevice;
	if (device == nil) {
		return false;
	}
	*out_device = device;
	return true;
}

static MTLPixelFormat
vk_format_to_metal_color_format(int64_t format)
{
	switch ((uint32_t)format) {
	case 37: return MTLPixelFormatRGBA8Unorm;
	case 43: return MTLPixelFormatRGBA8Unorm_sRGB;
	case 44: return MTLPixelFormatBGRA8Unorm;
	case 50: return MTLPixelFormatBGRA8Unorm_sRGB;
	default: return MTLPixelFormatInvalid;
	}
}

static MTLTextureUsage
xrt_usage_to_metal(enum xrt_swapchain_usage_bits bits)
{
	MTLTextureUsage usage = MTLTextureUsageUnknown;
	if ((bits & XRT_SWAPCHAIN_USAGE_COLOR) != 0) usage |= MTLTextureUsageRenderTarget;
	if ((bits & XRT_SWAPCHAIN_USAGE_SAMPLED) != 0 || (bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0)
		usage |= MTLTextureUsageShaderRead;
	if ((bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0) usage |= MTLTextureUsageShaderWrite;
	if ((bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT) != 0) usage |= MTLTextureUsagePixelFormatView;
	return usage;
}

bool
comp_metal_texture_create_from_bootstrap_name_for_vk_device(struct vk_bundle *vk,
                                                            const struct xrt_swapchain_create_info *info,
                                                            const char *bootstrap_name,
                                                            void **out_texture)
{
	if (vk == NULL || info == NULL || bootstrap_name == NULL || bootstrap_name[0] == '\0' || out_texture == NULL) {
		return false;
	}
	*out_texture = NULL;
	if (info->array_size == 0 || info->face_count != 1 || info->mip_count != 1 || info->sample_count != 1) {
		U_LOG_E("Metal bootstrap import only supports 2D single-mip single-sample textures: array_size=%u",
		        info->array_size);
		return false;
	}

	MTLPixelFormat pixel_format = vk_format_to_metal_color_format(info->format);
	if (pixel_format == MTLPixelFormatInvalid) {
		U_LOG_E("Metal bootstrap import unsupported Vulkan format=%lld", (long long)info->format);
		return false;
	}

	id<MTLDevice> vk_device = nil;
	if (!get_vk_metal_device(vk, &vk_device)) {
		return false;
	}

	mach_port_t bootstrap = MACH_PORT_NULL;
	if (task_get_bootstrap_port(mach_task_self(), &bootstrap) != KERN_SUCCESS || bootstrap == MACH_PORT_NULL) {
		U_LOG_E("Metal bootstrap import could not obtain bootstrap port for '%s'", bootstrap_name);
		return false;
	}

	mach_port_t texture_port = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_look_up(bootstrap, (char *)bootstrap_name, &texture_port);
	mach_port_deallocate(mach_task_self(), bootstrap);
	if (kr != KERN_SUCCESS || texture_port == MACH_PORT_NULL) {
		U_LOG_E("Metal bootstrap lookup failed for '%s': kr=%d", bootstrap_name, (int)kr);
		return false;
	}

	@autoreleasepool {
		MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:texture_port];
		if (handle == nil) {
			mach_port_deallocate(mach_task_self(), texture_port);
			U_LOG_E("Could not reconstruct MTLSharedTextureHandle for '%s'", bootstrap_name);
			return false;
		}

		id<MTLTexture> texture = [vk_device newSharedTextureWithHandle:handle];
		[handle release];
		if (texture == nil) {
			U_LOG_E("Could not reopen DXMT shared texture '%s' on Vulkan MTLDevice", bootstrap_name);
			return false;
		}

		MTLTextureType expected_type = info->array_size > 1 ? MTLTextureType2DArray : MTLTextureType2D;
		if (texture.device != vk_device || texture.width != info->width || texture.height != info->height ||
		    texture.arrayLength != info->array_size || texture.mipmapLevelCount != 1 || texture.sampleCount != 1 ||
		    texture.pixelFormat != pixel_format || texture.textureType != expected_type) {
			U_LOG_E("DXMT shared texture geometry mismatch for '%s': got=%lux%lu array=%lu type=%lu format=%lu expected=%ux%u array=%u type=%lu format=%lu",
			        bootstrap_name,
			        (unsigned long)texture.width,
			        (unsigned long)texture.height,
			        (unsigned long)texture.arrayLength,
			        (unsigned long)texture.textureType,
			        (unsigned long)texture.pixelFormat,
			        info->width,
			        info->height,
			        info->array_size,
			        (unsigned long)expected_type,
			        (unsigned long)pixel_format);
			[texture release];
			return false;
		}

		U_LOG_I("DXMT shared Metal texture imported by bootstrap name: '%s' texture=%p array_size=%u",
		        bootstrap_name,
		        (__bridge void *)texture,
		        info->array_size);
		*out_texture = (__bridge void *)texture;
		return true;
	}
}

bool
comp_metal_texture_create_from_iosurface_id_for_vk_device(struct vk_bundle *vk,
                                                          const struct xrt_swapchain_create_info *info,
                                                          uint32_t iosurface_id,
                                                          void **out_texture)
{
	if (vk == NULL || info == NULL || iosurface_id == 0 || out_texture == NULL) return false;
	*out_texture = NULL;
	if (info->array_size != 1 || info->face_count != 1 || info->mip_count != 1 || info->sample_count != 1) {
		U_LOG_E("IOSurface-ID import only supports 2D single-mip single-sample images: id=%u", iosurface_id);
		return false;
	}
	MTLPixelFormat pixel_format = vk_format_to_metal_color_format(info->format);
	if (pixel_format == MTLPixelFormatInvalid) {
		U_LOG_E("IOSurface-ID import unsupported Vulkan format: id=%u format=%lld", iosurface_id, (long long)info->format);
		return false;
	}
	id<MTLDevice> vk_device = nil;
	if (!get_vk_metal_device(vk, &vk_device)) return false;

	@autoreleasepool {
		IOSurfaceRef surface = IOSurfaceLookup((IOSurfaceID)iosurface_id);
		if (surface == NULL) {
			U_LOG_E("IOSurfaceLookup failed for external id=%u", iosurface_id);
			return false;
		}
		if (IOSurfaceGetWidth(surface) != info->width || IOSurfaceGetHeight(surface) != info->height ||
		    IOSurfaceGetBytesPerElement(surface) < 4) {
			U_LOG_E("External IOSurface geometry mismatch: id=%u got=%zux%zu expected=%ux%u",
			        iosurface_id, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), info->width, info->height);
			CFRelease(surface);
			return false;
		}
		MTLTextureDescriptor *descriptor =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format width:info->width height:info->height mipmapped:NO];
		descriptor.storageMode = MTLStorageModeShared;
		descriptor.usage = xrt_usage_to_metal(info->bits);
		id<MTLTexture> texture = [vk_device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
		CFRelease(surface);
		if (texture == nil) {
			U_LOG_E("Could not create MTLTexture from external IOSurface id=%u", iosurface_id);
			return false;
		}
		if (texture.device != vk_device || texture.width != info->width || texture.height != info->height ||
		    texture.arrayLength != 1 || texture.mipmapLevelCount != 1 || texture.sampleCount != 1 ||
		    texture.pixelFormat != pixel_format || texture.textureType != MTLTextureType2D) {
			[texture release];
			return false;
		}
		U_LOG_I("External IOSurface imported on MoltenVK device: id=%u texture=%p", iosurface_id, (__bridge void *)texture);
		*out_texture = (__bridge void *)texture;
		return true;
	}
}

void
comp_metal_texture_release(void *texture)
{
	if (texture != NULL) [(__bridge id<MTLTexture>)texture release];
}

void
comp_metal_texture_finish_for_vk_device(void *texture, bool needs_release)
{
	if (!needs_release || texture == NULL) {
		return;
	}

	id<MTLTexture> metal_texture = (__bridge id<MTLTexture>)texture;
	[metal_texture release];
}
