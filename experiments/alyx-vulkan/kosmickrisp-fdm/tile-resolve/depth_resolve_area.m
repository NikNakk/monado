// Does Metal resolve a multisampled depth attachment when renderTargetWidth or
// renderTargetHeight is smaller than the attachment?
//
// A 2-layer 2x MSAA Depth32Float_Stencil8 target is cleared to 0.375 and
// resolved (sample 0) into a 2-layer single-sample texture that starts at 1.0.
// The resolved values inside the render target area must be 0.375.
//
// usage: depth_resolve_area [RTW RTH]   (default 48 56 of 64x64)
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
   @autoreleasepool {
      const uint32_t W = 64, H = 64;
      uint32_t rtw = argc > 2 ? atoi(argv[1]) : 48, rth = argc > 2 ? atoi(argv[2]) : 56;
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      id<MTL4CommandQueue> q = [dev newMTL4CommandQueue];
      NSError *err = nil;

      MTLTextureDescriptor *md = [MTLTextureDescriptor new];
      md.textureType = MTLTextureType2DMultisampleArray;
      md.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      md.width = W; md.height = H; md.arrayLength = 2; md.sampleCount = 2;
      md.usage = MTLTextureUsageRenderTarget;
      md.storageMode = MTLStorageModePrivate;
      id<MTLTexture> ms = [dev newTextureWithDescriptor:md];

      MTLTextureDescriptor *rd = [MTLTextureDescriptor new];
      rd.textureType = MTLTextureType2DArray;
      rd.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      rd.width = W; rd.height = H; rd.arrayLength = 2;
      rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      rd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> rs = [dev newTextureWithDescriptor:rd];

      id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 * 2 options:MTLResourceStorageModeShared];
      id<MTLResidencySet> set = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err];
      [set addAllocation:ms];
      [set addAllocation:rs];
      [set addAllocation:out];
      [set commit];
      [q addResidencySet:set];

      id<MTL4CommandAllocator> alloc = [dev newCommandAllocator];
      id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
      [cb beginCommandBufferWithAllocator:alloc];

      // Fill the resolve target with 1.0 first.
      MTL4RenderPassDescriptor *init = [MTL4RenderPassDescriptor new];
      init.depthAttachment.texture = rs;
      init.depthAttachment.loadAction = MTLLoadActionClear;
      init.depthAttachment.clearDepth = 1.0;
      init.depthAttachment.storeAction = MTLStoreActionStore;
      init.renderTargetArrayLength = 2;
      id<MTL4RenderCommandEncoder> e0 = [cb renderCommandEncoderWithDescriptor:init];
      [e0 barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
      [e0 endEncoding];

      MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
      d.depthAttachment.texture = ms;
      d.depthAttachment.loadAction = MTLLoadActionClear;
      d.depthAttachment.clearDepth = 0.375;
      d.depthAttachment.storeAction = getenv("STORE") ? MTLStoreActionStoreAndMultisampleResolve
                                                      : MTLStoreActionMultisampleResolve;
      d.depthAttachment.resolveTexture = rs;
      d.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
      d.renderTargetArrayLength = 2;
      d.renderTargetWidth = rtw;
      d.renderTargetHeight = rth;
      id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
      [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
      [e endEncoding];

      id<MTL4ComputeCommandEncoder> c = [cb computeCommandEncoder];
      [c barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
      for (uint32_t l = 0; l < 2; l++)
         [c copyFromTexture:rs sourceSlice:l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(W, H, 1) toBuffer:out destinationOffset:W * H * 4 * l
            destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4
                    options:MTLBlitOptionDepthFromDepthStencil];
      [c endEncoding];
      [cb endCommandBuffer];

      id<MTLSharedEvent> done = [dev newSharedEvent];
      [q commit:&cb count:1];
      [q signalEvent:done value:1];
      [done waitUntilSignaledValue:1 timeoutMS:5000];

      const float *v = out.contents;
      for (uint32_t l = 0; l < 2; l++) {
         uint32_t inside_ok = 0, inside = 0, outside_changed = 0;
         for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
               float f = v[(l * H + y) * W + x];
               if (x < rtw && y < rth) {
                  inside++;
                  inside_ok += f == 0.375f;
               } else {
                  outside_changed += f != 1.0f;
               }
            }
         printf("rt %ux%u of %ux%u layer %u: %u of %u inside resolved, %u outside changed (first %.3f)\n",
                rtw, rth, W, H, l, inside_ok, inside, outside_changed, v[l * H * W]);
      }
   }
   return 0;
}
