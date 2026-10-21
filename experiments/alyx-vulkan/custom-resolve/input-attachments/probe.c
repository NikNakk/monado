// Custom resolve probe shaped like native Alyx's stereo pass (from Codex's
// resolve-observe capture): 2-sample, 2-layer multiview (viewMask 3,
// layerCount 1), colour resolved by a shader, depth+stencil SAMPLE_ZERO,
// CLEAR/DONT_CARE attachments, the pass split across command buffers with
// suspend/resume, and a resolve shader that reads multisampled colour and
// depth input attachments and writes FragDepth.
//
// usage: probe SHADER_DIR [--segments 1|3] [--fill-segment N]
//              [--format b10g11r11|rgba8] [--no-depth-input]
// Select the driver with VK_DRIVER_FILES.

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define LAYERS 2

#define VKOK(call)                                                       \
   do {                                                                  \
      VkResult r_ = (call);                                              \
      if (r_ != VK_SUCCESS) {                                            \
         fprintf(stderr, "FAIL %s -> %d (line %d)\n", #call, r_, __LINE__); \
         exit(2);                                                        \
      }                                                                  \
   } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;

static uint32_t
memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties p;
   vkGetPhysicalDeviceMemoryProperties(phys, &p);
   for (uint32_t i = 0; i < p.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(2);
}

typedef struct {
   VkImage image;
   VkDeviceMemory mem;
} Image;

static Image
make_image(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage)
{
   Image img;
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = LAYERS,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
   };
   VKOK(vkCreateImage(dev, &ci, NULL, &img.image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, img.image, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex =
         memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VKOK(vkAllocateMemory(dev, &ai, NULL, &img.mem));
   VKOK(vkBindImageMemory(dev, img.image, img.mem, 0));
   return img;
}

static VkImageView
make_view_usage(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                VkImageUsageFlags usage)
{
   VkImageViewUsageCreateInfo uci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = usage,
   };
   VkImageViewCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = usage ? &uci : NULL,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = format,
      .subresourceRange = {aspect, 0, 1, 0, LAYERS},
   };
   VkImageView v;
   VKOK(vkCreateImageView(dev, &ci, NULL, &v));
   return v;
}

static VkImageView
make_view(VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
   return make_view_usage(image, format, aspect, 0);
}

static VkShaderModule
load_shader(const char *dir, const char *name)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "missing %s\n", path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(n);
   fread(code, 1, n, f);
   fclose(f);
   VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = n,
      .pCode = code,
   };
   VkShaderModule m;
   VKOK(vkCreateShaderModule(dev, &ci, NULL, &m));
   free(code);
   return m;
}

static void
barrier_general(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {aspect, 0, 1, 0, LAYERS},
   };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                        NULL, 1, &b);
}

static float
uf(uint32_t v, int mbits)
{
   uint32_t e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1);
   if (e == 0)
      return ldexpf((float)m / (1 << mbits), -14);
   return ldexpf(1.0f + (float)m / (1 << mbits), (int)e - 15);
}

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s SHADER_DIR [options]\n", argv[0]);
      return 2;
   }
   const char *dir = argv[1];
   int segments = 3, fill_segment = 0, depth_input = 1, restricted = 0, state_before = 0, dummy = 0;
   VkFormat cfmt = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--segments") && i + 1 < argc)
         segments = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--fill-segment") && i + 1 < argc)
         fill_segment = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--format") && i + 1 < argc)
         cfmt = !strcmp(argv[++i], "rgba8") ? VK_FORMAT_R8G8B8A8_UNORM
                                            : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
      else if (!strcmp(argv[i], "--no-depth-input"))
         depth_input = 0;
      else if (!strcmp(argv[i], "--restricted-views"))
         restricted = 1;
      else if (!strcmp(argv[i], "--state-before"))
         state_before = 1;
      else if (!strcmp(argv[i], "--dummy-descriptors"))
         dummy = 1;
   }
   if (segments != 1 && segments != 3)
      segments = 3;
   if (fill_segment >= segments)
      fill_segment = segments - 1;
   const VkFormat dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
   printf("config: segments=%d fill-segment=%d format=%s depth-input=%d restricted-views=%d state-before=%d dummy-descriptors=%d\n",
          segments, fill_segment,
          cfmt == VK_FORMAT_R8G8B8A8_UNORM ? "rgba8" : "b10g11r11", depth_input,
          restricted, state_before, dummy);

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_4,
   };
   const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
      .pApplicationInfo = &app,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = iext,
   };
   VkInstance inst;
   VKOK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
   if (er != VK_SUCCESS && er != VK_INCOMPLETE)
      return 2;
   VkPhysicalDeviceDriverProperties drv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 pp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &drv,
   };
   vkGetPhysicalDeviceProperties2(phys, &pp);
   printf("driver: %s %s\n", drv.driverName, drv.driverInfo);

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkPhysicalDeviceCustomResolveFeaturesEXT fcr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT,
      .customResolve = VK_TRUE,
   };
   VkPhysicalDeviceVulkan14Features f14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = &fcr,
      .dynamicRenderingLocalRead = VK_TRUE,
   };
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &f14,
      .dynamicRendering = VK_TRUE,
   };
   VkPhysicalDeviceVulkan11Features f11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &f13,
      .multiview = VK_TRUE,
   };
   const char *dext[] = {"VK_EXT_custom_resolve"};
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &f11,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = dext,
   };
   VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
   if (r != VK_SUCCESS) {
      printf("SKIP device with VK_EXT_custom_resolve not available (%d)\n", r);
      return 3;
   }
   PFN_vkCmdBeginCustomResolveEXT begin_custom_resolve =
      (PFN_vkCmdBeginCustomResolveEXT)vkGetDeviceProcAddr(
         dev, "vkCmdBeginCustomResolveEXT");
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   Image ms_color = make_image(cfmt, VK_SAMPLE_COUNT_2_BIT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
   Image rs_color = make_image(cfmt, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   Image ms_depth = make_image(dfmt, VK_SAMPLE_COUNT_2_BIT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
   Image rs_depth = make_image(dfmt, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   const VkImageAspectFlags ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   VkImageView ms_color_v = make_view(ms_color.image, cfmt, VK_IMAGE_ASPECT_COLOR_BIT);
   VkImageView rs_color_v = make_view(rs_color.image, cfmt, VK_IMAGE_ASPECT_COLOR_BIT);
   VkImageView ms_depth_v = make_view(ms_depth.image, dfmt, ds);
   const VkImageUsageFlags read_usage =
      restricted ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
   VkImageView ms_color_in =
      restricted ? make_view_usage(ms_color.image, cfmt, VK_IMAGE_ASPECT_COLOR_BIT, read_usage)
                 : ms_color_v;
   VkImageView ms_depth_in =
      make_view_usage(ms_depth.image, dfmt, VK_IMAGE_ASPECT_DEPTH_BIT, read_usage);
   VkImageView rs_depth_v = make_view(rs_depth.image, dfmt, ds);

   VkDescriptorSetLayoutBinding binds[2] = {
      {0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
   };
   VkDescriptorSetLayoutCreateInfo slci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = binds,
   };
   VkDescriptorSetLayout set_layout;
   VKOK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &set_layout));
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkPipelineLayout layout;
   VKOK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2};
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &ps,
   };
   VkDescriptorPool dpool;
   VKOK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   VkDescriptorSet set;
   VKOK(vkAllocateDescriptorSets(dev, &dsai, &set));
   if (dummy) {
      Image dummy_img = make_image(cfmt, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      VkImageViewCreateInfo dvci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = dummy_img.image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = cfmt,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      VkImageView dummy_view;
      VKOK(vkCreateImageView(dev, &dvci, NULL, &dummy_view));
      ms_color_in = dummy_view;
      ms_depth_in = dummy_view;
   }
   VkDescriptorImageInfo dii[2] = {
      {VK_NULL_HANDLE, ms_color_in, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, ms_depth_in, VK_IMAGE_LAYOUT_GENERAL},
   };
   VkWriteDescriptorSet wds[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
       .dstBinding = 0, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .pImageInfo = &dii[0]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
       .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .pImageInfo = &dii[1]},
   };
   vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

   VkShaderModule vs = load_shader(dir, "fill.vert.spv");
   VkShaderModule fs_fill = load_shader(dir, "fill.frag.spv");
   VkShaderModule fs_resolve = load_shader(
      dir, depth_input ? "resolve.frag.spv" : "resolve_nodepth.frag.spv");

   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE,
      .lineWidth = 1,
   };
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba,
   };
   VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dys = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn,
   };

   VkCustomResolveCreateInfoEXT crc_fill = {
      .sType = VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,
      .customResolve = VK_FALSE,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &cfmt,
   };
   VkPipelineRenderingCreateInfo prci_fill = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = &crc_fill,
      .viewMask = 3,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &cfmt,
      .depthAttachmentFormat = dfmt,
      .stencilAttachmentFormat = dfmt,
   };
   VkPipelineMultisampleStateCreateInfo ms2 = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_2_BIT,
   };
   VkPipelineDepthStencilStateCreateInfo ds_fill = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
   };
   VkPipelineShaderStageCreateInfo st_fill[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_fill, .pName = "main"},
   };
   VkGraphicsPipelineCreateInfo gp_fill = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &prci_fill,
      .stageCount = 2,
      .pStages = st_fill,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vps,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms2,
      .pDepthStencilState = &ds_fill,
      .pColorBlendState = &cb,
      .pDynamicState = &dys,
      .layout = layout,
   };
   VkPipeline pipe_fill;
   VKOK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp_fill, NULL, &pipe_fill));

   uint32_t color_index = 0, depth_index = 1;
   VkRenderingInputAttachmentIndexInfo ial = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentInputIndices = &color_index,
      .pDepthInputAttachmentIndex = &depth_index,
   };
   VkCustomResolveCreateInfoEXT crc_resolve = {
      .sType = VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,
      .pNext = &ial,
      .customResolve = VK_TRUE,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &cfmt,
   };
   VkPipelineRenderingCreateInfo prci_resolve = prci_fill;
   prci_resolve.pNext = &crc_resolve;
   VkPipelineMultisampleStateCreateInfo ms1 = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineDepthStencilStateCreateInfo ds_resolve = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
   };
   VkPipelineShaderStageCreateInfo st_resolve[2] = {
      st_fill[0],
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_resolve, .pName = "main"},
   };
   VkGraphicsPipelineCreateInfo gp_resolve = gp_fill;
   gp_resolve.pNext = &prci_resolve;
   gp_resolve.pStages = st_resolve;
   gp_resolve.pMultisampleState = &ms1;
   gp_resolve.pDepthStencilState = &ds_resolve;
   VkPipeline pipe_resolve;
   VKOK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp_resolve, NULL, &pipe_resolve));

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)W * H * 4 * LAYERS * 2,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buf;
   VKOK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements breq;
   vkGetBufferMemoryRequirements(dev, buf, &breq);
   VkMemoryAllocateInfo bai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = breq.size,
      .memoryTypeIndex = memory_type(breq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory bmem;
   VKOK(vkAllocateMemory(dev, &bai, NULL, &bmem));
   VKOK(vkBindBufferMemory(dev, buf, bmem, 0));

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   VKOK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBuffer cbs[3];
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 3,
   };
   VKOK(vkAllocateCommandBuffers(dev, &cbai, cbs));

   VkRenderingAttachmentInfo color_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ms_color_v,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = VK_RESOLVE_MODE_CUSTOM_BIT_EXT,
      .resolveImageView = rs_color_v,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
   };
   VkRenderingAttachmentInfo depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ms_depth_v,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .resolveImageView = rs_depth_v,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .clearValue = {.depthStencil = {1.0f, 0}},
   };
   VkRenderingAttachmentInfo stencil_att = depth_att;
   VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, {W, H}},
      .layerCount = 1,
      .viewMask = 3,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_att,
      .pDepthAttachment = &depth_att,
      .pStencilAttachment = &stencil_att,
   };
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

   for (int s = 0; s < segments; s++) {
      VkCommandBuffer c = cbs[s];
      VKOK(vkBeginCommandBuffer(c, &bi));
      if (s == 0) {
         barrier_general(c, ms_color.image, VK_IMAGE_ASPECT_COLOR_BIT);
         barrier_general(c, rs_color.image, VK_IMAGE_ASPECT_COLOR_BIT);
         barrier_general(c, ms_depth.image, ds);
         barrier_general(c, rs_depth.image, ds);
      }
      ri.flags = VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT;
      if (s > 0)
         ri.flags |= VK_RENDERING_RESUMING_BIT;
      if (s < segments - 1)
         ri.flags |= VK_RENDERING_SUSPENDING_BIT;
      vkCmdBeginRendering(c, &ri);
      vkCmdSetViewport(c, 0, 1, &vp);
      vkCmdSetScissor(c, 0, 1, &sc);
      if (s == fill_segment) {
         vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_fill);
         vkCmdDraw(c, 3, 1, 0, 0);
      }
      if (s == segments - 1) {
         VkBeginCustomResolveInfoEXT bcr = {
            .sType = VK_STRUCTURE_TYPE_BEGIN_CUSTOM_RESOLVE_INFO_EXT,
         };
         if (state_before) {
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    0, 1, &set, 0, NULL);
            vkCmdSetViewport(c, 0, 1, &vp);
            vkCmdSetScissor(c, 0, 1, &sc);
         }
         begin_custom_resolve(c, &bcr);
         vkCmdSetRenderingInputAttachmentIndices(c, &ial);
         vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_resolve);
         if (!state_before) {
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    0, 1, &set, 0, NULL);
            vkCmdSetViewport(c, 0, 1, &vp);
            vkCmdSetScissor(c, 0, 1, &sc);
         }
         vkCmdDraw(c, 3, 1, 0, 0);
      }
      vkCmdEndRendering(c);
      if (s == segments - 1) {
         VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         };
         vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                              NULL, 0, NULL);
         VkBufferImageCopy cc = {
            .bufferOffset = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS},
            .imageExtent = {W, H, 1},
         };
         vkCmdCopyImageToBuffer(c, rs_color.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &cc);
         VkBufferImageCopy dc = {
            .bufferOffset = (VkDeviceSize)W * H * 4 * LAYERS,
            .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, LAYERS},
            .imageExtent = {W, H, 1},
         };
         vkCmdCopyImageToBuffer(c, rs_depth.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &dc);
      }
      VKOK(vkEndCommandBuffer(c));
   }
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = (uint32_t)segments,
      .pCommandBuffers = cbs,
   };
   VKOK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   VKOK(vkQueueWaitIdle(queue));

   void *map;
   VKOK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
   const uint32_t *px = map;
   const float *dpx = (const float *)((const char *)map + (size_t)W * H * 4 * LAYERS);
   int failures = 0;
   for (int layer = 0; layer < LAYERS; layer++) {
      float want[3] = {0.75f, depth_input ? 0.375f : 0.0f, layer == 0 ? 0.25f : 0.5f};
      int bad = 0;
      float got[3] = {0};
      for (int i = 0; i < W * H; i++) {
         uint32_t v = px[layer * W * H + i];
         float c[3];
         if (cfmt == VK_FORMAT_R8G8B8A8_UNORM) {
            c[0] = (v & 255) / 255.0f;
            c[1] = ((v >> 8) & 255) / 255.0f;
            c[2] = ((v >> 16) & 255) / 255.0f;
         } else {
            c[0] = uf(v & 0x7ff, 6);
            c[1] = uf((v >> 11) & 0x7ff, 6);
            c[2] = uf((v >> 22) & 0x3ff, 5);
         }
         if (i == 0)
            memcpy(got, c, sizeof got);
         for (int k = 0; k < 3; k++)
            if (fabsf(c[k] - want[k]) > 0.01f)
               bad++;
      }
      printf("%s colour layer %d: first (%.3f, %.3f, %.3f), want (%.3f, %.3f, %.3f), bad channels %d\n",
             bad ? "FAIL" : "PASS", layer, got[0], got[1], got[2], want[0],
             want[1], want[2], bad);
      failures += bad != 0;
      int dbad = 0;
      for (int i = 0; i < W * H; i++)
         if (fabsf(dpx[layer * W * H + i] - 0.375f) > 1e-6f)
            dbad++;
      printf("%s depth layer %d: first %.4f, want 0.375, bad %d\n",
             dbad ? "FAIL" : "PASS", layer, dpx[layer * W * H], dbad);
      failures += dbad != 0;
   }
   printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
   return failures ? 1 : 0;
}
