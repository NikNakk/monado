// Standalone VK_EXT_metal_objects interop test for a Vulkan driver on macOS.
//
// Mirrors what Monado's Vulkan client does inside the game process: Metal
// textures, an IOSurface and an MTLSharedEvent are created on the driver's own
// exported MTLDevice, imported into Vulkan, written by Vulkan GPU work, and
// read back through Metal after a shared-event signal. A second submission
// waits on a value that Metal signals, to check the reverse direction.
//
// Build: see run.sh. Select the driver with VK_DRIVER_FILES.

#define VK_USE_PLATFORM_METAL_EXT 1
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
   do {                                                                     \
      if (cond) {                                                           \
         printf("PASS ");                                                   \
      } else {                                                              \
         printf("FAIL ");                                                   \
         failures++;                                                        \
      }                                                                     \
      printf(__VA_ARGS__);                                                  \
      printf("\n");                                                         \
   } while (0)

#define VKOK(call)                                                          \
   do {                                                                     \
      VkResult r_ = (call);                                                 \
      if (r_ != VK_SUCCESS) {                                               \
         printf("FAIL %s -> %d\n", #call, r_);                              \
         exit(2);                                                           \
      }                                                                     \
   } while (0)

enum { W = 64, H = 64 };

static VkDevice dev;
static VkPhysicalDevice phys;
static PFN_vkExportMetalObjectsEXT export_objects;

static uint32_t
memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(phys, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; i++)
      if ((bits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

static VkImage
import_texture(id<MTLTexture> tex, VkFormat format, uint32_t layers,
               VkImageUsageFlags usage, VkResult *out_result)
{
   VkExportMetalObjectCreateInfoEXT export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
      .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
   };
   VkImportMetalTextureInfoEXT import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT,
      .pNext = &export_info,
      .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
      .mtlTexture = tex,
   };
   VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &import,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage image = VK_NULL_HANDLE;
   *out_result = vkCreateImage(dev, &info, NULL, &image);
   return image;
}

static id<MTLTexture>
export_texture(VkImage image)
{
   VkExportMetalTextureInfoEXT tex = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
      .image = image,
      .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
   };
   VkExportMetalObjectsInfoEXT objects = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
      .pNext = &tex,
   };
   export_objects(dev, &objects);
   return tex.mtlTexture;
}

static void
barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
        uint32_t layers, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {aspect, 0, 1, 0, layers},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                        NULL, 1, &b);
}

static bool
pixels_equal(const uint8_t *p, size_t count, const uint8_t want[4])
{
   for (size_t i = 0; i < count; i++)
      if (memcmp(p + i * 4, want, 4) != 0)
         return false;
   return true;
}

int
main(void)
{
   @autoreleasepool {
      VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .apiVersion = VK_API_VERSION_1_3,
      };
      const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
      VkInstanceCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
         .pApplicationInfo = &app,
         .enabledExtensionCount = 1,
         .ppEnabledExtensionNames = iext,
      };
      VkInstance instance;
      VKOK(vkCreateInstance(&ici, NULL, &instance));
      uint32_t n = 1;
      VkResult er = vkEnumeratePhysicalDevices(instance, &n, &phys);
      if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
         printf("FAIL vkEnumeratePhysicalDevices -> %d\n", er);
         return 2;
      }
      VkPhysicalDeviceDriverProperties driver = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      VkPhysicalDeviceProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &driver,
      };
      vkGetPhysicalDeviceProperties2(phys, &props);
      printf("driver=%s (%s) device=%s\n", driver.driverName,
             driver.driverInfo, props.properties.deviceName);

      float prio = 1;
      VkDeviceQueueCreateInfo qci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueCount = 1,
         .pQueuePriorities = &prio,
      };
      VkPhysicalDeviceVulkan13Features f13 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
         .dynamicRendering = VK_TRUE,
      };
      VkPhysicalDeviceVulkan12Features f12 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
         .pNext = &f13,
         .timelineSemaphore = VK_TRUE,
      };
      const char *dext[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME};
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .pNext = &f12,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &qci,
         .enabledExtensionCount = 1,
         .ppEnabledExtensionNames = dext,
      };
      VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
      CHECK(r == VK_SUCCESS, "vkCreateDevice with VK_EXT_metal_objects (%d)", r);
      if (r != VK_SUCCESS)
         return 1;
      export_objects = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(
         dev, "vkExportMetalObjectsEXT");
      CHECK(export_objects != NULL, "vkExportMetalObjectsEXT resolves");
      if (!export_objects)
         return 1;
      VkQueue queue;
      vkGetDeviceQueue(dev, 0, 0, &queue);

      // Device export, then create every Metal object on that device.
      VkExportMetalDeviceInfoEXT dinfo = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
      };
      VkExportMetalObjectsInfoEXT dobjs = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
         .pNext = &dinfo,
      };
      export_objects(dev, &dobjs);
      id<MTLDevice> mtl = dinfo.mtlDevice;
      CHECK(mtl != nil, "exported MTLDevice %s", mtl ? mtl.name.UTF8String : "(null)");
      if (!mtl)
         return 1;

      MTLTextureDescriptor *cd = [MTLTextureDescriptor new];
      cd.textureType = MTLTextureType2DArray;
      cd.pixelFormat = MTLPixelFormatRGBA8Unorm;
      cd.width = W;
      cd.height = H;
      cd.arrayLength = 2;
      cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                 MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
      cd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> color = [mtl newTextureWithDescriptor:cd];

      MTLTextureDescriptor *dd = [MTLTextureDescriptor new];
      dd.textureType = MTLTextureType2D;
      dd.pixelFormat = MTLPixelFormatDepth32Float;
      dd.width = W;
      dd.height = H;
      dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      dd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> depth = [mtl newTextureWithDescriptor:dd];

      IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
         (id)kIOSurfaceWidth : @(W),
         (id)kIOSurfaceHeight : @(H),
         (id)kIOSurfaceBytesPerElement : @4,
         (id)kIOSurfacePixelFormat : @(0x42475241), // 'BGRA'
      });
      id<MTLSharedEvent> event = [mtl newSharedEvent];
      event.signaledValue = 0;
      CHECK(color && depth && surface && event, "Metal objects created");

      // Colour 2D array import and identity.
      VkImageUsageFlags cusage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkImage cimg = import_texture(color, VK_FORMAT_R8G8B8A8_UNORM, 2,
                                    cusage, &r);
      CHECK(r == VK_SUCCESS, "import 2-layer RGBA8 MTLTexture (%d)", r);
      CHECK(export_texture(cimg) == color,
            "exported texture is the imported one (colour array)");

      VkImage dimg = import_texture(
         depth, VK_FORMAT_D32_SFLOAT, 1,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
         &r);
      CHECK(r == VK_SUCCESS, "import D32 MTLTexture (%d)", r);
      CHECK(export_texture(dimg) == depth,
            "exported texture is the imported one (depth)");

      VkImage bad = import_texture(color, VK_FORMAT_B8G8R8A8_UNORM, 2, cusage,
                                   &r);
      printf("INFO mismatched-format import %s (%d); an app error the driver "
             "may or may not catch\n",
             r != VK_SUCCESS ? "rejected" : "accepted", r);
      if (bad != VK_NULL_HANDLE)
         vkDestroyImage(dev, bad, NULL);

      // IOSurface import, Monado style: create, allocate, bind.
      VkImportMetalIOSurfaceInfoEXT ios = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
         .ioSurface = surface,
      };
      VkImageCreateInfo sci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = &ios,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .extent = {W, H, 1},
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      };
      VkImage simg = VK_NULL_HANDLE;
      r = vkCreateImage(dev, &sci, NULL, &simg);
      CHECK(r == VK_SUCCESS, "import BGRA IOSurface (%d)", r);
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(dev, simg, &req);
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      VkDeviceMemory smem;
      VKOK(vkAllocateMemory(dev, &mai, NULL, &smem));
      r = vkBindImageMemory(dev, simg, smem, 0);
      CHECK(r == VK_SUCCESS, "bind memory to IOSurface image is accepted (%d)", r);
      VkExportMetalIOSurfaceInfoEXT eios = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_IO_SURFACE_INFO_EXT,
         .image = simg,
      };
      VkExportMetalObjectsInfoEXT eobjs = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
         .pNext = &eios,
      };
      export_objects(dev, &eobjs);
      CHECK(eios.ioSurface == surface, "exported IOSurface is the imported one");

      // Timeline import and identity.
      VkSemaphoreTypeCreateInfo stype = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      };
      VkImportMetalSharedEventInfoEXT sev = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT,
         .pNext = &stype,
         .mtlSharedEvent = event,
      };
      VkSemaphoreCreateInfo semci = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         .pNext = &sev,
      };
      VkSemaphore sem;
      r = vkCreateSemaphore(dev, &semci, NULL, &sem);
      CHECK(r == VK_SUCCESS, "import MTLSharedEvent as timeline (%d)", r);
      VkExportMetalSharedEventInfoEXT esev = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
         .semaphore = sem,
      };
      VkExportMetalObjectsInfoEXT esobjs = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
         .pNext = &esev,
      };
      export_objects(dev, &esobjs);
      CHECK(esev.mtlSharedEvent == event,
            "exported shared event is the imported one");

      // Image view of layer 1 for a render-pass clear.
      VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = cimg,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1},
      };
      VkImageView view1;
      VKOK(vkCreateImageView(dev, &vci, NULL, &view1));

      VkCommandPoolCreateInfo pci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      };
      VkCommandPool pool;
      VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
      VkCommandBuffer cmds[2];
      VkCommandBufferAllocateInfo cai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 2,
      };
      VKOK(vkAllocateCommandBuffers(dev, &cai, cmds));
      VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };

      // Submission A: write everything, then signal timeline value 1.
      VkCommandBuffer a = cmds[0];
      VKOK(vkBeginCommandBuffer(a, &bi));
      barrier(a, cimg, VK_IMAGE_ASPECT_COLOR_BIT, 2, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue red = {.float32 = {1, 0, 0, 1}};
      VkImageSubresourceRange layer0 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(a, cimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red,
                           1, &layer0);
      barrier(a, cimg, VK_IMAGE_ASPECT_COLOR_BIT, 2,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      VkRenderingAttachmentInfo att = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = view1,
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = {.color = {.float32 = {0, 1, 0, 1}}},
      };
      VkRenderingInfo ri = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {{0, 0}, {W, H}},
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &att,
      };
      vkCmdBeginRendering(a, &ri);
      vkCmdEndRendering(a);
      barrier(a, dimg, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearDepthStencilValue dval = {.depth = 0.25f};
      VkImageSubresourceRange drange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(a, dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &dval, 1, &drange);
      barrier(a, simg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue blue = {.float32 = {0, 0, 1, 1}};
      vkCmdClearColorImage(a, simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue,
                           1, &layer0);
      VKOK(vkEndCommandBuffer(a));

      uint64_t one = 1;
      VkTimelineSemaphoreSubmitInfo tsA = {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .signalSemaphoreValueCount = 1,
         .pSignalSemaphoreValues = &one,
      };
      VkSubmitInfo sA = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &tsA,
         .commandBufferCount = 1,
         .pCommandBuffers = &a,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &sem,
      };
      VKOK(vkQueueSubmit(queue, 1, &sA, VK_NULL_HANDLE));

      // Metal waits for the Vulkan GPU signal, then reads everything back.
      id<MTLCommandQueue> mq = [mtl newCommandQueue];
      id<MTLBuffer> rb = [mtl newBufferWithLength:W * H * 4 * 3
                                          options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> mcb = [mq commandBuffer];
      [mcb encodeWaitForEvent:event value:1];
      id<MTLBlitCommandEncoder> blit = [mcb blitCommandEncoder];
      for (NSUInteger slice = 0; slice < 2; slice++)
         [blit copyFromTexture:color
                          sourceSlice:slice
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(W, H, 1)
                             toBuffer:rb
                    destinationOffset:slice * W * H * 4
               destinationBytesPerRow:W * 4
             destinationBytesPerImage:W * H * 4];
      [blit copyFromTexture:depth
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(W, H, 1)
                          toBuffer:rb
                 destinationOffset:2 * W * H * 4
            destinationBytesPerRow:W * 4
          destinationBytesPerImage:W * H * 4];
      [blit endEncoding];
      [mcb commit];
      [mcb waitUntilCompleted];
      CHECK(mcb.status == MTLCommandBufferStatusCompleted && event.signaledValue >= 1,
            "Metal waited on the Vulkan timeline signal (value %llu)",
            event.signaledValue);

      const uint8_t *px = rb.contents;
      const uint8_t want_red[4] = {255, 0, 0, 255};
      const uint8_t want_green[4] = {0, 255, 0, 255};
      CHECK(pixels_equal(px, W * H, want_red),
            "layer 0 is red after vkCmdClearColorImage (first px %u,%u,%u,%u)",
            px[0], px[1], px[2], px[3]);
      CHECK(pixels_equal(px + W * H * 4, W * H, want_green),
            "layer 1 is green after a render-pass clear through an image view "
            "(first px %u,%u,%u,%u)",
            px[W * H * 4], px[W * H * 4 + 1], px[W * H * 4 + 2],
            px[W * H * 4 + 3]);
      const float *dpx = (const float *)(px + 2 * W * H * 4);
      bool depth_ok = true;
      for (int i = 0; i < W * H; i++)
         depth_ok &= dpx[i] == 0.25f;
      CHECK(depth_ok, "depth is 0.25 (first %f)", dpx[0]);

      IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
      const uint8_t *spx = IOSurfaceGetBaseAddress(surface);
      size_t stride = IOSurfaceGetBytesPerRow(surface);
      const uint8_t want_blue_bgra[4] = {255, 0, 0, 255};
      bool blue_ok = true;
      for (int y = 0; y < H; y++)
         blue_ok &= pixels_equal(spx + y * stride, W, want_blue_bgra);
      CHECK(blue_ok, "IOSurface is blue (first BGRA %u,%u,%u,%u)", spx[0],
            spx[1], spx[2], spx[3]);
      IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);

      // Submission B: Vulkan waits for value 2, which Metal signals later.
      VkCommandBuffer b = cmds[1];
      VKOK(vkBeginCommandBuffer(b, &bi));
      barrier(b, simg, VK_IMAGE_ASPECT_COLOR_BIT, 1,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue white = {.float32 = {1, 1, 1, 1}};
      vkCmdClearColorImage(b, simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &white, 1, &layer0);
      VKOK(vkEndCommandBuffer(b));
      uint64_t two = 2;
      VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkTimelineSemaphoreSubmitInfo tsB = {
         .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .waitSemaphoreValueCount = 1,
         .pWaitSemaphoreValues = &two,
      };
      VkSubmitInfo sB = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &tsB,
         .waitSemaphoreCount = 1,
         .pWaitSemaphores = &sem,
         .pWaitDstStageMask = &stage,
         .commandBufferCount = 1,
         .pCommandBuffers = &b,
      };
      VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence;
      VKOK(vkCreateFence(dev, &fci, NULL, &fence));
      VKOK(vkQueueSubmit(queue, 1, &sB, fence));
      r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 200 * 1000 * 1000ull);
      CHECK(r == VK_TIMEOUT, "Vulkan work is held before Metal signals (%d)", r);

      id<MTLCommandBuffer> sig = [mq commandBuffer];
      [sig encodeSignalEvent:event value:2];
      [sig commit];
      r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000 * 1000 * 1000ull);
      CHECK(r == VK_SUCCESS, "Vulkan work runs after the Metal GPU signal (%d)", r);
      uint64_t value = 0;
      vkGetSemaphoreCounterValue(dev, sem, &value);
      CHECK(value == 2, "Vulkan sees the Metal-signalled value (%llu)",
            (unsigned long long)value);

      IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
      spx = IOSurfaceGetBaseAddress(surface);
      const uint8_t want_white[4] = {255, 255, 255, 255};
      bool white_ok = true;
      for (int y = 0; y < H; y++)
         white_ok &= pixels_equal(spx + y * stride, W, want_white);
      CHECK(white_ok, "IOSurface is white after the gated clear (first %u,%u,%u,%u)",
            spx[0], spx[1], spx[2], spx[3]);
      IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);

      // Monado-style swapchain texture: no ShaderWrite usage. xrizer copies
      // eyes into it with vkCmdCopyImage or vkCmdBlitImage.
      MTLTextureDescriptor *md = [MTLTextureDescriptor new];
      md.textureType = MTLTextureType2DArray;
      md.pixelFormat = MTLPixelFormatRGBA8Unorm;
      md.width = W;
      md.height = H;
      md.arrayLength = 2;
      md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      md.storageMode = MTLStorageModePrivate;
      id<MTLTexture> swap = [mtl newTextureWithDescriptor:md];
      VkImage wimg = import_texture(
         swap, VK_FORMAT_R8G8B8A8_UNORM, 2,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         &r);
      CHECK(r == VK_SUCCESS,
            "import texture without ShaderWrite as a transfer destination (%d)",
            r);
      if (r == VK_SUCCESS) {
         VkCommandBuffer c;
         cai.commandBufferCount = 1;
         VKOK(vkAllocateCommandBuffers(dev, &cai, &c));
         VKOK(vkBeginCommandBuffer(c, &bi));
         barrier(c, cimg, VK_IMAGE_ASPECT_COLOR_BIT, 2,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
         barrier(c, wimg, VK_IMAGE_ASPECT_COLOR_BIT, 2,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         VkImageCopy copy = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .extent = {W, H, 1},
         };
         vkCmdCopyImage(c, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wimg,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
         VkImageBlit blitr = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1},
            .srcOffsets = {{0, 0, 0}, {W, H, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1},
            .dstOffsets = {{0, 0, 0}, {W, H, 1}},
         };
         vkCmdBlitImage(c, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wimg,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitr,
                        VK_FILTER_NEAREST);
         VKOK(vkEndCommandBuffer(c));
         VkSubmitInfo sc = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &c,
         };
         VKOK(vkResetFences(dev, 1, &fence));
         VKOK(vkQueueSubmit(queue, 1, &sc, fence));
         VKOK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000 * 1000 * 1000ull));

         id<MTLCommandBuffer> rcb = [mq commandBuffer];
         id<MTLBlitCommandEncoder> rbl = [rcb blitCommandEncoder];
         for (NSUInteger slice = 0; slice < 2; slice++)
            [rbl copyFromTexture:swap
                             sourceSlice:slice
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(W, H, 1)
                                toBuffer:rb
                       destinationOffset:slice * W * H * 4
                  destinationBytesPerRow:W * 4
                destinationBytesPerImage:W * H * 4];
         [rbl endEncoding];
         [rcb commit];
         [rcb waitUntilCompleted];
         px = rb.contents;
         CHECK(pixels_equal(px, W * H, want_red),
               "vkCmdCopyImage into it writes layer 0 (first px %u,%u,%u,%u)",
               px[0], px[1], px[2], px[3]);
         CHECK(pixels_equal(px + W * H * 4, W * H, want_green),
               "vkCmdBlitImage into it writes layer 1 (first px %u,%u,%u,%u)",
               px[W * H * 4], px[W * H * 4 + 1], px[W * H * 4 + 2],
               px[W * H * 4 + 3]);
         vkDeviceWaitIdle(dev);
         vkDestroyImage(dev, wimg, NULL);
      }

      vkDeviceWaitIdle(dev);
      vkDestroyFence(dev, fence, NULL);
      vkDestroyCommandPool(dev, pool, NULL);
      vkDestroyImageView(dev, view1, NULL);
      vkDestroySemaphore(dev, sem, NULL);
      vkDestroyImage(dev, simg, NULL);
      vkFreeMemory(dev, smem, NULL);
      vkDestroyImage(dev, dimg, NULL);
      vkDestroyImage(dev, cimg, NULL);
      vkDestroyDevice(dev, NULL);
      vkDestroyInstance(instance, NULL);
      CHECK(color.width == W && event.signaledValue == 2,
            "Metal objects outlive the Vulkan handles that imported them");
      CFRelease(surface);

      printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
      return failures ? 1 : 0;
   }
}
