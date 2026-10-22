// Does a Metal 4 render pass suspended and resumed across command buffers keep
// a memoryless MSAA target in tile memory?
//
// Three MTL4 command buffers each draw one third of the screen into the same
// render pass (Suspending, Resuming|Suspending, Resuming) on a memoryless
// 2-layer MSAA target. The last one resolves it with a tile function. All three
// are committed together. The result must contain all three thirds.
//
// usage: stitch_resolve [W H SAMPLES]
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
"vertex VOut vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant float2 &band [[buffer(0)]]) {\n"
"  float2 p = float2(vid & 1, vid >> 1);\n"
"  float x = mix(band.x, band.y, p.x) * 2.0 - 1.0;\n"
"  VOut o; o.pos = float4(x, p.y * 2.0 - 1.0, 0.0, 1.0); o.layer = iid; return o; }\n"
"vertex VOut vs_heavy(uint vid [[vertex_id]], uint iid [[instance_id]], constant float2 &band [[buffer(0)]]) {\n"
"  uint t = vid / 3, c = vid % 3;\n"
"  float fx = fract(float(t) * 0.6180339), fy = fract(float(t) * 0.7548776);\n"
"  float x = mix(band.x, band.y, fx) * 2.0 - 1.0 + (c == 1 ? 0.01 : 0.0);\n"
"  float y = fy * 2.0 - 1.0 + (c == 2 ? 0.01 : 0.0);\n"
"  VOut o; o.pos = float4(x, y, 0.5, 1.0); o.layer = iid; return o; }\n"
"kernel void slow_write(device float4 *bands [[buffer(0)]], constant uint &spin [[buffer(1)]], uint tid [[thread_position_in_grid]]) {\n"
"  float acc = 0.0; for (uint k = 0; k < spin; k++) acc = fma(acc, 0.999, 1.0);\n"
"  if (tid < 3) { float v = acc < -1.0 ? 5.0 : 0.0; bands[tid] = float4(tid / 3.0 + v, (tid + 1) / 3.0 + v, 0, 0); } }\n"
"struct FOut { half4 c [[color(0)]]; };\n"
"fragment FOut fs(VOut in [[stage_in]], constant float2 &band [[buffer(0)]], uint s [[sample_id]]) {\n"
"  FOut o; o.c = half4(band.x, float(in.layer), float(s) / 8.0, 1.0); return o; }\n"
"struct Px { half4 c [[color(0)]]; };\n"
"kernel void tile_resolve(imageblock<Px, imageblock_layout_implicit> ib,\n"
"                         texture2d_array<half, access::write> dst [[texture(0)]],\n"
"                         ushort2 lid [[thread_position_in_threadgroup]],\n"
"                         uint2 gid [[thread_position_in_grid]],\n"
"                         uint layer [[render_target_array_index]]) {\n"
"  ushort n = ib.get_num_samples(); half4 sum = 0;\n"
"  for (ushort i = 0; i < n; i++) sum += ib.read(lid, i, imageblock_data_rate::sample).c;\n"
"  if (gid.x < dst.get_width() && gid.y < dst.get_height()) dst.write(sum / half(n), gid, layer); }\n";

int
main(int argc, char **argv)
{
   @autoreleasepool {
      int W = argc > 1 ? atoi(argv[1]) : 1680, H = argc > 2 ? atoi(argv[2]) : 1713;
      int S = argc > 3 ? atoi(argv[3]) : 2;
      NSUInteger tile = S >= 4 ? 16 : 32;
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      id<MTL4CommandQueue> q = [dev newMTL4CommandQueue];
      NSError *err = nil;

      id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
      if (!lib) { printf("FAIL compile: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
      pd.vertexFunction = [lib newFunctionWithName:@"vs"];
      pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
      pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      pd.rasterSampleCount = S;
      pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      int depth = getenv("DEPTH") != NULL;
      if (depth)
         pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      if (depth)
         pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      id<MTLRenderPipelineState> draw = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      pd.vertexFunction = [lib newFunctionWithName:@"vs_heavy"];
      id<MTLRenderPipelineState> heavy = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      int heavy_tris = getenv("HEAVY") ? atoi(getenv("HEAVY")) : 0;
      MTLTileRenderPipelineDescriptor *tpd = [MTLTileRenderPipelineDescriptor new];
      tpd.tileFunction = [lib newFunctionWithName:@"tile_resolve"];
      tpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      tpd.rasterSampleCount = S;
      tpd.threadgroupSizeMatchesTileSize = YES;
      id<MTLRenderPipelineState> resolve =
         [dev newRenderPipelineStateWithTileDescriptor:tpd options:0 reflection:nil error:&err];
      if (!draw || !resolve) { printf("FAIL pipelines: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLTextureDescriptor *td = [MTLTextureDescriptor new];
      td.textureType = MTLTextureType2DMultisampleArray;
      td.pixelFormat = MTLPixelFormatRGBA16Float;
      td.width = W; td.height = H; td.arrayLength = 2; td.sampleCount = S;
      td.usage = MTLTextureUsageRenderTarget;
      int kk_style = getenv("KK_STYLE") != NULL;
      td.storageMode = kk_style ? MTLStorageModePrivate : MTLStorageModeMemoryless;
      id<MTLTexture> ms = [dev newTextureWithDescriptor:td];
      MTLTextureDescriptor *dd = [MTLTextureDescriptor new];
      dd.textureType = MTLTextureType2DMultisampleArray;
      dd.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
      dd.width = W; dd.height = H; dd.arrayLength = 2; dd.sampleCount = S;
      dd.usage = MTLTextureUsageRenderTarget;
      dd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> ds = depth ? [dev newTextureWithDescriptor:dd] : nil;
      MTLTextureDescriptor *rd = [MTLTextureDescriptor new];
      rd.textureType = MTLTextureType2DArray;
      rd.pixelFormat = MTLPixelFormatRGBA16Float;
      rd.width = W; rd.height = H; rd.arrayLength = 2;
      rd.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
      rd.storageMode = MTLStorageModeShared;
      id<MTLTexture> dst = [dev newTextureWithDescriptor:rd];
      MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                    width:64 height:64 mipmapped:NO];
      od.usage = MTLTextureUsageRenderTarget;
      od.storageMode = MTLStorageModePrivate;
      id<MTLTexture> other = [dev newTextureWithDescriptor:od];

      int race = getenv("RACE") ? atoi(getenv("RACE")) : 0;
      id<MTLComputePipelineState> slow = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"slow_write"] error:&err];
      id<MTLBuffer> spinbuf = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
      *(uint32_t *)spinbuf.contents = (uint32_t)race;
      MTL4ArgumentTableDescriptor *cad = [MTL4ArgumentTableDescriptor new];
      cad.maxBufferBindCount = 2;
      id<MTL4ArgumentTable> ctable = [dev newArgumentTableWithDescriptor:cad error:&err];
      id<MTL4ArgumentTable> tables[3];
      id<MTLBuffer> bands = [dev newBufferWithLength:3 * 16 options:MTLResourceStorageModeShared];
      float *b = bands.contents;
      for (int i = 0; i < 3; i++) {
         b[i * 4 + 0] = race ? 2.0f : i / 3.0f;
         b[i * 4 + 1] = race ? 2.0f : (i + 1) / 3.0f;
         MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
         ad.maxBufferBindCount = 1;
         ad.maxTextureBindCount = 1;
         tables[i] = [dev newArgumentTableWithDescriptor:ad error:&err];
         [tables[i] setAddress:bands.gpuAddress + i * 16 atIndex:0];
         if (i == 2)
            [tables[i] setTexture:dst.gpuResourceID atIndex:0];
      }

      id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err];
      id<MTLBuffer> ib = nil;
      if (getenv("INDEXED")) {
         ib = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
         uint16_t *ix = ib.contents;
         for (int k = 0; k < 4; k++) ix[k] = k;
         [rs addAllocation:ib];
      }
      [rs addAllocation:dst];
      if (ms.storageMode != MTLStorageModeMemoryless) [rs addAllocation:ms];
      if (ds) [rs addAllocation:ds];
      [rs addAllocation:other];
      [rs addAllocation:bands];
      [rs addAllocation:spinbuf];
      [ctable setAddress:bands.gpuAddress atIndex:0];
      [ctable setAddress:spinbuf.gpuAddress atIndex:1];
      [rs commit];
      [q addResidencySet:rs];

      id<MTLRasterizationRateMap> map = nil;
      if (getenv("RATE")) {
         MTLRasterizationRateMapDescriptor *md = [MTLRasterizationRateMapDescriptor
            rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 0)];
         float h[3] = {1.0f, 1.0f, 1.0f}, v[3] = {1.0f, 1.0f, 1.0f};
         for (int l = 0; l < 2; l++)
            [md setLayer:[[MTLRasterizationRateLayerDescriptor alloc]
                            initWithSampleCount:MTLSizeMake(3, 3, 0) horizontal:h vertical:v]
                 atIndex:l];
         map = [dev newRasterizationRateMapWithDescriptor:md];
      }
      int N = getenv("SEGMENTS") ? atoi(getenv("SEGMENTS")) : 3;
      if (getenv("EMPTY_LAST")) {
         td.storageMode = MTLStorageModePrivate;
         td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
         ms = [dev newTextureWithDescriptor:td];
         [rs addAllocation:ms];
         [rs commit];
      }
      id<MTL4CommandBuffer> cbs[8];
      id<MTL4CommandAllocator> allocs[8];
      MTL4RenderEncoderOptions opts[8];
      for (int i = 0; i < N; i++)
         opts[i] = getenv("NOSTITCH") ? 0 : (i > 0 ? MTL4RenderEncoderOptionResuming : 0) |
                   (i < N - 1 ? MTL4RenderEncoderOptionSuspending : 0);
      for (int i = 0; i < N; i++) {
         allocs[i] = [dev newCommandAllocator];
         cbs[i] = [dev newCommandBuffer];
         [cbs[i] beginCommandBufferWithAllocator:allocs[i]];
         if (i == 0 && race) {
            id<MTL4ComputeCommandEncoder> c = [cbs[i] computeCommandEncoder];
            [c setComputePipelineState:slow];
            [c setArgumentTable:ctable];
            [c dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [c barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                visibilityOptions:MTL4VisibilityOptionDevice];
            [c endEncoding];
         }
         if (i == 0 && getenv("EXTRA_BEFORE")) {
            id<MTL4ComputeCommandEncoder> c = [cbs[i] computeCommandEncoder];
            [c barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionNone];
            [c endEncoding];
         }
         MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
         d.colorAttachments[0].texture = ms;
         d.colorAttachments[0].loadAction = kk_style ? MTLLoadActionLoad : MTLLoadActionClear;
         d.colorAttachments[0].clearColor = MTLClearColorMake(0.9, 0.9, 0.9, 0.9);
         d.colorAttachments[0].storeAction =
            (getenv("EMPTY_LAST") || getenv("LAST_STORE")) && i == N - 1 ? MTLStoreActionStore : MTLStoreActionDontCare;
         d.renderTargetArrayLength = 2;
         if (getenv("PARTIAL")) {
            int pw = W * 3 / 4, ph = H * 3 / 4;
            sscanf(getenv("PARTIAL"), "%d,%d", &pw, &ph);
            d.renderTargetWidth = pw;
            d.renderTargetHeight = ph;
         }
         if (ds) {
            d.depthAttachment.texture = ds;
            d.depthAttachment.loadAction = kk_style ? MTLLoadActionLoad : MTLLoadActionClear;
            d.depthAttachment.storeAction = getenv("LAST_STORE") && i == N - 1 ? MTLStoreActionStore : MTLStoreActionDontCare;
            d.stencilAttachment.texture = ds;
            d.stencilAttachment.loadAction = kk_style ? MTLLoadActionLoad : MTLLoadActionClear;
            d.stencilAttachment.storeAction = getenv("LAST_STORE") && i == N - 1 ? MTLStoreActionStore : MTLStoreActionDontCare;
         }
         d.tileWidth = tile; d.tileHeight = tile;
         if (map) d.rasterizationRateMap = map;
         id<MTL4RenderCommandEncoder> e = [cbs[i] renderCommandEncoderWithDescriptor:d options:opts[i]];
         int t = i == N - 1 ? 2 : (i < 2 ? i : 1);
         [e setArgumentTable:tables[t] atStages:MTLRenderStageVertex | MTLRenderStageFragment | MTLRenderStageTile];
         int empty_last = getenv("EMPTY_LAST") != NULL;
         if ((i < 2 || (i == N - 1 && !empty_last) || getenv("DRAW_ALL")) &&
             !(i == 1 && getenv("EMPTY_MIDDLE")) && !(i == N - 1 && empty_last)) {
            [e setRenderPipelineState:draw];
            if (heavy_tris) {
               [e setRenderPipelineState:heavy];
               [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * heavy_tris instanceCount:2];
               [e setRenderPipelineState:draw];
            }
            int loops = getenv("LOOP") ? atoi(getenv("LOOP")) : 1;
            for (int l = 0; l < loops; l++)
            if (ib)
               [e drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4
                              indexType:MTLIndexTypeUInt16 indexBuffer:ib.gpuAddress
                      indexBufferLength:ib.length instanceCount:2 baseVertex:0 baseInstance:0];
            else
               [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:2];
         }
         if (i == N - 1 && !empty_last) {
            [e setRenderPipelineState:resolve];
            [e dispatchThreadsPerTile:MTLSizeMake(tile, tile, 1)];
         }
         if (getenv("END_BARRIER"))
            [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                visibilityOptions:MTL4VisibilityOptionResourceAlias];
         if (getenv("START_BARRIER") && i == 0)
            ;
         [e endEncoding];
         if (i == N - 1 && getenv("RENDER_AFTER")) {
            MTL4RenderPassDescriptor *d2 = [MTL4RenderPassDescriptor new];
            d2.colorAttachments[0].texture = other;
            d2.colorAttachments[0].loadAction = MTLLoadActionClear;
            d2.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTL4RenderCommandEncoder> e2 = [cbs[i] renderCommandEncoderWithDescriptor:d2];
            [e2 endEncoding];
         }
         if (i == N - 1 && getenv("EXTRA_AFTER")) {
            id<MTL4ComputeCommandEncoder> c = [cbs[i] computeCommandEncoder];
            [c barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll
                     visibilityOptions:MTL4VisibilityOptionNone];
            [c endEncoding];
         }
         [cbs[i] endCommandBuffer];
      }
      id<MTLSharedEvent> done = [dev newSharedEvent];
      MTL4CommitOptions *co = [MTL4CommitOptions new];
      [co addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
         printf("gpu %.1f ms%s%s\n", (fb.GPUEndTime - fb.GPUStartTime) * 1000.0,
                fb.error ? " error: " : "", fb.error ? fb.error.localizedDescription.UTF8String : "");
      }];
      [q commit:cbs count:N options:co];
      if (getenv("EXTRA_SEPARATE")) {
         id<MTL4CommandAllocator> a4 = [dev newCommandAllocator];
         id<MTL4CommandBuffer> c4 = [dev newCommandBuffer];
         [c4 beginCommandBufferWithAllocator:a4];
         id<MTL4ComputeCommandEncoder> c = [c4 computeCommandEncoder];
         [c endEncoding];
         [c4 endCommandBuffer];
         [q commit:&c4 count:1];
      }
      [q signalEvent:done value:1];
      if (![done waitUntilSignaledValue:1 timeoutMS:10000]) { printf("FAIL timeout\n"); return 1; }

      long bad = 0, checked = 0;
      __fp16 *row = malloc((size_t)W * 8);
      double expect_alpha = 1.0;
      for (int l = 0; l < 2; l++)
         for (int y = 0; y < H; y += 7) {
            [dst getBytes:row bytesPerRow:(size_t)W * 8 bytesPerImage:(size_t)W * 8 * H
                fromRegion:MTLRegionMake2D(0, y, W, 1) mipmapLevel:0 slice:l];
            for (int x = 0; x < W; x++) {
               int third = x * 3 / W;
               double want_r = third / 3.0, want_s = (S - 1) / 2.0 / 8.0;
               checked++;
               if (fabs((double)row[x * 4] - want_r) > 0.01 || fabs((double)row[x * 4 + 1] - l) > 0.01 ||
                   fabs((double)row[x * 4 + 2] - want_s) > 0.01 ||
                   fabs((double)row[x * 4 + 3] - expect_alpha) > 0.01) {
                  if (bad < 3)
                     printf("  layer %d (%d,%d): %.3f %.3f %.3f %.3f, want %.3f %d %.3f 1\n", l, x, y,
                            (double)row[x * 4], (double)row[x * 4 + 1], (double)row[x * 4 + 2],
                            (double)row[x * 4 + 3], want_r, l, want_s);
                  bad++;
               }
            }
         }
      printf("%dx%dx2 %dx MSAA memoryless, 3 stitched command buffers: %ld of %ld checked pixels wrong\n",
             W, H, S, bad, checked);
      printf("%s\n", bad ? "FAILED" : "OK");
      return bad != 0;
   }
}
