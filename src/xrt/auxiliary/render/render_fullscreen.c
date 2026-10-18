// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Full-screen fragment pass into one color attachment. No platform policy. */
#include "render/render_fullscreen.h"

VkResult
render_fullscreen_init(struct vk_bundle *vk,
                       struct render_fullscreen *rf,
                       VkShaderModule vert,
                       VkShaderModule frag,
                       VkDescriptorSetLayout set_layout,
                       VkFormat format,
                       VkImageLayout final_layout)
{
	VkAttachmentDescription attachment = {
	    .format = format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, // Every pixel is written.
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = final_layout,
	};
	VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &reference,
	};
	// Order the layout transition after the presentation engine (acquire wait)
	// and earlier writes; make the result visible to whatever follows.
	VkSubpassDependency dependencies[2] = {
	    {
	        .srcSubpass = VK_SUBPASS_EXTERNAL,
	        .dstSubpass = 0,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    },
	    {
	        .srcSubpass = 0,
	        .dstSubpass = VK_SUBPASS_EXTERNAL,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
	    },
	};
	VkRenderPassCreateInfo pass_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	    .dependencyCount = 2,
	    .pDependencies = dependencies,
	};
	VkResult ret = vk->vkCreateRenderPass(vk->device, &pass_info, NULL, &rf->render_pass);
	if (ret != VK_SUCCESS)
		return ret;

	VkPipelineLayoutCreateInfo layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &set_layout,
	};
	ret = vk->vkCreatePipelineLayout(vk->device, &layout_info, NULL, &rf->pipeline_layout);
	if (ret != VK_SUCCESS)
		return ret;

	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = frag,
	        .pName = "main",
	    },
	};
	VkPipelineVertexInputStateCreateInfo vertex_input = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_attachment = {
	    .blendEnable = VK_FALSE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                      VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_attachment,
	};
	VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dynamic_states,
	};
	VkGraphicsPipelineCreateInfo pipeline_info = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vertex_input,
	    .pInputAssemblyState = &input_assembly,
	    .pViewportState = &viewport,
	    .pRasterizationState = &rasterization,
	    .pMultisampleState = &multisample,
	    .pColorBlendState = &blend,
	    .pDynamicState = &dynamic,
	    .layout = rf->pipeline_layout,
	    .renderPass = rf->render_pass,
	    .subpass = 0,
	};
	return vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &rf->pipeline);
}

VkResult
render_fullscreen_framebuffer(struct vk_bundle *vk,
                              const struct render_fullscreen *rf,
                              VkImageView view,
                              VkExtent2D extent,
                              VkFramebuffer *out_framebuffer)
{
	VkFramebufferCreateInfo info = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = rf->render_pass,
	    .attachmentCount = 1,
	    .pAttachments = &view,
	    .width = extent.width,
	    .height = extent.height,
	    .layers = 1,
	};
	return vk->vkCreateFramebuffer(vk->device, &info, NULL, out_framebuffer);
}

void
render_fullscreen_draw(struct vk_bundle *vk,
                       const struct render_fullscreen *rf,
                       VkCommandBuffer cmd,
                       VkFramebuffer framebuffer,
                       VkExtent2D extent,
                       VkDescriptorSet descriptors)
{
	VkRect2D area = {{0, 0}, extent};
	VkRenderPassBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = rf->render_pass,
	    .framebuffer = framebuffer,
	    .renderArea = area,
	};
	vk->vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport viewport = {0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
	vk->vkCmdSetViewport(cmd, 0, 1, &viewport);
	vk->vkCmdSetScissor(cmd, 0, 1, &area);
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rf->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rf->pipeline_layout, 0, 1, &descriptors, 0,
	                            NULL);
	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

void
render_fullscreen_fini(struct vk_bundle *vk, struct render_fullscreen *rf)
{
	if (rf->pipeline)
		vk->vkDestroyPipeline(vk->device, rf->pipeline, NULL);
	if (rf->pipeline_layout)
		vk->vkDestroyPipelineLayout(vk->device, rf->pipeline_layout, NULL);
	if (rf->render_pass)
		vk->vkDestroyRenderPass(vk->device, rf->render_pass, NULL);
	rf->pipeline = VK_NULL_HANDLE;
	rf->pipeline_layout = VK_NULL_HANDLE;
	rf->render_pass = VK_NULL_HANDLE;
}
