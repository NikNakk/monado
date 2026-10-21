// Rate-mapped FDM probe shaped like native Alyx's stereo pass.
//
// Uploads Alyx's live 21x24x2 density map (ultra square: full rate over map
// columns 6-14 and rows 8-15, 56/255 elsewhere) through a staging buffer, renders
// a 672x768 two-view pass into a subsampled RGBA16 target with a fragment shader
// that writes its own gl_FragCoord and counts its invocations, then reads the
// target back at full size through a subsampled sampler.
//
// A full-rate driver passes with 100% invocations. A rate-mapped driver must
// still put the right screen coordinate at every pixel (exact in the full-rate
// centre, within the coarse fragment size elsewhere) with fewer invocations.
//
// --custom-resolve renders into a 2-sample subsampled target instead and
// custom-resolves it into the subsampled image inside the same FDM pass, as
// Alyx does.
//
// --eye-maps gives the right eye a different map (centre shifted 3 texels
// right). --views reads each eye through its own single-layer view, as Alyx
// does; --fetch does the same with texelFetch.
//
// usage: probe_rate SHADER_DIR [--custom-resolve] [--eye-maps] [--views|--fetch]
//        [--area] [--samples4] [--switch] [--mixed]   (driver from VK_DRIVER_FILES)
// --mixed adds a larger second colour attachment to the FDM pass, so the rate
// map's screen is bigger than the image read back (not with --custom-resolve).
// --switch renders the target again with a second density map after the
// readback pass, in the same command buffer, as Alyx does when its fidelity
// level changes. The readback must still decode with the first map.
// --area renders only the top-left 448x512 with a 14x16 map sized to that area,
// as Alyx's dynamic resolution does; --samples4 uses 4x MSAA with
// --custom-resolve.

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 672
#define H 768
#define LAYERS 2
#define MW 21
#define MH 24

#define VKOK(call)                                                       \
   do {                                                                  \
      VkResult r_ = (call);                                              \
      if (r_ != VK_SUCCESS) {                                            \
         printf("FAIL %s -> %d (line %d)\n", #call, r_, __LINE__);       \
         exit(2);                                                        \
      }                                                                  \
   } while (0)

static int eye_maps, area, extra_att;
static int mw = MW, mh = MH, aw = W, ah = H, vw = W, vh = H;
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
   exit(2);
}

static VkImage
make_image_ms(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
              VkImageCreateFlags flags, VkSampleCountFlagBits samples)
{
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags,
      .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
      .mipLevels = 1, .arrayLayers = LAYERS, .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
   VkImage img;
   VKOK(vkCreateImage(dev, &ci, NULL, &img));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, img, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
      .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory mem;
   VKOK(vkAllocateMemory(dev, &ai, NULL, &mem));
   VKOK(vkBindImageMemory(dev, img, mem, 0));
   return img;
}

static VkImage
make_image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
           VkImageCreateFlags flags)
{
   return make_image_ms(format, w, h, usage, flags, VK_SAMPLE_COUNT_1_BIT);
}

static VkImageView
make_view(VkImage img, VkFormat format)
{
   VkImageViewCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS}};
   VkImageView v;
   VKOK(vkCreateImageView(dev, &ci, NULL, &v));
   return v;
}

static VkBuffer
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size, .usage = usage};
   VkBuffer buf;
   VKOK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buf, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
      .memoryTypeIndex = memory_type(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory mem;
   VKOK(vkAllocateMemory(dev, &ai, NULL, &mem));
   VKOK(vkBindBufferMemory(dev, buf, mem, 0));
   VKOK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
   return buf;
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
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = n, .pCode = code};
   VkShaderModule m;
   VKOK(vkCreateShaderModule(dev, &ci, NULL, &m));
   free(code);
   return m;
}

static void
barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from, .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS}};
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkPipeline
make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
              VkFormat fmt, VkPipelineCreateFlags flags,
              VkSampleCountFlagBits samples, const void *rendering_next)
{
   VkFormat fmts[2] = {fmt, fmt};
   VkPipelineRenderingCreateInfo prci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = rendering_next, .viewMask = 3,
      .colorAttachmentCount = extra_att ? 2 : 1, .pColorAttachmentFormats = fmts};
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, vw, vh, 0, 1};
   VkRect2D sc = {{0, 0}, {vw, vh}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = samples};
   VkPipelineColorBlendAttachmentState cba[2] = {{.colorWriteMask = 0xf},
                                                 {.colorWriteMask = 0xf}};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = extra_att ? 2 : 1, .pAttachments = cba};
   VkGraphicsPipelineCreateInfo gp = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci,
      .flags = flags, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
      .pInputAssemblyState = &ia, .pViewportState = &vps,
      .pRasterizationState = &rs, .pMultisampleState = &ms,
      .pColorBlendState = &cb, .layout = layout};
   VkPipeline p;
   VKOK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p));
   return p;
}


static int
in_centre_texel(int l, int tx, int ty)
{
   int shift = eye_maps && l ? 3 : 0;
   if (area)
      return tx >= 4 + shift && tx <= 9 + shift && ty >= 5 && ty <= 10;
   return tx >= 6 + shift && tx <= 14 + shift && ty >= 8 && ty <= 15;
}

static int
in_centre(int l, int x, int y)
{
   return in_centre_texel(l, x / 32, y / 32);
}

int
main(int argc, char **argv)
{
   if (argc < 2)
      return 2;
   const char *dir = argv[1];
   int cr = 0, views = 0, fetch = 0, samples4 = 0, sw = 0, mixed = 0;
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--custom-resolve"))
         cr = 1;
      else if (!strcmp(argv[i], "--eye-maps"))
         eye_maps = 1;
      else if (!strcmp(argv[i], "--views"))
         views = 1;
      else if (!strcmp(argv[i], "--fetch"))
         fetch = 1;
      else if (!strcmp(argv[i], "--area"))
         area = 1;
      else if (!strcmp(argv[i], "--samples4"))
         samples4 = 1;
      else if (!strcmp(argv[i], "--switch"))
         sw = 1;
      else if (!strcmp(argv[i], "--mixed"))
         mixed = 1;
      else
         return 2;
   }
   const int split = views || fetch;
   if (area) {
      mw = 14;
      mh = 16;
      aw = 448;
      ah = 512;
   }
   const VkSampleCountFlagBits ms_samples =
      samples4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
   const VkFormat cfmt = VK_FORMAT_R16G16B16A16_UNORM;

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_4};
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

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueCount = 1, .pQueuePriorities = &prio};
   VkPhysicalDeviceFragmentDensityMapFeaturesEXT fen = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
      .fragmentDensityMap = VK_TRUE, .fragmentDensityMapNonSubsampledImages = VK_TRUE};
   VkPhysicalDeviceCustomResolveFeaturesEXT fcr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT,
      .pNext = &fen, .customResolve = VK_TRUE};
   VkPhysicalDeviceVulkan14Features f14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = cr ? (void *)&fcr : (void *)&fen, .dynamicRenderingLocalRead = cr};
   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &f14, .dynamicRendering = VK_TRUE};
   VkPhysicalDeviceVulkan11Features f11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &f13, .multiview = VK_TRUE};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = &f11,
                                   .features = {.fragmentStoresAndAtomics = VK_TRUE}};
   const char *dext[] = {VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME, "VK_EXT_custom_resolve"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &f2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci, .enabledExtensionCount = cr ? 2 : 1,
                             .ppEnabledExtensionNames = dext};
   VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
   if (r != VK_SUCCESS) {
      printf("SKIP device with VK_EXT_fragment_density_map (%d)\n", r);
      return 3;
   }
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   void *stage_map;
   VkBuffer stage = make_buffer(mw * mh * 2 * LAYERS, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage_map);
   uint8_t *m = stage_map;
   for (int l = 0; l < LAYERS; l++)
      for (int y = 0; y < mh; y++)
         for (int x = 0; x < mw; x++) {
            uint8_t v = in_centre_texel(l, x, y) ? 255 : 56;
            m[((l * mh + y) * mw + x) * 2 + 0] = v;
            m[((l * mh + y) * mw + x) * 2 + 1] = v;
         }
   void *counter_map;
   VkBuffer counter = make_buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &counter_map);
   *(uint32_t *)counter_map = 0;
   void *out_map;
   VkBuffer outbuf = make_buffer((VkDeviceSize)W * H * 8 * LAYERS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out_map);

   VkImage map = make_image(VK_FORMAT_R8G8_UNORM, mw, mh,
                            VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
   VkImageView map_v = make_view(map, VK_FORMAT_R8G8_UNORM);
   void *stage2_map;
   VkBuffer stage2 = make_buffer(mw * mh * 2 * LAYERS, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage2_map);
   for (int i = 0; i < mw * mh * LAYERS; i++) {
      int x = i % mw, y = (i / mw) % mh;
      uint8_t v = (x < 5 && y < 6) ? 255 : 40;
      ((uint8_t *)stage2_map)[i * 2] = v;
      ((uint8_t *)stage2_map)[i * 2 + 1] = v;
   }
   VkImage map2 = make_image(VK_FORMAT_R8G8_UNORM, mw, mh,
                             VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
   VkImageView map2_v = make_view(map2, VK_FORMAT_R8G8_UNORM);
   VkImage target = make_image(cfmt, W, H,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
   VkImageView target_v = make_view(target, cfmt);
   VkImageView big_v = VK_NULL_HANDLE;
   VkImage big = VK_NULL_HANDLE;
   if (mixed) {
      big = make_image(cfmt, W + 160, H + 96, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
      big_v = make_view(big, cfmt);
   }
   VkImage ms = VK_NULL_HANDLE;
   VkImageView ms_v = VK_NULL_HANDLE;
   if (cr) {
      ms = make_image_ms(cfmt, W, H,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                         VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT, ms_samples);
      ms_v = make_view(ms, cfmt);
   }
   VkImage out = make_image(cfmt, W, H,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
   VkImageView out_v = make_view(out, cfmt);

   VkShaderModule vs = load_shader(dir, "fill.vert.spv");

   VkDescriptorSetLayoutBinding cbind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo cslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &cbind};
   VkDescriptorSetLayout csl;
   VKOK(vkCreateDescriptorSetLayout(dev, &cslci, NULL, &csl));
   VkPipelineLayoutCreateInfo cplci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &csl};
   VkPipelineLayout clayout;
   VKOK(vkCreatePipelineLayout(dev, &cplci, NULL, &clayout));
   vw = aw;
   vh = ah;
   extra_att = mixed;
   VkCustomResolveCreateInfoEXT crc_fill = {
      .sType = VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,
      .customResolve = VK_FALSE, .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &cfmt};
   VkPipeline coord_pipe =
      make_pipeline(vs, load_shader(dir, "coord.frag.spv"), clayout, cfmt,
                    VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT,
                    cr ? ms_samples : VK_SAMPLE_COUNT_1_BIT,
                    cr ? &crc_fill : NULL);

   uint32_t color_index = 0;
   VkRenderingInputAttachmentIndexInfo ial = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentInputIndices = &color_index};
   VkCustomResolveCreateInfoEXT crc_resolve = {
      .sType = VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT, .pNext = &ial,
      .customResolve = VK_TRUE, .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &cfmt};
   VkDescriptorSetLayoutBinding rbind = {0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo rslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &rbind};
   VkDescriptorSetLayout rsl;
   VKOK(vkCreateDescriptorSetLayout(dev, &rslci, NULL, &rsl));
   VkPipelineLayoutCreateInfo rplci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &rsl};
   VkPipelineLayout rlayout;
   VKOK(vkCreatePipelineLayout(dev, &rplci, NULL, &rlayout));
   VkPipeline resolve_pipe = VK_NULL_HANDLE;
   if (cr)
      resolve_pipe =
         make_pipeline(vs, load_shader(dir, "resolve.frag.spv"), rlayout, cfmt,
                       VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT,
                       VK_SAMPLE_COUNT_1_BIT, &crc_resolve);

   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT,
      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
   VkSampler sampler;
   VKOK(vkCreateSampler(dev, &sci, NULL, &sampler));
   VkDescriptorSetLayoutBinding sbind[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler}};
   VkDescriptorSetLayoutCreateInfo sslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = split ? 2 : 1, .pBindings = sbind};
   VkDescriptorSetLayout ssl;
   VKOK(vkCreateDescriptorSetLayout(dev, &sslci, NULL, &ssl));
   VkPipelineLayoutCreateInfo splci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &ssl};
   VkPipelineLayout slayout;
   VKOK(vkCreatePipelineLayout(dev, &splci, NULL, &slayout));
   vw = W;
   vh = H;
   extra_att = 0;
   VkPipeline sample_pipe = make_pipeline(vs, load_shader(dir, fetch ? "fetch_views.frag.spv"
                                                       : views ? "sample_views.frag.spv"
                                                               : "sample.frag.spv"),
                                          slayout, cfmt, 0,
                                          VK_SAMPLE_COUNT_1_BIT, NULL);

   VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                 {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                 {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1}};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 3, .poolSizeCount = 3, .pPoolSizes = ps};
   VkDescriptorPool dp;
   VKOK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
   VkDescriptorSet cset, sset;
   VkDescriptorSetAllocateInfo a1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                     .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &csl};
   VKOK(vkAllocateDescriptorSets(dev, &a1, &cset));
   VkDescriptorSetAllocateInfo a2 = a1;
   a2.pSetLayouts = &ssl;
   VKOK(vkAllocateDescriptorSets(dev, &a2, &sset));
   VkDescriptorBufferInfo cbi = {counter, 0, 4};
   VkDescriptorImageInfo dii = {VK_NULL_HANDLE, target_v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cset, .dstBinding = 0,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       .pBufferInfo = &cbi},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sset, .dstBinding = 0,
       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .pImageInfo = &dii}};
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
   if (split) {
      VkImageView layer_v[2];
      VkDescriptorImageInfo lii[2];
      VkWriteDescriptorSet lw[2];
      for (int l = 0; l < 2; l++) {
         VkImageViewCreateInfo lci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = target,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = cfmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1}};
         VKOK(vkCreateImageView(dev, &lci, NULL, &layer_v[l]));
         lii[l] = (VkDescriptorImageInfo){VK_NULL_HANDLE, layer_v[l],
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         lw[l] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sset, .dstBinding = l,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &lii[l]};
      }
      vkUpdateDescriptorSets(dev, 2, lw, 0, NULL);
   }
   VkDescriptorSet rset = VK_NULL_HANDLE;
   if (cr) {
      VkDescriptorSetAllocateInfo a3 = a1;
      a3.pSetLayouts = &rsl;
      VKOK(vkAllocateDescriptorSets(dev, &a3, &rset));
      VkDescriptorImageInfo rii = {VK_NULL_HANDLE, ms_v, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet rw = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = rset, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .pImageInfo = &rii};
      vkUpdateDescriptorSets(dev, 1, &rw, 0, NULL);
   }

   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   VKOK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VkCommandBuffer cb;
   VKOK(vkAllocateCommandBuffers(dev, &cbai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VKOK(vkBeginCommandBuffer(cb, &bi));

   barrier(cb, map, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   VkBufferImageCopy up = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS},
                           .imageExtent = {mw, mh, 1}};
   vkCmdCopyBufferToImage(cb, stage, map, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
   barrier(cb, map, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT);
   barrier(cb, map2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   vkCmdCopyBufferToImage(cb, stage2, map2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
   barrier(cb, map2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT);
   VkImageLayout att_layout =
      cr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, att_layout);
   if (mixed)
      barrier(cb, big, VK_IMAGE_LAYOUT_UNDEFINED, att_layout);
   if (cr)
      barrier(cb, ms, VK_IMAGE_LAYOUT_UNDEFINED, att_layout);

   VkRenderingFragmentDensityMapAttachmentInfoEXT fdm_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT,
      .imageView = map_v, .imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = target_v,
      .imageLayout = att_layout,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
   if (cr) {
      att.imageView = ms_v;
      att.resolveMode = VK_RESOLVE_MODE_CUSTOM_BIT_EXT;
      att.resolveImageView = target_v;
      att.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }
   VkRenderingAttachmentInfo atts[2] = {att, att};
   atts[1].imageView = big_v;
   VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .pNext = &fdm_att,
                         .flags = cr ? VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT : 0,
                         .renderArea = {{0, 0}, {aw, ah}}, .layerCount = 1, .viewMask = 3,
                         .colorAttachmentCount = mixed ? 2 : 1, .pColorAttachments = atts};
   vkCmdBeginRendering(cb, &ri);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, coord_pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, clayout, 0, 1, &cset, 0, NULL);
   vkCmdDraw(cb, 3, 1, 0, 0);
   if (cr) {
      PFN_vkCmdBeginCustomResolveEXT begin_custom_resolve =
         (PFN_vkCmdBeginCustomResolveEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginCustomResolveEXT");
      VkBeginCustomResolveInfoEXT bcr = {.sType = VK_STRUCTURE_TYPE_BEGIN_CUSTOM_RESOLVE_INFO_EXT};
      begin_custom_resolve(cb, &bcr);
      vkCmdSetRenderingInputAttachmentIndices(cb, &ial);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rlayout, 0, 1, &rset, 0, NULL);
      vkCmdDraw(cb, 3, 1, 0, 0);
   }
   vkCmdEndRendering(cb);

   barrier(cb, target, att_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   barrier(cb, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
   VkRenderingAttachmentInfo att2 = att;
   att2.imageView = out_v;
   att2.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   att2.resolveMode = VK_RESOLVE_MODE_NONE;
   att2.resolveImageView = VK_NULL_HANDLE;
   VkRenderingInfo ri2 = ri;
   ri2.renderArea = (VkRect2D){{0, 0}, {W, H}};
   ri2.pNext = NULL;
   ri2.flags = 0;
   ri2.pColorAttachments = &att2;
   ri2.colorAttachmentCount = 1;
   vkCmdBeginRendering(cb, &ri2);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sample_pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, slayout, 0, 1, &sset, 0, NULL);
   vkCmdDraw(cb, 3, 1, 0, 0);
   vkCmdEndRendering(cb);
   barrier(cb, out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   if (sw) {
      barrier(cb, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, att_layout);
      fdm_att.imageView = map2_v;
      vkCmdBeginRendering(cb, &ri);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, coord_pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, clayout, 0, 1, &cset, 0, NULL);
      vkCmdDraw(cb, 3, 1, 0, 0);
      if (cr) {
         PFN_vkCmdBeginCustomResolveEXT begin_custom_resolve =
            (PFN_vkCmdBeginCustomResolveEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginCustomResolveEXT");
         VkBeginCustomResolveInfoEXT bcr = {.sType = VK_STRUCTURE_TYPE_BEGIN_CUSTOM_RESOLVE_INFO_EXT};
         begin_custom_resolve(cb, &bcr);
         vkCmdSetRenderingInputAttachmentIndices(cb, &ial);
         vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipe);
         vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rlayout, 0, 1, &rset, 0, NULL);
         vkCmdDraw(cb, 3, 1, 0, 0);
      }
      vkCmdEndRendering(cb);
   }
   VkBufferImageCopy down = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS},
                             .imageExtent = {W, H, 1}};
   vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outbuf, 1, &down);
   VKOK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                      .pCommandBuffers = &cb};
   VKOK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   VKOK(vkQueueWaitIdle(queue));

   uint32_t inv = *(uint32_t *)counter_map;
   printf("fragment invocations %u = %.1f%% of full rate\n", inv,
          100.0 * inv / ((double)W * H * LAYERS));
   const uint16_t *px = out_map;
   int failures = 0;
   {
      const int pts[][2] = {{0, 0}, {10, 10}, {100, 100}, {200, 260}, {336, 384},
                            {470, 500}, {600, 700}, {671, 767}};
      for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
         const uint16_t *p = px + ((size_t)pts[i][1] * W + pts[i][0]) * 4;
         printf("  sample (%3d,%3d) -> screen (%7.2f,%7.2f) layer %.0f\n", pts[i][0],
                pts[i][1], p[0] / 65535.0 * W, p[1] / 65535.0 * H, p[2] / 65535.0);
      }
   }
   for (int l = 0; l < LAYERS; l++) {
      double max_c = 0, max_p = 0;
      int bad_c = 0, bad_p = 0, bad_layer = 0, bad_edge = 0;
      for (int y = 0; y < ah; y++)
         for (int x = 0; x < aw; x++) {
            const uint16_t *p = px + ((size_t)(l * H + y) * W + x) * 4;
            double ex = fabs(p[0] / 65535.0 * W - (x + 0.5));
            double ey = fabs(p[1] / 65535.0 * H - (y + 0.5));
            double e = ex > ey ? ex : ey;
            if (area && (x == aw - 1 || y == ah - 1)) {
               bad_edge += e > 6.0;
               continue;
            }
            if (p[2] != (l ? 65535 : 0))
               bad_layer++;
            if (in_centre(l, x, y)) {
               if (e > max_c) max_c = e;
               if (e > 0.6) bad_c++;
            } else {
               if (e > max_p) max_p = e;
               if (e > 6.0) {
                  if (!bad_p && getenv("PROBE_SHOW_BAD"))
                     printf("  first bad periphery pixel layer %d at (%d,%d)\n", l, x, y);
                  bad_p++;
               }
            }
         }
      int ok = !bad_c && !bad_p && !bad_layer;
      failures += !ok;
      printf("%s layer %d: max screen error centre %.2f px (bad %d), periphery %.2f px "
             "(bad %d), wrong layer %d",
             ok ? "PASS" : "FAIL", l, max_c, bad_c, max_p, bad_p, bad_layer);
      if (area)
         printf(", render-area edge pixels unshaded %d (not counted)", bad_edge);
      printf("\n");
   }
   printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
   return failures ? 1 : 0;
}
