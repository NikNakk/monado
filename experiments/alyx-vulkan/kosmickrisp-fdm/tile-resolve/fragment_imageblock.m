// Does a fragment function that reads an MSAA colour attachment with
// [[color(0)]] run per sample, and see each sample's own value?
//
// Pass 1 draws a full-screen quad into a 2-layer, 2x MSAA RGBA16F target with
// per-sample shading, writing (layer, sample, x/W, 1). In the same encoder, a
// second draw runs at pixel rate and reads the attachment through an implicit
// imageblock at [[pixel_position_in_tile]], sample 0, and writes the value it
// read to a device buffer. The buffer must hold (layer, 0, x/W, 1) everywhere.
//
// usage: fragment_imageblock [W H]
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
"vertex VOut vs(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
"  float2 p = float2(vid & 1, vid >> 1);\n"
"  VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.layer = iid; return o; }\n"
"struct FOut { half4 c [[color(0)]]; };\n"
"fragment FOut fill(VOut in [[stage_in]], uint s [[sample_id]], constant uint &w [[buffer(0)]]) {\n"
"  FOut o; o.c = half4(float(in.layer), float(s), in.pos.x / float(w), 1.0); return o; }\n"
"struct Px { half4 c [[color(0)]]; };\n"
"fragment FOut readback(VOut in [[stage_in]], half4 c [[color(0)]],\n"
"                       device half4 *out [[buffer(1)]], constant uint &w [[buffer(0)]],\n"
"                       device atomic_uint *calls [[buffer(2)]]) {\n"
"  uint2 p = uint2(in.pos.xy);\n"
"  uint i = (in.layer * 4096u + p.y) * w + p.x;\n"
"  atomic_fetch_add_explicit(&calls[i], 1u, memory_order_relaxed);\n"
"  if (c.y == 0.0h) out[i] = c;\n"
"  discard_fragment();\n"
"  return FOut{}; }\n";

int
main(int argc, char **argv)
{
   @autoreleasepool {
      uint32_t W = argc > 1 ? atoi(argv[1]) : 384, H = argc > 2 ? atoi(argv[2]) : 480;
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      id<MTL4CommandQueue> q = [dev newMTL4CommandQueue];
      NSError *err = nil;
      id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
      if (!lib) { printf("FAIL compile: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
      pd.vertexFunction = [lib newFunctionWithName:@"vs"];
      pd.fragmentFunction = [lib newFunctionWithName:@"fill"];
      pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      pd.rasterSampleCount = 2;
      pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      id<MTLRenderPipelineState> fill = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      pd.fragmentFunction = [lib newFunctionWithName:@"readback"];
      pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
      id<MTLRenderPipelineState> readback = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      if (!fill || !readback) { printf("FAIL pipelines: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLTextureDescriptor *td = [MTLTextureDescriptor new];
      td.textureType = MTLTextureType2DMultisampleArray;
      td.pixelFormat = MTLPixelFormatRGBA16Float;
      td.width = W; td.height = H; td.arrayLength = 2; td.sampleCount = 2;
      td.usage = MTLTextureUsageRenderTarget;
      td.storageMode = getenv("PRIVATE") ? MTLStorageModePrivate : MTLStorageModeMemoryless;
      id<MTLTexture> ms = [dev newTextureWithDescriptor:td];

      id<MTLBuffer> out = [dev newBufferWithLength:(size_t)2 * 4096 * W * 8 options:MTLResourceStorageModeShared];
      memset(out.contents, 0, out.length);
      id<MTLBuffer> wbuf = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
      *(uint32_t *)wbuf.contents = W;

      MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
      ad.maxBufferBindCount = 3;
      id<MTL4ArgumentTable> table = [dev newArgumentTableWithDescriptor:ad error:&err];
      [table setAddress:wbuf.gpuAddress atIndex:0];
      [table setAddress:out.gpuAddress atIndex:1];
      id<MTLBuffer> calls = [dev newBufferWithLength:(size_t)2 * 4096 * W * 4 options:MTLResourceStorageModeShared];
      memset(calls.contents, 0, calls.length);
      [table setAddress:calls.gpuAddress atIndex:2];

      id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err];
      [rs addAllocation:out];
      [rs addAllocation:wbuf];
      [rs addAllocation:calls];
      if (ms.storageMode != MTLStorageModeMemoryless) [rs addAllocation:ms];
      [rs commit];
      [q addResidencySet:rs];

      id<MTL4CommandAllocator> alloc = [dev newCommandAllocator];
      id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
      [cb beginCommandBufferWithAllocator:alloc];
      MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
      d.colorAttachments[0].texture = ms;
      d.colorAttachments[0].loadAction = MTLLoadActionClear;
      d.colorAttachments[0].storeAction = MTLStoreActionDontCare;
      d.renderTargetArrayLength = 2;
      id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
      [e setArgumentTable:table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
      [e setRenderPipelineState:fill];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:2];
      [e barrierAfterEncoderStages:MTLStageFragment beforeEncoderStages:MTLStageFragment
                 visibilityOptions:MTL4VisibilityOptionNone];
      [e setRenderPipelineState:readback];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:2];
      [e endEncoding];
      [cb endCommandBuffer];

      __block int failed = 0;
      MTL4CommitOptions *co = [MTL4CommitOptions new];
      [co addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
         if (fb.error) { printf("gpu error: %s\n", fb.error.localizedDescription.UTF8String); failed = 1; }
      }];
      id<MTLSharedEvent> done = [dev newSharedEvent];
      [q commit:&cb count:1 options:co];
      [q signalEvent:done value:1];
      if (![done waitUntilSignaledValue:1 timeoutMS:10000]) { printf("FAIL timeout\n"); return 1; }
      usleep(50000);

      __fp16 *px = out.contents;
      long bad = 0, checked = 0;
      for (uint32_t l = 0; l < 2; l++)
         for (uint32_t y = 0; y < H; y += 7)
            for (uint32_t x = 0; x < W; x += 5) {
               __fp16 *v = px + (((size_t)l * 4096 + y) * W + x) * 4;
               float want[4] = {l, 0, (x + 0.5f) / W, 1};
               checked++;
               int ok = 1;
               for (int c = 0; c < 4; c++)
                  if (fabsf((float)v[c] - want[c]) > 0.01f) ok = 0;
               if (!ok && bad++ < 4)
                  printf("  layer %u (%u,%u): %.3f %.3f %.3f %.3f\n", l, x, y, (float)v[0],
                         (float)v[1], (float)v[2], (float)v[3]);
            }
      uint32_t *nc = calls.contents;
      long hist[4] = {0};
      for (uint32_t l = 0; l < 2; l++)
         for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
               hist[MIN(nc[((size_t)l * 4096 + y) * W + x], 3u)]++;
      printf("invocations per pixel: 0:%ld 1:%ld 2:%ld 3+:%ld\n", hist[0], hist[1], hist[2], hist[3]);
      printf("%ux%u 2-layer 2x %s: %ld of %ld pixels wrong\n", W, H,
             ms.storageMode == MTLStorageModeMemoryless ? "memoryless" : "private", bad, checked);
      printf("%s\n", bad || failed ? "FAILED" : "OK");
      return bad || failed;
   }
}
