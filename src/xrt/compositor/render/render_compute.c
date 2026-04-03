// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The compositor compute based rendering code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_render
 */

#include "math/m_api.h"
#include "math/m_matrix_4x4_f64.h"

#include "util/u_debug.h"

#include "vk/vk_mini_helpers.h"

#include "render/render_interface.h"

#include <stdio.h>


DEBUG_GET_ONCE_BOOL_OPTION(log_timewarp_inputs, "XRT_COMPOSITOR_LOG_TIMEWARP_INPUTS", false)
DEBUG_GET_ONCE_BOOL_OPTION(force_timewarp_identity, "XRT_COMPOSITOR_FORCE_TIMEWARP_IDENTITY", false)
DEBUG_GET_ONCE_BOOL_OPTION(force_timewarp_pretransform_identity, "XRT_COMPOSITOR_FORCE_TIMEWARP_PRETRANSFORM_IDENTITY", false)

/*
 *
 * Helper functions.
 *
 */

/*!
 * Get the @ref vk_bundle from @ref render_compute.
 */
static inline struct vk_bundle *
vk_from_render(struct render_compute *render)
{
	return render->r->vk;
}

static uint32_t
uint_divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + (b - 1)) / b;
}

static void
calc_dispatch_dims_1_view(const struct render_viewport_data views, uint32_t *out_w, uint32_t *out_h)
{
	// Power of two divide and round up.
	uint32_t w = uint_divide_and_round_up(views.w, 8);
	uint32_t h = uint_divide_and_round_up(views.h, 8);

	*out_w = w;
	*out_h = h;
}

/*
 * For dispatching compute to the view, calculate the number of groups.
 */
static void
calc_dispatch_dims_views(const struct render_viewport_data views[XRT_MAX_VIEWS],
                         uint32_t view_count,
                         uint32_t *out_w,
                         uint32_t *out_h)
{
#define IMAX(a, b) ((a) > (b) ? (a) : (b))
	uint32_t w = 0;
	uint32_t h = 0;
	for (uint32_t i = 0; i < view_count; ++i) {
		w = IMAX(w, views[i].w);
		h = IMAX(h, views[i].h);
	}
#undef IMAX

	// Power of two divide and round up.
	w = uint_divide_and_round_up(w, 8);
	h = uint_divide_and_round_up(h, 8);

	*out_w = w;
	*out_h = h;
}

static struct xrt_normalized_rect
timewarp_identity_pre_transform(void)
{
	return (struct xrt_normalized_rect){
	    .x = 0.0f,
	    .y = 0.0f,
	    .w = 1.0f,
	    .h = 1.0f,
	};
}

static void
maybe_log_timewarp_inputs(uint64_t frame_id,
                          uint32_t eye,
                          const struct xrt_normalized_rect *pre_transform,
                          const struct xrt_normalized_rect *post_transform,
                          const struct xrt_matrix_4x4 *begin,
                          const struct xrt_matrix_4x4 *end)
{
	if (!debug_get_bool_option_log_timewarp_inputs()) {
		return;
	}

	static uint64_t log_count = 0;
	log_count++;
	if (log_count > 10 && log_count % 120 != 0) {
		return;
	}

	fprintf(stderr,
	        "atw-input frame=%llu eye=%u pre=(%.5f,%.5f,%.5f,%.5f) post=(%.5f,%.5f,%.5f,%.5f) "
	        "begin0=(%.5f,%.5f,%.5f,%.5f) begin1=(%.5f,%.5f,%.5f,%.5f) "
	        "end0=(%.5f,%.5f,%.5f,%.5f) end1=(%.5f,%.5f,%.5f,%.5f)\n",
	        (unsigned long long)frame_id,
	        eye,
	        pre_transform->x,
	        pre_transform->y,
	        pre_transform->w,
	        pre_transform->h,
	        post_transform->x,
	        post_transform->y,
	        post_transform->w,
	        post_transform->h,
	        begin->v[0],
	        begin->v[1],
	        begin->v[2],
	        begin->v[3],
	        begin->v[4],
	        begin->v[5],
	        begin->v[6],
	        begin->v[7],
	        end->v[0],
	        end->v[1],
	        end->v[2],
	        end->v[3],
	        end->v[4],
	        end->v[5],
	        end->v[6],
	        end->v[7]);
}


/*
 *
 * Vulkan helpers.
 *
 */

XRT_MAYBE_UNUSED static void
update_compute_layer_descriptor_set(struct vk_bundle *vk,
                                    uint32_t src_binding,
                                    VkSampler src_samplers[RENDER_MAX_IMAGES_SIZE],
                                    VkImageView src_image_views[RENDER_MAX_IMAGES_SIZE],
                                    uint32_t image_count,
                                    uint32_t target_binding,
                                    VkImageView target_image_view,
                                    uint32_t ubo_binding,
                                    VkBuffer ubo_buffer,
                                    VkDeviceSize ubo_size,
                                    VkDescriptorSet descriptor_set)
{
	VkDescriptorImageInfo src_image_info[RENDER_MAX_IMAGES_SIZE];
	for (uint32_t i = 0; i < image_count; i++) {
		src_image_info[i].sampler = src_samplers[i];
		src_image_info[i].imageView = src_image_views[i];
		src_image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo target_image_info = {
	    .imageView = target_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkDescriptorBufferInfo buffer_info = {
	    .buffer = ubo_buffer,
	    .offset = 0,
	    .range = ubo_size,
	};

	VkWriteDescriptorSet write_descriptor_sets[3] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = src_binding,
	        .descriptorCount = image_count,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = src_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = target_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo = &target_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = ubo_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pBufferInfo = &buffer_info,
	    },
	};

	vk->vkUpdateDescriptorSets(            //
	    vk->device,                        //
	    ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	    write_descriptor_sets,             // pDescriptorWrites
	    0,                                 // descriptorCopyCount
	    NULL);                             // pDescriptorCopies
}

XRT_MAYBE_UNUSED static void
update_compute_shared_descriptor_set(struct vk_bundle *vk,
                                     uint32_t src_binding,
                                     VkSampler src_samplers[XRT_MAX_VIEWS],
                                     VkImageView src_image_views[XRT_MAX_VIEWS],
                                     uint32_t distortion_binding,
                                     VkSampler distortion_samplers[3 * XRT_MAX_VIEWS],
                                     VkImageView distortion_image_views[3 * XRT_MAX_VIEWS],
                                     uint32_t target_binding,
                                     VkImageView target_image_view,
                                     uint32_t ubo_binding,
                                     VkBuffer ubo_buffer,
                                     VkDeviceSize ubo_size,
                                     VkDescriptorSet descriptor_set,
                                     uint32_t view_count)
{
	VkDescriptorImageInfo src_image_info[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < view_count; ++i) {
		src_image_info[i].sampler = src_samplers[i];
		src_image_info[i].imageView = src_image_views[i];
		src_image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo distortion_image_info[3 * XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < 3 * view_count; ++i) {
		distortion_image_info[i].sampler = distortion_samplers[i];
		distortion_image_info[i].imageView = distortion_image_views[i];
		distortion_image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo target_image_info = {
	    .imageView = target_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkDescriptorBufferInfo buffer_info = {
	    .buffer = ubo_buffer,
	    .offset = 0,
	    .range = ubo_size,
	};

	VkWriteDescriptorSet write_descriptor_sets[4] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = src_binding,
	        .descriptorCount = view_count,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = src_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = distortion_binding,
	        .descriptorCount = 3 * view_count,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = distortion_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = target_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo = &target_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = ubo_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pBufferInfo = &buffer_info,
	    },
	};

	vk->vkUpdateDescriptorSets(            //
	    vk->device,                        //
	    ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	    write_descriptor_sets,             // pDescriptorWrites
	    0,                                 // descriptorCopyCount
	    NULL);                             // pDescriptorCopies
}

XRT_MAYBE_UNUSED static void
update_compute_descriptor_set_target(struct vk_bundle *vk,
                                     uint32_t target_binding,
                                     VkImageView target_image_view,
                                     uint32_t ubo_binding,
                                     VkBuffer ubo_buffer,
                                     VkDeviceSize ubo_size,
                                     VkDescriptorSet descriptor_set,
                                     uint32_t view_count)
{
	VkDescriptorImageInfo target_image_info = {
	    .imageView = target_image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	VkDescriptorBufferInfo buffer_info = {
	    .buffer = ubo_buffer,
	    .offset = 0,
	    .range = ubo_size,
	};

	VkWriteDescriptorSet write_descriptor_sets[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = target_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo = &target_image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = ubo_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pBufferInfo = &buffer_info,
	    },
	};

	vk->vkUpdateDescriptorSets(            //
	    vk->device,                        //
	    ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	    write_descriptor_sets,             // pDescriptorWrites
	    0,                                 // descriptorCopyCount
	    NULL);                             // pDescriptorCopies
}

static void
dispatch_project_pipeline(struct render_compute *render,
                          VkSampler src_samplers[XRT_MAX_VIEWS],
                          VkImageView src_image_views[XRT_MAX_VIEWS],
                          const struct xrt_normalized_rect src_norm_rects[XRT_MAX_VIEWS],
                          VkImage target_image,
                          VkImageView target_image_view,
                          const struct render_viewport_data views[XRT_MAX_VIEWS],
                          VkPipeline pipeline)
{
	struct vk_bundle *vk = vk_from_render(render);
	struct render_resources *r = render->r;


	/*
	 * UBO
	 */

	struct render_compute_distortion_ubo_data *data =
	    (struct render_compute_distortion_ubo_data *)r->compute.distortion.ubo.mapped;
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		data->views[i] = views[i];
		data->post_transforms[i] = src_norm_rects[i];
	}


	/*
	 * Source, target and distortion images.
	 */

	VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = VK_REMAINING_MIP_LEVELS,
	    .baseArrayLayer = 0,
	    .layerCount = VK_REMAINING_ARRAY_LAYERS,
	};

	vk_cmd_image_barrier_gpu_locked( //
	    vk,                          //
	    r->cmd,                      //
	    target_image,                //
	    0,                           //
	    VK_ACCESS_SHADER_WRITE_BIT,  //
	    VK_IMAGE_LAYOUT_UNDEFINED,   //
	    VK_IMAGE_LAYOUT_GENERAL,     //
	    subresource_range);          //

	VkSampler sampler = r->samplers.clamp_to_edge;
	VkSampler distortion_samplers[3 * XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		distortion_samplers[3 * i + 0] = sampler;
		distortion_samplers[3 * i + 1] = sampler;
		distortion_samplers[3 * i + 2] = sampler;
	}

	update_compute_shared_descriptor_set( //
	    vk,                               //
	    r->compute.src_binding,           //
	    src_samplers,                     //
	    src_image_views,                  //
	    r->compute.distortion_binding,    //
	    distortion_samplers,              //
	    r->distortion.image_views,        //
	    r->compute.target_binding,        //
	    target_image_view,                //
	    r->compute.ubo_binding,           //
	    r->compute.distortion.ubo.buffer, //
	    VK_WHOLE_SIZE,                    //
	    render->shared_descriptor_set,    //
	    render->r->view_count);           //

	vk->vkCmdBindPipeline(              //
	    r->cmd,                         //
	    VK_PIPELINE_BIND_POINT_COMPUTE, // pipelineBindPoint
	    pipeline);                      // pipeline

	vk->vkCmdBindDescriptorSets(               //
	    r->cmd,                                //
	    VK_PIPELINE_BIND_POINT_COMPUTE,        // pipelineBindPoint
	    r->compute.distortion.pipeline_layout, // layout
	    0,                                     // firstSet
	    1,                                     // descriptorSetCount
	    &render->shared_descriptor_set,        // pDescriptorSets
	    0,                                     // dynamicOffsetCount
	    NULL);                                 // pDynamicOffsets


	uint32_t w = 0, h = 0;
	calc_dispatch_dims_views(views, render->r->view_count, &w, &h);
	assert(w != 0 && h != 0);

	vk->vkCmdDispatch( //
	    r->cmd,        //
	    w,             // groupCountX
	    h,             // groupCountY
	    2);            // groupCountZ

#ifdef XRT_OS_OSX
	{
		struct render_buffer *buffer = &r->apple_target_debug.buffer;
		if (buffer->buffer != VK_NULL_HANDLE) {
			uint32_t target_width = 0;
			uint32_t target_height = 0;
			for (uint32_t i = 0; i < render->r->view_count; ++i) {
				target_width = target_width > views[i].x + views[i].w ? target_width : views[i].x + views[i].w;
				target_height =
				    target_height > views[i].y + views[i].h ? target_height : views[i].y + views[i].h;
			}

			if (target_width > 0 && target_height > 0) {
				const uint32_t max_x = target_width - 1;
				const uint32_t max_y = target_height - 1;
				const int32_t quarter_x = (int32_t)(target_width / 4 < max_x ? target_width / 4 : max_x);
				const int32_t center_x = (int32_t)(target_width / 2 < max_x ? target_width / 2 : max_x);
				const int32_t right_x = (int32_t)(((target_width * 3) / 4) < max_x ? (target_width * 3) / 4 : max_x);
				const int32_t sample_y = (int32_t)(target_height / 2 < max_y ? target_height / 2 : max_y);
				const VkImageSubresourceRange target_subresource_range = {
				    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				    .baseMipLevel = 0,
				    .levelCount = 1,
				    .baseArrayLayer = 0,
				    .layerCount = 1,
				};
				const VkBufferImageCopy copies[3] = {
				    {
				        .bufferOffset = 0,
				        .imageSubresource =
				            {
				                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				                .mipLevel = 0,
				                .baseArrayLayer = 0,
				                .layerCount = 1,
				            },
				        .imageOffset = {.x = quarter_x, .y = sample_y, .z = 0},
				        .imageExtent = {.width = 1, .height = 1, .depth = 1},
				    },
				    {
				        .bufferOffset = 4,
				        .imageSubresource =
				            {
				                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				                .mipLevel = 0,
				                .baseArrayLayer = 0,
				                .layerCount = 1,
				            },
				        .imageOffset = {.x = center_x, .y = sample_y, .z = 0},
				        .imageExtent = {.width = 1, .height = 1, .depth = 1},
				    },
				    {
				        .bufferOffset = 8,
				        .imageSubresource =
				            {
				                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				                .mipLevel = 0,
				                .baseArrayLayer = 0,
				                .layerCount = 1,
				            },
				        .imageOffset = {.x = right_x, .y = sample_y, .z = 0},
				        .imageExtent = {.width = 1, .height = 1, .depth = 1},
				    },
				};
				const VkBufferMemoryBarrier buffer_barrier = {
				    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				    .buffer = buffer->buffer,
				    .offset = 0,
				    .size = VK_WHOLE_SIZE,
				};

				vk_cmd_image_barrier_gpu_locked(          //
				    vk,                                   //
				    r->cmd,                               //
				    target_image,                         //
				    VK_ACCESS_SHADER_WRITE_BIT,           //
				    VK_ACCESS_TRANSFER_READ_BIT,          //
				    VK_IMAGE_LAYOUT_GENERAL,              //
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, //
				    target_subresource_range);            //

				vk->vkCmdCopyImageToBuffer( //
				    r->cmd,                  //
				    target_image,            //
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    buffer->buffer,
				    ARRAY_SIZE(copies),
				    copies);

				vk->vkCmdPipelineBarrier(          //
				    r->cmd,                        //
				    VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_HOST_BIT,
				    0,
				    0,
				    NULL,
				    1,
				    &buffer_barrier,
				    0,
				    NULL);

				vk_cmd_image_barrier_gpu_locked(      //
				    vk,                                //
				    r->cmd,                            //
				    target_image,                      //
				    VK_ACCESS_TRANSFER_READ_BIT,       //
				    VK_ACCESS_MEMORY_READ_BIT,         //
				    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,   //
				    target_subresource_range);         //

				r->apple_target_debug.pending = true;
			}
		}
	}
#else
	VkImageMemoryBarrier memoryBarrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = target_image,
	    .subresourceRange = subresource_range,
	};

	vk->vkCmdPipelineBarrier(                 //
	    r->cmd,                               //
	    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, //
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,    //
	    0,                                    //
	    0,                                    //
	    NULL,                                 //
	    0,                                    //
	    NULL,                                 //
	    1,                                    //
	    &memoryBarrier);                      //
#endif
}


/*
 *
 * 'Exported' functions.
 *
 */

bool
render_compute_init(struct render_compute *render, struct render_resources *r)
{
	VkResult ret;

	assert(render->r == NULL);

	struct vk_bundle *vk = r->vk;
	render->r = r;

	for (uint32_t i = 0; i < RENDER_MAX_LAYER_RUNS_COUNT(r); i++) {
		ret = vk_create_descriptor_set(             //
		    vk,                                     // vk_bundle
		    r->compute.descriptor_pool,             // descriptor_pool
		    r->compute.layer.descriptor_set_layout, // descriptor_set_layout
		    &render->layer_descriptor_sets[i]);     // descriptor_set
		VK_CHK_WITH_RET(ret, "vk_create_descriptor_set", false);

		VK_NAME_DESCRIPTOR_SET(vk, render->layer_descriptor_sets[i], "render_compute layer descriptor set");
	}

	ret = vk_create_descriptor_set(                  //
	    vk,                                          // vk_bundle
	    r->compute.descriptor_pool,                  // descriptor_pool
	    r->compute.distortion.descriptor_set_layout, // descriptor_set_layout
	    &render->shared_descriptor_set);             // descriptor_set
	VK_CHK_WITH_RET(ret, "vk_create_descriptor_set", false);

	VK_NAME_DESCRIPTOR_SET(vk, render->shared_descriptor_set, "render_compute shared descriptor set");

	return true;
}

bool
render_compute_begin(struct render_compute *render)
{
	VkResult ret;
	struct vk_bundle *vk = vk_from_render(render);

	ret = vk->vkResetCommandPool(vk->device, render->r->cmd_pool, 0);
	VK_CHK_WITH_RET(ret, "vkResetCommandPool", false);

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	ret = vk->vkBeginCommandBuffer( //
	    render->r->cmd,             //
	    &begin_info);               //
	VK_CHK_WITH_RET(ret, "vkBeginCommandBuffer", false);

	vk->vkCmdResetQueryPool(   //
	    render->r->cmd,        //
	    render->r->query_pool, //
	    0,                     // firstQuery
	    2);                    // queryCount

	vk->vkCmdWriteTimestamp(               //
	    render->r->cmd,                    //
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // pipelineStage
	    render->r->query_pool,             //
	    0);                                // query

	return true;
}

bool
render_compute_end(struct render_compute *render)
{
	struct vk_bundle *vk = vk_from_render(render);
	VkResult ret;

	vk->vkCmdWriteTimestamp(                  //
	    render->r->cmd,                       //
	    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // pipelineStage
	    render->r->query_pool,                //
	    1);                                   // query

	ret = vk->vkEndCommandBuffer(render->r->cmd);
	VK_CHK_WITH_RET(ret, "vkEndCommandBuffer", false);

	return true;
}

void
render_compute_fini(struct render_compute *render)
{
	assert(render->r != NULL);

	struct vk_bundle *vk = vk_from_render(render);

	// Reclaimed by vkResetDescriptorPool.
	render->shared_descriptor_set = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < ARRAY_SIZE(render->layer_descriptor_sets); i++) {
		render->layer_descriptor_sets[i] = VK_NULL_HANDLE;
	}

	vk->vkResetDescriptorPool(vk->device, render->r->compute.descriptor_pool, 0);

	render->r = NULL;
}

void
render_compute_layers(struct render_compute *render,
                      VkDescriptorSet descriptor_set,
                      VkBuffer ubo,
                      VkSampler src_samplers[RENDER_MAX_IMAGES_SIZE],
                      VkImageView src_image_views[RENDER_MAX_IMAGES_SIZE],
                      uint32_t num_srcs,
                      VkImageView target_image_view,
                      const struct render_viewport_data *view,
                      bool do_timewarp)
{
	assert(render->r != NULL);

	struct vk_bundle *vk = vk_from_render(render);
	struct render_resources *r = render->r;


	/*
	 * Source, target and distortion images.
	 */

	update_compute_layer_descriptor_set( //
	    vk,                              //
	    r->compute.src_binding,          //
	    src_samplers,                    //
	    src_image_views,                 //
	    num_srcs,                        //
	    r->compute.target_binding,       //
	    target_image_view,               //
	    r->compute.ubo_binding,          //
	    ubo,                             //
	    VK_WHOLE_SIZE,                   //
	    descriptor_set);                 //

	VkPipeline pipeline = do_timewarp ? r->compute.layer.timewarp_pipeline : r->compute.layer.non_timewarp_pipeline;
	vk->vkCmdBindPipeline(              //
	    render->r->cmd,                 //
	    VK_PIPELINE_BIND_POINT_COMPUTE, // pipelineBindPoint
	    pipeline);                      // pipeline

	vk->vkCmdBindDescriptorSets(          //
	    r->cmd,                           //
	    VK_PIPELINE_BIND_POINT_COMPUTE,   // pipelineBindPoint
	    r->compute.layer.pipeline_layout, // layout
	    0,                                // firstSet
	    1,                                // descriptorSetCount
	    &descriptor_set,                  // pDescriptorSets
	    0,                                // dynamicOffsetCount
	    NULL);                            // pDynamicOffsets


	uint32_t w = 0, h = 0;
	calc_dispatch_dims_1_view(*view, &w, &h);
	assert(w != 0 && h != 0);

	vk->vkCmdDispatch( //
	    r->cmd,        //
	    w,             // groupCountX
	    h,             // groupCountY
	    1);            // groupCountZ
}

void
render_compute_projection_timewarp(struct render_compute *render,
                                   VkSampler src_samplers[XRT_MAX_VIEWS],
                                   VkImageView src_image_views[XRT_MAX_VIEWS],
                                   const struct xrt_normalized_rect src_norm_rects[XRT_MAX_VIEWS],
                                   const struct xrt_pose src_poses[XRT_MAX_VIEWS],
                                   const struct xrt_fov src_fovs[XRT_MAX_VIEWS],
                                   const struct xrt_pose new_poses_scanout_begin[XRT_MAX_VIEWS],
                                   const struct xrt_pose new_poses_scanout_end[XRT_MAX_VIEWS],
                                   VkImage target_image,
                                   VkImageView target_image_view,
                                   const struct render_viewport_data views[XRT_MAX_VIEWS])
{
	assert(render->r != NULL);
	struct render_resources *r = render->r;


	/*
	 * UBO
	 */

	struct xrt_matrix_4x4 time_warp_matrix_scanout_begin[XRT_MAX_VIEWS];
	struct xrt_matrix_4x4 time_warp_matrix_scanout_end[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		render_calc_time_warp_matrix(            //
		    &src_poses[i],                       //
		    &src_fovs[i],                        //
		    &new_poses_scanout_begin[i],         //
		    &time_warp_matrix_scanout_begin[i]); //

		render_calc_time_warp_matrix(          //
		    &src_poses[i],                     //
		    &src_fovs[i],                      //
		    &new_poses_scanout_end[i],         //
		    &time_warp_matrix_scanout_end[i]); //
	}

	const bool force_timewarp_identity = debug_get_bool_option_force_timewarp_identity();
	const bool force_timewarp_pretransform_identity =
	    debug_get_bool_option_force_timewarp_pretransform_identity();
	const struct xrt_normalized_rect identity_pre_transform = timewarp_identity_pre_transform();
	struct render_compute_distortion_ubo_data *data =
	    (struct render_compute_distortion_ubo_data *)r->compute.distortion.ubo.mapped;
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		if (force_timewarp_identity) {
			math_matrix_4x4_identity(&time_warp_matrix_scanout_begin[i]);
			math_matrix_4x4_identity(&time_warp_matrix_scanout_end[i]);
		}

		data->views[i] = views[i];
		data->pre_transforms[i] =
		    force_timewarp_pretransform_identity ? identity_pre_transform : r->distortion.uv_to_tanangle[i];
		data->transform_timewarp_scanout_begin[i] = time_warp_matrix_scanout_begin[i];
		data->transform_timewarp_scanout_end[i] = time_warp_matrix_scanout_end[i];
		data->post_transforms[i] = src_norm_rects[i];

#ifdef XRT_OS_OSX
		maybe_log_timewarp_inputs(r->apple_target_debug.frame_id, i, &data->pre_transforms[i],
		                          &data->post_transforms[i], &data->transform_timewarp_scanout_begin[i],
		                          &data->transform_timewarp_scanout_end[i]);
#endif
	}

	dispatch_project_pipeline(render, src_samplers, src_image_views, src_norm_rects, target_image,
	                          target_image_view, views, r->compute.distortion.timewarp_pipeline);
}


/*
 * This function is intended to be used on content already timewarped to new_poses_scanout_begin.
 * It performs only the timewarp nesscary to compensate for the time delta between the start and end of
 * scanout.
 */
void
render_compute_projection_scanout_compensation(struct render_compute *render,
                                               VkSampler src_samplers[XRT_MAX_VIEWS],
                                               VkImageView src_image_views[XRT_MAX_VIEWS],
                                               const struct xrt_normalized_rect src_rects[XRT_MAX_VIEWS],
                                               const struct xrt_fov src_fovs[XRT_MAX_VIEWS],
                                               const struct xrt_pose new_poses_scanout_begin[XRT_MAX_VIEWS],
                                               const struct xrt_pose new_poses_scanout_end[XRT_MAX_VIEWS],
                                               VkImage target_image,
                                               VkImageView target_image_view,
                                               const struct render_viewport_data views[XRT_MAX_VIEWS])
{
	assert(render->r != NULL);
	struct render_resources *r = render->r;


	/*
	 * UBO
	 */

	struct xrt_matrix_4x4 time_warp_matrix_scanout_begin[XRT_MAX_VIEWS];
	struct xrt_matrix_4x4 time_warp_matrix_scanout_end[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		render_calc_time_warp_projection(&src_fovs[i], &time_warp_matrix_scanout_begin[i]);

		render_calc_time_warp_matrix(          //
		    &new_poses_scanout_begin[i],       //
		    &src_fovs[i],                      //
		    &new_poses_scanout_end[i],         //
		    &time_warp_matrix_scanout_end[i]); //
	}

	const bool force_timewarp_identity = debug_get_bool_option_force_timewarp_identity();
	const bool force_timewarp_pretransform_identity =
	    debug_get_bool_option_force_timewarp_pretransform_identity();
	const struct xrt_normalized_rect identity_pre_transform = timewarp_identity_pre_transform();
	struct render_compute_distortion_ubo_data *data =
	    (struct render_compute_distortion_ubo_data *)r->compute.distortion.ubo.mapped;
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		if (force_timewarp_identity) {
			math_matrix_4x4_identity(&time_warp_matrix_scanout_begin[i]);
			math_matrix_4x4_identity(&time_warp_matrix_scanout_end[i]);
		}

		data->views[i] = views[i];
		data->pre_transforms[i] =
		    force_timewarp_pretransform_identity ? identity_pre_transform : r->distortion.uv_to_tanangle[i];
		data->transform_timewarp_scanout_begin[i] = time_warp_matrix_scanout_begin[i];
		data->transform_timewarp_scanout_end[i] = time_warp_matrix_scanout_end[i];
		data->post_transforms[i] = src_rects[i];

#ifdef XRT_OS_OSX
		maybe_log_timewarp_inputs(r->apple_target_debug.frame_id, i, &data->pre_transforms[i],
		                          &data->post_transforms[i], &data->transform_timewarp_scanout_begin[i],
		                          &data->transform_timewarp_scanout_end[i]);
#endif
	}

	dispatch_project_pipeline(render, src_samplers, src_image_views, src_rects, target_image, target_image_view,
	                          views, r->compute.distortion.timewarp_pipeline);
}

void
render_compute_projection_no_timewarp(struct render_compute *render,
                                      VkSampler src_samplers[XRT_MAX_VIEWS],
                                      VkImageView src_image_views[XRT_MAX_VIEWS],
                                      const struct xrt_normalized_rect src_rects[XRT_MAX_VIEWS],
                                      VkImage target_image,
                                      VkImageView target_image_view,
                                      const struct render_viewport_data views[XRT_MAX_VIEWS])
{
	assert(render->r != NULL);
	struct render_resources *r = render->r;

	dispatch_project_pipeline(render, src_samplers, src_image_views, src_rects, target_image, target_image_view,
	                          views, r->compute.distortion.pipeline);
}

void
render_compute_clear(struct render_compute *render,
                     VkImage target_image,
                     VkImageView target_image_view,
                     const struct render_viewport_data views[XRT_MAX_VIEWS])
{
	assert(render->r != NULL);

	struct vk_bundle *vk = vk_from_render(render);
	struct render_resources *r = render->r;


	/*
	 * UBO
	 */

	// Calculate transforms.
	struct xrt_matrix_4x4 transforms[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < render->r->view_count; i++) {
		math_matrix_4x4_identity(&transforms[i]);
	}

	struct render_compute_distortion_ubo_data *data =
	    (struct render_compute_distortion_ubo_data *)r->compute.clear.ubo.mapped;
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		data->views[i] = views[i];
	}

	/*
	 * Source, target and distortion images.
	 */

	VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = VK_REMAINING_MIP_LEVELS,
	    .baseArrayLayer = 0,
	    .layerCount = VK_REMAINING_ARRAY_LAYERS,
	};

	vk_cmd_image_barrier_gpu_locked( //
	    vk,                          //
	    r->cmd,                      //
	    target_image,                //
	    0,                           //
	    VK_ACCESS_SHADER_WRITE_BIT,  //
	    VK_IMAGE_LAYOUT_UNDEFINED,   //
	    VK_IMAGE_LAYOUT_GENERAL,     //
	    subresource_range);          //

	VkSampler sampler = r->samplers.mock;
	VkSampler src_samplers[XRT_MAX_VIEWS];
	VkImageView src_image_views[XRT_MAX_VIEWS];
	VkSampler distortion_samplers[3 * XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < render->r->view_count; ++i) {
		src_samplers[i] = sampler;
		src_image_views[i] = r->mock.color.image_view;
		distortion_samplers[3 * i + 0] = sampler;
		distortion_samplers[3 * i + 1] = sampler;
		distortion_samplers[3 * i + 2] = sampler;
	}

	update_compute_shared_descriptor_set( //
	    vk,                               //
	    r->compute.src_binding,           //
	    src_samplers,                     //
	    src_image_views,                  //
	    r->compute.distortion_binding,    //
	    distortion_samplers,              //
	    r->distortion.image_views,        //
	    r->compute.target_binding,        //
	    target_image_view,                //
	    r->compute.ubo_binding,           //
	    r->compute.clear.ubo.buffer,      //
	    VK_WHOLE_SIZE,                    // ubo_size
	    render->shared_descriptor_set,    // descriptor_set
	    render->r->view_count);           //

	vk->vkCmdBindPipeline(              //
	    r->cmd,                         //
	    VK_PIPELINE_BIND_POINT_COMPUTE, // pipelineBindPoint
	    r->compute.clear.pipeline);     // pipeline

	vk->vkCmdBindDescriptorSets(               //
	    r->cmd,                                //
	    VK_PIPELINE_BIND_POINT_COMPUTE,        // pipelineBindPoint
	    r->compute.distortion.pipeline_layout, // layout
	    0,                                     // firstSet
	    1,                                     // descriptorSetCount
	    &render->shared_descriptor_set,        // pDescriptorSets
	    0,                                     // dynamicOffsetCount
	    NULL);                                 // pDynamicOffsets


	uint32_t w = 0, h = 0;
	calc_dispatch_dims_views(views, render->r->view_count, &w, &h);
	assert(w != 0 && h != 0);

	vk->vkCmdDispatch( //
	    r->cmd,        //
	    w,             // groupCountX
	    h,             // groupCountY
	    2);            // groupCountZ

	VkImageMemoryBarrier memoryBarrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = target_image,
	    .subresourceRange = subresource_range,
	};

	vk->vkCmdPipelineBarrier(                 //
	    r->cmd,                               //
	    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, //
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,    //
	    0,                                    //
	    0,                                    //
	    NULL,                                 //
	    0,                                    //
	    NULL,                                 //
	    1,                                    //
	    &memoryBarrier);                      //
}
