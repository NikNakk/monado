// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Offscreen Vulkan render benchmark: same workload on whichever ICD the loader picks.
// Usage: kkbench <alu|tex> <iters> <width> <height> <frames> <inflight>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_);                               \
			exit(2);                                                                                       \
		}                                                                                                      \
	} while (0)

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int
cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}
static double
pct(double *v, int n, double p)
{
	qsort(v, n, sizeof *v, cmpd);
	return v[(int)(p * (n - 1))];
}

static VkDevice dev;
static VkPhysicalDevice pd;

static uint32_t *
load(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *d = malloc(*size);
	fread(d, 1, *size, f);
	fclose(f);
	return d;
}
static VkShaderModule
module(const char *path)
{
	size_t n;
	uint32_t *code = load(path, &n);
	VkShaderModule m;
	CHECK(vkCreateShaderModule(dev,
	                           &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                                       .codeSize = n,
	                                                       .pCode = code},
	                           NULL, &m));
	free(code);
	return m;
}
static uint32_t
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(2);
}
static VkImage
image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageView *view)
{
	VkImage img;
	VkDeviceMemory mem;
	VkMemoryRequirements req;
	CHECK(vkCreateImage(dev,
	                    &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                                         .imageType = VK_IMAGE_TYPE_2D,
	                                         .format = fmt,
	                                         .extent = {w, h, 1},
	                                         .mipLevels = 1,
	                                         .arrayLayers = 1,
	                                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                                         .tiling = VK_IMAGE_TILING_OPTIMAL,
	                                         .usage = usage},
	                    NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &req);
	CHECK(vkAllocateMemory(dev,
	                       &(VkMemoryAllocateInfo){
	                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                           .allocationSize = req.size,
	                           .memoryTypeIndex = memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)},
	                       NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	CHECK(vkCreateImageView(dev,
	                        &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                                                 .image = img,
	                                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                                                 .format = fmt,
	                                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	                        NULL, view));
	return img;
}

struct pc
{
	uint32_t iters, frame;
};

static VkPipeline
pipeline(VkRenderPass rp, VkPipelineLayout layout, const char *frag)
{
	VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                          .stage = VK_SHADER_STAGE_VERTEX_BIT,
	                                          .module = module("fs.vert.spv"),
	                                          .pName = "main"},
	                                         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	                                          .module = module(frag),
	                                          .pName = "main"}};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
	VkPipeline p;
	CHECK(vkCreateGraphicsPipelines(
	    dev, VK_NULL_HANDLE, 1,
	    &(VkGraphicsPipelineCreateInfo){
	        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	        .stageCount = 2,
	        .pStages = st,
	        .pVertexInputState =
	            &(VkPipelineVertexInputStateCreateInfo){
	                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
	        .pInputAssemblyState =
	            &(VkPipelineInputAssemblyStateCreateInfo){
	                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
	        .pViewportState =
	            &(VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		                                         .viewportCount = 1,
		                                         .scissorCount = 1},
	        .pRasterizationState =
	            &(VkPipelineRasterizationStateCreateInfo){
	                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1},
	        .pMultisampleState =
	            &(VkPipelineMultisampleStateCreateInfo){
	                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
	        .pColorBlendState =
	            &(VkPipelineColorBlendStateCreateInfo){.sType =
		                                               VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		                                           .attachmentCount = 1,
		                                           .pAttachments = &cba},
	        .pDynamicState =
	            &(VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		                                        .dynamicStateCount = 2,
		                                        .pDynamicStates = dyn},
	        .layout = layout,
	        .renderPass = rp},
	    NULL, &p));
	return p;
}

static void
draw(VkCommandBuffer cb,
     VkRenderPass rp,
     VkFramebuffer fb,
     VkPipeline p,
     VkPipelineLayout layout,
     VkDescriptorSet ds,
     uint32_t w,
     uint32_t h,
     struct pc pcv)
{
	vkCmdBeginRenderPass(cb,
	                     &(VkRenderPassBeginInfo){.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	                                              .renderPass = rp,
	                                              .framebuffer = fb,
	                                              .renderArea = {{0, 0}, {w, h}}},
	                     VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	if (ds)
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, NULL);
	vkCmdSetViewport(cb, 0, 1, &(VkViewport){0, 0, w, h, 0, 1});
	// Same pixels either way: split the target into horizontal strips, one draw each.
	uint32_t draws = getenv("KKB_DRAWS") ? atoi(getenv("KKB_DRAWS")) : 1;
	for (uint32_t i = 0; i < draws; i++) {
		uint32_t y0 = h * i / draws, y1 = h * (i + 1) / draws;
		vkCmdSetScissor(cb, 0, 1, &(VkRect2D){{0, y0}, {w, y1 - y0}});
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcv, &pcv);
		vkCmdDraw(cb, 3, 1, 0, 0);
	}
	vkCmdEndRenderPass(cb);
}

int
main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "usage: kkbench <alu|tex> iters w h frames inflight\n");
		return 1;
	}
	int tex = !strcmp(argv[1], "tex");
	uint32_t iters = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
	int frames = atoi(argv[5]), inflight = atoi(argv[6]), warm = 30;

	const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
	VkInstance inst;
	CHECK(vkCreateInstance(
	    &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	                            .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
	                            .enabledExtensionCount = 1,
	                            .ppEnabledExtensionNames = iext,
	                            .pApplicationInfo =
	                                &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                                                     .apiVersion = VK_API_VERSION_1_2}},
	    NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (!n) {
		fprintf(stderr, "no device\n");
		return 2;
	}
	VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
	vkGetPhysicalDeviceProperties2(pd, &props);
	double tsp = props.properties.limits.timestampPeriod;

	uint32_t en = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
	VkExtensionProperties *ep = calloc(en, sizeof *ep);
	vkEnumerateDeviceExtensionProperties(pd, NULL, &en, ep);
	const char *dext[1];
	uint32_t dn = 0;
	for (uint32_t i = 0; i < en; i++)
		if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset"))
			dext[dn++] = "VK_KHR_portability_subset";
	CHECK(vkCreateDevice(
	    pd,
	    &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	                          .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos =
	                              &(VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                                                         .queueCount = 1,
	                                                         .pQueuePriorities = (float[]){1}},
	                          .enabledExtensionCount = dn,
	                          .ppEnabledExtensionNames = dext},
	    NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(
	    dev,
	    &(VkRenderPassCreateInfo){
	        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	        .attachmentCount = 1,
	        .pAttachments = &(VkAttachmentDescription){.format = fmt,
		                                           .samples = VK_SAMPLE_COUNT_1_BIT,
		                                           .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		                                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                                           .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	        .subpassCount = 1,
	        .pSubpasses =
	            &(VkSubpassDescription){.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		                            .colorAttachmentCount = 1,
		                            .pColorAttachments =
		                                &(VkAttachmentReference){0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}},
	        .dependencyCount = 1,
	        .pDependencies =
	            &(VkSubpassDependency){VK_SUBPASS_EXTERNAL, 0,
		                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
		                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
		                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0}},
	    NULL, &rp));

	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(
	    dev,
	    &(VkDescriptorSetLayoutCreateInfo){
	        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	        .bindingCount = 1,
	        .pBindings = &(VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		                                             VK_SHADER_STAGE_FRAGMENT_BIT}},
	    NULL, &dsl));
	VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc)};
	VkPipelineLayout pl_alu, pl_tex;
	CHECK(
	    vkCreatePipelineLayout(dev,
	                           &(VkPipelineLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                                         .pushConstantRangeCount = 1,
	                                                         .pPushConstantRanges = &pcr},
	                           NULL, &pl_alu));
	CHECK(
	    vkCreatePipelineLayout(dev,
	                           &(VkPipelineLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                                         .setLayoutCount = 1,
	                                                         .pSetLayouts = &dsl,
	                                                         .pushConstantRangeCount = 1,
	                                                         .pPushConstantRanges = &pcr},
	                           NULL, &pl_tex));
	double t0 = now_ms();
	VkPipeline p_alu = pipeline(rp, pl_alu, "alu.frag.spv");
	VkPipeline p_tex = tex ? pipeline(rp, pl_tex, "tex.frag.spv") : VK_NULL_HANDLE;
	double compile_ms = now_ms() - t0;

	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageView srcv;
	image(w, h, fmt, usage, &srcv);
	VkFramebuffer srcfb;
	CHECK(vkCreateFramebuffer(dev,
	                          &(VkFramebufferCreateInfo){.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	                                                     .renderPass = rp,
	                                                     .attachmentCount = 1,
	                                                     .pAttachments = &srcv,
	                                                     .width = w,
	                                                     .height = h,
	                                                     .layers = 1},
	                          NULL, &srcfb));
	VkFramebuffer *fb = calloc(inflight, sizeof *fb);
	for (int i = 0; i < inflight; i++) {
		VkImageView v;
		image(w, h, fmt, usage, &v);
		CHECK(vkCreateFramebuffer(dev,
		                          &(VkFramebufferCreateInfo){.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		                                                     .renderPass = rp,
		                                                     .attachmentCount = 1,
		                                                     .pAttachments = &v,
		                                                     .width = w,
		                                                     .height = h,
		                                                     .layers = 1},
		                          NULL, &fb[i]));
	}
	VkSampler smp;
	CHECK(vkCreateSampler(dev,
	                      &(VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                                             .magFilter = VK_FILTER_LINEAR,
	                                             .minFilter = VK_FILTER_LINEAR,
	                                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
	                      NULL, &smp));
	VkDescriptorPool dp;
	VkDescriptorSet ds;
	CHECK(vkCreateDescriptorPool(
	    dev,
	    &(VkDescriptorPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                  .maxSets = 1,
	                                  .poolSizeCount = 1,
	                                  .pPoolSizes =
	                                      &(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
	    NULL, &dp));
	CHECK(vkAllocateDescriptorSets(
	    dev,
	    &(VkDescriptorSetAllocateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                   .descriptorPool = dp,
	                                   .descriptorSetCount = 1,
	                                   .pSetLayouts = &dsl},
	    &ds));
	vkUpdateDescriptorSets(
	    dev, 1,
	    &(VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	                            .dstSet = ds,
	                            .descriptorCount = 1,
	                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	                            .pImageInfo =
	                                &(VkDescriptorImageInfo){smp, srcv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
	    0, NULL);

	VkCommandPool cp;
	CHECK(vkCreateCommandPool(dev,
	                          &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                                                     .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT},
	                          NULL, &cp));
	VkCommandBuffer *cb = calloc(inflight, sizeof *cb);
	CHECK(vkAllocateCommandBuffers(
	    dev,
	    &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = cp,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = inflight},
	    cb));
	VkFence *fence = calloc(inflight, sizeof *fence);
	for (int i = 0; i < inflight; i++)
		CHECK(vkCreateFence(dev,
		                    &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		                                         .flags = VK_FENCE_CREATE_SIGNALED_BIT},
		                    NULL, &fence[i]));
	VkQueryPool qp;
	CHECK(vkCreateQueryPool(dev,
	                        &(VkQueryPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	                                                 .queryType = VK_QUERY_TYPE_TIMESTAMP,
	                                                 .queryCount = 2 * inflight},
	                        NULL, &qp));

	// Fill the source texture with a pattern.
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	CHECK(vkResetFences(dev, 1, &fence[0]));
	CHECK(vkBeginCommandBuffer(cb[0], &bi));
	vkCmdResetQueryPool(cb[0], qp, 0, 2 * inflight);
	draw(cb[0], rp, srcfb, p_alu, pl_alu, VK_NULL_HANDLE, w, h, (struct pc){4, 0});
	CHECK(vkEndCommandBuffer(cb[0]));
	CHECK(vkQueueSubmit(
	    q, 1,
	    &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb[0]},
	    fence[0]));
	CHECK(vkWaitForFences(dev, 1, &fence[0], VK_TRUE, UINT64_MAX));

	double *gpu = calloc(frames, sizeof *gpu), *cpu = calloc(frames, sizeof *cpu);
	int ngpu = 0, ncpu = 0, *used = calloc(inflight, sizeof *used);
	double start = 0;
	for (int f = 0; f < frames + warm; f++) {
		int s = f % inflight;
		CHECK(vkWaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX));
		if (used[s] > warm) {
			uint64_t ts[2];
			CHECK(vkGetQueryPoolResults(dev, qp, 2 * s, 2, sizeof ts, ts, sizeof ts[0],
			                            VK_QUERY_RESULT_64_BIT));
			gpu[ngpu++] = (ts[1] - ts[0]) * tsp / 1e6;
		}
		if (f == warm)
			start = now_ms();
		double c0 = now_ms();
		CHECK(vkResetFences(dev, 1, &fence[s]));
		CHECK(vkBeginCommandBuffer(cb[s], &bi));
		vkCmdResetQueryPool(cb[s], qp, 2 * s, 2);
		vkCmdWriteTimestamp(cb[s], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2 * s);
		draw(cb[s], rp, fb[s], tex ? p_tex : p_alu, tex ? pl_tex : pl_alu, tex ? ds : VK_NULL_HANDLE, w, h,
		     (struct pc){iters, f});
		vkCmdWriteTimestamp(cb[s], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * s + 1);
		CHECK(vkEndCommandBuffer(cb[s]));
		CHECK(vkQueueSubmit(q, 1,
		                    &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                                    .commandBufferCount = 1,
		                                    .pCommandBuffers = &cb[s]},
		                    fence[s]));
		if (f >= warm)
			cpu[ncpu++] = (now_ms() - c0) * 1e3;
		used[s] = f + 1;
	}
	CHECK(vkQueueWaitIdle(q));
	double wall = (now_ms() - start) / frames;
	printf(
	    "{\"driver\":\"%s\",\"shader\":\"%s\",\"iters\":%u,\"w\":%u,\"h\":%u,\"frames\":%d,\"inflight\":%d,"
	    "\"wall_ms_per_frame\":%.3f,\"gpu_ms_median\":%.3f,\"gpu_ms_p95\":%.3f,\"gpu_samples\":%d,"
	    "\"gpu_ms_p99\":%.3f,\"cpu_record_submit_us_p99\":%.1f,\"draws\":%s,\"cpu_record_submit_us_median\":%.1f,"
	    "\"cpu_record_submit_us_p95\":%.1f,\"pipeline_compile_ms\":%.1f}\n",
	    drv.driverName, argv[1], iters, w, h, frames, inflight, wall, pct(gpu, ngpu, 0.5), pct(gpu, ngpu, 0.95),
	    ngpu, pct(gpu, ngpu, 0.99), pct(cpu, ncpu, 0.99), getenv("KKB_DRAWS") ? getenv("KKB_DRAWS") : "1",
	    pct(cpu, ncpu, 0.5), pct(cpu, ncpu, 0.95), compile_ms);
	return 0;
}
