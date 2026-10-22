// Depth-only stitched render pass shaped like Alyx's shadow atlas pass:
// 4096x4096 single-sample depth, a 1024x1024 area at an offset, Load/Store
// like KosmicKrisp, heavy geometry spread over SEGMENTS command buffers that
// are suspended and resumed and committed together.
//
// env: HEAVY=<triangles per segment> SEGMENTS=n NOSTITCH=1 OX=,OY= FMT=d16|d32|d32s8
//      INDEXED=1 VARYINGS=1
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 v0; float4 v1; float4 v2; float4 v3; };\n"
"vertex VOut vs(uint vid [[vertex_id]], constant float4 &box [[buffer(0)]]) {\n"
"  uint t = vid / 3, c = vid % 3;\n"
"  float fx = fract(float(t) * 0.6180339), fy = fract(float(t) * 0.7548776);\n"
"  float sz = 0.05;\n"
"  float x = mix(box.x, box.z, fx) + (c == 1 ? sz : 0.0);\n"
"  float y = mix(box.y, box.w, fy) + (c == 2 ? sz : 0.0);\n"
"  VOut o; o.pos = float4(x, y, fract(float(t) * 0.137), 1.0);\n"
"  o.v0 = float4(fx); o.v1 = float4(fy); o.v2 = o.pos; o.v3 = float4(float(t)); return o; }\n"
"struct POut { float4 pos [[position]]; };\n"
"vertex POut vs_plain(uint vid [[vertex_id]], constant float4 &box [[buffer(0)]]) {\n"
"  uint t = vid / 3, c = vid % 3;\n"
"  float fx = fract(float(t) * 0.6180339), fy = fract(float(t) * 0.7548776);\n"
"  float sz = 0.05;\n"
"  float x = mix(box.x, box.z, fx) + (c == 1 ? sz : 0.0);\n"
"  float y = mix(box.y, box.w, fy) + (c == 2 ? sz : 0.0);\n"
"  POut o; o.pos = float4(x, y, fract(float(t) * 0.137), 1.0); return o; }\n"
"fragment void fs_v(VOut in [[stage_in]]) {}\n";

int
main(void)
{
   @autoreleasepool {
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      id<MTL4CommandQueue> q = [dev newMTL4CommandQueue];
      NSError *err = nil;
      id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
      if (!lib) { printf("compile: %s\n", err.localizedDescription.UTF8String); return 1; }

      const char *fmts = getenv("FMT") ? getenv("FMT") : "d32";
      MTLPixelFormat fmt = !strcmp(fmts, "d16") ? MTLPixelFormatDepth16Unorm
                         : !strcmp(fmts, "d32s8") ? MTLPixelFormatDepth32Float_Stencil8
                                                  : MTLPixelFormatDepth32Float;
      bool varyings = getenv("VARYINGS") != NULL;
      MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
      pd.vertexFunction = [lib newFunctionWithName:varyings ? @"vs" : @"vs_plain"];
      if (varyings)
         pd.fragmentFunction = [lib newFunctionWithName:@"fs_v"];
      pd.depthAttachmentPixelFormat = fmt;
      if (fmt == MTLPixelFormatDepth32Float_Stencil8)
         pd.stencilAttachmentPixelFormat = fmt;
      pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      if (!pso) { printf("pso: %s\n", err.localizedDescription.UTF8String); return 1; }
      MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
      dsd.depthCompareFunction = MTLCompareFunctionLessEqual;
      dsd.depthWriteEnabled = YES;
      id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                    width:4096 height:4096 mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      id<MTLTexture> depth = [dev newTextureWithDescriptor:td];

      int heavy = getenv("HEAVY") ? atoi(getenv("HEAVY")) : 100000;
      int N = getenv("SEGMENTS") ? atoi(getenv("SEGMENTS")) : 3;
      int ox = getenv("OX") ? atoi(getenv("OX")) : 1024, oy = getenv("OY") ? atoi(getenv("OY")) : 2048;
      bool stitch = getenv("NOSTITCH") == NULL;

      id<MTLBuffer> ib = nil;
      if (getenv("INDEXED")) {
         ib = [dev newBufferWithLength:(size_t)heavy * 3 * 4 options:MTLResourceStorageModeShared];
         uint32_t *ix = ib.contents;
         for (size_t k = 0; k < (size_t)heavy * 3; k++) ix[k] = (uint32_t)k;
      }
      id<MTLBuffer> boxes = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
      float *b = boxes.contents;
      b[0] = -1; b[1] = -1; b[2] = 0.95f; b[3] = 0.95f;

      MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
      ad.maxBufferBindCount = 1;
      id<MTL4ArgumentTable> table = [dev newArgumentTableWithDescriptor:ad error:&err];
      [table setAddress:boxes.gpuAddress atIndex:0];

      id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err];
      [rs addAllocation:depth];
      [rs addAllocation:boxes];
      if (ib) [rs addAllocation:ib];
      [rs commit];
      [q addResidencySet:rs];

      id<MTL4CommandBuffer> cbs[8];
      id<MTL4CommandAllocator> allocs[8];
      for (int i = 0; i < N; i++) {
         allocs[i] = [dev newCommandAllocator];
         cbs[i] = [dev newCommandBuffer];
         [cbs[i] beginCommandBufferWithAllocator:allocs[i]];
         MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
         d.depthAttachment.texture = depth;
         d.depthAttachment.loadAction = MTLLoadActionLoad;
         d.depthAttachment.storeAction = (stitch && i < N - 1) ? MTLStoreActionDontCare : MTLStoreActionStore;
         if (fmt == MTLPixelFormatDepth32Float_Stencil8) {
            d.stencilAttachment.texture = depth;
            d.stencilAttachment.loadAction = MTLLoadActionLoad;
            d.stencilAttachment.storeAction = d.depthAttachment.storeAction;
         }
         d.renderTargetWidth = ox + 1024;
         d.renderTargetHeight = oy + 1024;
         d.visibilityResultType = stitch ? MTLVisibilityResultTypeAccumulate : MTLVisibilityResultTypeReset;
         MTL4RenderEncoderOptions opt = 0;
         if (stitch) {
            if (i > 0) opt |= MTL4RenderEncoderOptionResuming;
            if (i < N - 1) opt |= MTL4RenderEncoderOptionSuspending;
         }
         id<MTL4RenderCommandEncoder> e = [cbs[i] renderCommandEncoderWithDescriptor:d options:opt];
         [e setArgumentTable:table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
         [e setRenderPipelineState:pso];
         [e setDepthStencilState:dss];
         MTLViewport vp = {ox, oy, 1024, 1024, 0, 1};
         [e setViewport:vp];
         MTLScissorRect sc = {ox, oy, 1024, 1024};
         [e setScissorRect:sc];
         if (ib)
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)heavy * 3
                           indexType:MTLIndexTypeUInt32 indexBuffer:ib.gpuAddress
                   indexBufferLength:ib.length instanceCount:1 baseVertex:0 baseInstance:0];
         else
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)heavy * 3];
         [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
             visibilityOptions:MTL4VisibilityOptionNone];
         [e endEncoding];
         [cbs[i] endCommandBuffer];
      }
      __block int failed = 0;
      MTL4CommitOptions *co = [MTL4CommitOptions new];
      [co addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
         printf("gpu %.1f ms%s%s\n", (fb.GPUEndTime - fb.GPUStartTime) * 1000.0,
                fb.error ? " error: " : "", fb.error ? fb.error.localizedDescription.UTF8String : "");
         if (fb.error) failed = 1;
      }];
      id<MTLSharedEvent> done = [dev newSharedEvent];
      [q commit:cbs count:N options:co];
      int after = getenv("AFTER") ? atoi(getenv("AFTER")) : 0;
      for (int a = 0; a < after; a++) {
         id<MTL4CommandAllocator> aa = [dev newCommandAllocator];
         id<MTL4CommandBuffer> ac = [dev newCommandBuffer];
         [ac beginCommandBufferWithAllocator:aa];
         MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
         d.depthAttachment.texture = depth;
         d.depthAttachment.loadAction = MTLLoadActionLoad;
         d.depthAttachment.storeAction = MTLStoreActionStore;
         int ax = (a % 3) * 1024, ay = 0;
         d.renderTargetWidth = ax + 1024;
         d.renderTargetHeight = ay + 1024;
         id<MTL4RenderCommandEncoder> e = [ac renderCommandEncoderWithDescriptor:d];
         if (getenv("AFTER_CONSUMER"))
            [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionDevice];
         [e setArgumentTable:table atStages:MTLRenderStageVertex | MTLRenderStageFragment];
         [e setRenderPipelineState:pso];
         [e setDepthStencilState:dss];
         MTLViewport vp = {ax, ay, 1024, 1024, 0, 1};
         [e setViewport:vp];
         [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)heavy * 3];
         [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
             visibilityOptions:MTL4VisibilityOptionNone];
         [e endEncoding];
         [ac endCommandBuffer];
         [q commit:&ac count:1 options:co];
      }
      [q signalEvent:done value:1];
      if (![done waitUntilSignaledValue:1 timeoutMS:20000]) { printf("FAIL wait\n"); return 1; }
      usleep(100000);
      printf("%s %s %d seg x %d tris%s%s\n", failed ? "FAILED" : "OK", fmts, N, heavy,
             stitch ? " stitched" : "", ib ? " indexed" : "");
      return failed;
   }
}
