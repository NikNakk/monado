// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * One full-screen triangle drawn into a single color attachment. The target is
 * only used as a render target, so presentation images need no storage,
 * sampling or transfer usage. Previous contents are discarded.
 */
struct render_fullscreen
{
	VkRenderPass render_pass;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
};

//! The attachment starts UNDEFINED and finishes in final_layout.
VkResult
render_fullscreen_init(struct vk_bundle *vk,
                       struct render_fullscreen *rf,
                       VkShaderModule vert,
                       VkShaderModule frag,
                       VkDescriptorSetLayout set_layout,
                       VkFormat format,
                       VkImageLayout final_layout);

VkResult
render_fullscreen_framebuffer(struct vk_bundle *vk,
                              const struct render_fullscreen *rf,
                              VkImageView view,
                              VkExtent2D extent,
                              VkFramebuffer *out_framebuffer);

void
render_fullscreen_draw(struct vk_bundle *vk,
                       const struct render_fullscreen *rf,
                       VkCommandBuffer cmd,
                       VkFramebuffer framebuffer,
                       VkExtent2D extent,
                       VkDescriptorSet descriptors);

void
render_fullscreen_fini(struct vk_bundle *vk, struct render_fullscreen *rf);

#ifdef __cplusplus
}
#endif
