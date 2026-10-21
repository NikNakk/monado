// VK_EXT_fragment_density_map probe for a full-rate implementation.
//
// Checks advertisement, features, properties and the R8G8 density map format,
// then renders a per-pixel checkerboard into a 2-view multiview target with a
// density map that requests coarse (4x4) shading. A correct full-rate driver
// must still shade every pixel individually and report FragSizeEXT = 1x1.
// With --subsampled the target is a subsampled image, read back through a
// subsampled sampler in a second pass.
//
// usage: probe SHADER_DIR [--subsampled]   (driver from VK_DRIVER_FILES)
// exit: 0 pass, 1 fail, 2 error, 3 extension not advertised

#include <vulkan/vulkan.h>

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
         printf("FAIL %s -> %d (line %d)\n", #call, r_, __LINE__);       \
         exit(2);                                                        \
      }                                                                  \
   } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;
static int failures;

#define CHECK(cond, ...)                                                 \
   do {                                                                  \
      printf("%s ", (cond) ? "PASS" : "FAIL");                           \
      if (!(cond))                                                       \
         failures++;                                                     \
      printf(__VA_ARGS__);                                               \
      printf("\n");                                                      \
   } while (0)

static uint32_t
memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties p;
   vkGetPhysicalDeviceMemoryProperties(phys, &p);
   for (uint32_t i = 0; i < p.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
         return i;
   exit(2);
}

static VkImage
make_image(VkFormat format, uint32_t w, uint32_t h, uint32_t layers,
           VkImageUsageFlags usage, VkImageCreateFlags flags)
{
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = flags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {w, h, 1},
      .mipLevels = 1,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
   };
   VkImage img;
   VKOK(vkCreateImage(dev, &ci, NULL, &img));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, img, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory mem;
   VKOK(vkAllocateMemory(dev, &ai, NULL, &mem));
   VKOK(vkBindImageMemory(dev, img, mem, 0));
   return img;
}

static VkImageView
make_view(VkImage img, VkFormat format, uint32_t layers)
{
   VkImageViewCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers},
   };
   VkImageView v;
   VKOK(vkCreateImageView(dev, &ci, NULL, &v));
   return v;
}

static VkShaderModule
load_shader(const char *dir, const char *name)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f)
      exit(2);
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
barrier(VkCommandBuffer cb, VkImage img, uint32_t layers, VkImageLayout from,
        VkImageLayout to)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers},
   };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                        1, &b);
}

static VkPipeline
make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
              VkPipelineCreateFlags flags)
{
   VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkPipelineRenderingCreateInfo prci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .viewMask = 3,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &fmt,
   };
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gp = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &prci,
      .flags = flags,
      .stageCount = 2, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia,
      .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cb,
      .layout = layout,
   };
   VkPipeline p;
   VKOK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p));
   return p;
}

int
main(int argc, char **argv)
{
   if (argc < 2)
      return 2;
   const char *dir = argv[1];
   int subsampled = argc > 2 && !strcmp(argv[2], "--subsampled");

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_3};
   const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
      .pApplicationInfo = &app, .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = iext};
   VkInstance inst;
   VKOK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
   if (er != VK_SUCCESS && er != VK_INCOMPLETE)
      return 2;

   uint32_t ecount = 0;
   vkEnumerateDeviceExtensionProperties(phys, NULL, &ecount, NULL);
   VkExtensionProperties *exts = calloc(ecount, sizeof(*exts));
   vkEnumerateDeviceExtensionProperties(phys, NULL, &ecount, exts);
   int has_fdm = 0;
   for (uint32_t i = 0; i < ecount; i++)
      has_fdm |= !strcmp(exts[i].extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
   printf("extension VK_EXT_fragment_density_map: %s\n", has_fdm ? "advertised" : "absent");
   if (!has_fdm)
      return 3;

   VkPhysicalDeviceFragmentDensityMapFeaturesEXT ff = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = &ff};
   vkGetPhysicalDeviceFeatures2(phys, &f2);
   CHECK(ff.fragmentDensityMap && !ff.fragmentDensityMapDynamic &&
            ff.fragmentDensityMapNonSubsampledImages,
         "features: map=%u dynamic=%u nonSubsampled=%u", ff.fragmentDensityMap,
         ff.fragmentDensityMapDynamic, ff.fragmentDensityMapNonSubsampledImages);
   VkPhysicalDeviceFragmentDensityMapPropertiesEXT fp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                     .pNext = &fp};
   vkGetPhysicalDeviceProperties2(phys, &p2);
   CHECK(fp.minFragmentDensityTexelSize.width <= fp.maxFragmentDensityTexelSize.width &&
            fp.minFragmentDensityTexelSize.width > 0,
         "properties: texel min %ux%u max %ux%u invocations=%u",
         fp.minFragmentDensityTexelSize.width, fp.minFragmentDensityTexelSize.height,
         fp.maxFragmentDensityTexelSize.width, fp.maxFragmentDensityTexelSize.height,
         fp.fragmentDensityInvocations);
   VkFormatProperties fmtp;
   vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8_UNORM, &fmtp);
   CHECK(fmtp.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT,
         "R8G8_UNORM has FRAGMENT_DENSITY_MAP feature");
   VkPhysicalDeviceImageFormatInfo2 ifi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .flags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT};
   VkImageFormatProperties2 ifp = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
   CHECK(vkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp) == VK_SUCCESS,
         "subsampled RGBA8 colour image format supported");

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1, .pQueuePriorities = &prio};
   VkPhysicalDeviceFragmentDensityMapFeaturesEXT fen = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
      .fragmentDensityMap = VK_TRUE,
      .fragmentDensityMapNonSubsampledImages = VK_TRUE};
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &fen, .dynamicRendering = VK_TRUE};
   VkPhysicalDeviceVulkan11Features f11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &f13, .multiview = VK_TRUE};
   const char *dext[] = {VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &f11, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = dext};
   VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
   CHECK(r == VK_SUCCESS, "device with fragmentDensityMap (%d)", r);
   if (r != VK_SUCCESS)
      return 1;
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   uint32_t mw = (W + fp.minFragmentDensityTexelSize.width - 1) / fp.minFragmentDensityTexelSize.width;
   uint32_t mh = (H + fp.minFragmentDensityTexelSize.height - 1) / fp.minFragmentDensityTexelSize.height;
   VkImage map = make_image(VK_FORMAT_R8G8_UNORM, mw, mh, 1,
                            VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
   VkImageView map_v = make_view(map, VK_FORMAT_R8G8_UNORM, 1);
   VkImage target = make_image(VK_FORMAT_R8G8B8A8_UNORM, W, H, LAYERS,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  (subsampled ? VK_IMAGE_USAGE_SAMPLED_BIT
                                              : VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                               subsampled ? VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT : 0);
   VkImageView target_v = make_view(target, VK_FORMAT_R8G8B8A8_UNORM, LAYERS);
   VkImage out = target;
   VkImageView out_v = VK_NULL_HANDLE;
   if (subsampled) {
      out = make_image(VK_FORMAT_R8G8B8A8_UNORM, W, H, LAYERS,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
      out_v = make_view(out, VK_FORMAT_R8G8B8A8_UNORM, LAYERS);
   }

   VkShaderModule vs = load_shader(dir, "fill.vert.spv");
   VkShaderModule fs = load_shader(dir, "fdm.frag.spv");
   VkPipelineLayoutCreateInfo empty_plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout empty_layout;
   VKOK(vkCreatePipelineLayout(dev, &empty_plci, NULL, &empty_layout));
   VkPipeline pipe = make_pipeline(
      vs, fs, empty_layout,
      VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT);

   VkSampler sampler = VK_NULL_HANDLE;
   VkPipelineLayout sample_layout = VK_NULL_HANDLE;
   VkPipeline sample_pipe = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   if (subsampled) {
      VkSamplerCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
         .flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT,
         .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
      r = vkCreateSampler(dev, &sci, NULL, &sampler);
      CHECK(r == VK_SUCCESS, "subsampled sampler (%d)", r);
      VkDescriptorSetLayoutBinding bind = {
         0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, &sampler};
      VkDescriptorSetLayoutCreateInfo slci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1, .pBindings = &bind};
      VkDescriptorSetLayout sl;
      VKOK(vkCreateDescriptorSetLayout(dev, &slci, NULL, &sl));
      VkPipelineLayoutCreateInfo plci = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1, .pSetLayouts = &sl};
      VKOK(vkCreatePipelineLayout(dev, &plci, NULL, &sample_layout));
      sample_pipe = make_pipeline(vs, load_shader(dir, "sample.frag.spv"),
                                  sample_layout, 0);
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo dpci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
      VkDescriptorPool dp;
      VKOK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
      VkDescriptorSetAllocateInfo dsai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &sl};
      VKOK(vkAllocateDescriptorSets(dev, &dsai, &set));
      VkDescriptorImageInfo dii = {VK_NULL_HANDLE, target_v,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
         .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &dii};
      vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }

   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = W * H * 4 * LAYERS,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer buf;
   VKOK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements breq;
   vkGetBufferMemoryRequirements(dev, buf, &breq);
   VkMemoryAllocateInfo bai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = breq.size,
      .memoryTypeIndex = memory_type(breq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory bmem;
   VKOK(vkAllocateMemory(dev, &bai, NULL, &bmem));
   VKOK(vkBindBufferMemory(dev, buf, bmem, 0));

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   VKOK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
   VkCommandBuffer cb;
   VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VKOK(vkBeginCommandBuffer(cb, &bi));

   barrier(cb, map, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   VkClearColorValue coarse = {.float32 = {0.25f, 0.25f, 0, 0}};
   VkImageSubresourceRange mr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   vkCmdClearColorImage(cb, map, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &coarse, 1, &mr);
   barrier(cb, map, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT);
   barrier(cb, target, LAYERS, VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

   VkRenderingFragmentDensityMapAttachmentInfoEXT fdm_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT,
      .imageView = map_v,
      .imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = target_v,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                         .pNext = &fdm_att,
                         .renderArea = {{0, 0}, {W, H}},
                         .layerCount = 1, .viewMask = 3,
                         .colorAttachmentCount = 1, .pColorAttachments = &att};
   vkCmdBeginRendering(cb, &ri);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdDraw(cb, 3, 1, 0, 0);
   vkCmdEndRendering(cb);

   if (subsampled) {
      barrier(cb, target, LAYERS, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      barrier(cb, out, LAYERS, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      VkRenderingAttachmentInfo att2 = att;
      att2.imageView = out_v;
      VkRenderingInfo ri2 = ri;
      ri2.pNext = NULL;
      ri2.pColorAttachments = &att2;
      vkCmdBeginRendering(cb, &ri2);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sample_pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sample_layout,
                              0, 1, &set, 0, NULL);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
   }
   barrier(cb, out, LAYERS, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS},
                             .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
   VKOK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1, .pCommandBuffers = &cb};
   VKOK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   VKOK(vkQueueWaitIdle(queue));

   uint8_t *px;
   VKOK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
   for (int layer = 0; layer < LAYERS; layer++) {
      int bad = 0;
      for (int y = 0; y < H; y++)
         for (int x = 0; x < W; x++) {
            const uint8_t *p = px + ((layer * H + y) * W + x) * 4;
            uint8_t want_r = ((x + y + layer) & 1) ? 255 : 0;
            if (p[0] != want_r || abs(p[1] - 32) > 1 || abs(p[2] - 32) > 1 ||
                abs(p[3] - 32) > 1)
               bad++;
         }
      const uint8_t *p0 = px + layer * H * W * 4;
      CHECK(bad == 0,
            "%s layer %d: per-pixel checkerboard, FragSize 1x1, invocations 1 "
            "(first %u,%u,%u,%u; bad pixels %d)",
            subsampled ? "subsampled" : "non-subsampled", layer, p0[0], p0[1],
            p0[2], p0[3], bad);
   }
   printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
   return failures ? 1 : 0;
}
