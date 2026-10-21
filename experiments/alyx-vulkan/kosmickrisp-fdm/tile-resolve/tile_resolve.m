// Can a custom MSAA resolve run inside the render pass, from tile memory?
//
// Renders per-sample-distinct colours into a memoryless 2-layer MSAA target,
// then runs a tile function that reads every sample from the implicit
// imageblock and writes the average to a texture array. Checks the result and
// times it against KK's current approach: store the MSAA target, end the pass,
// and resolve in a second pass that reads the samples as a texture.
//
// usage: tile_resolve [W H SAMPLES ITERATIONS] [--rate]
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
"vertex VOut vs(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
"  VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.layer = iid; return o; }\n"
"struct FOut { half4 c [[color(0)]]; };\n"
"fragment FOut fs(VOut in [[stage_in]], uint s [[sample_id]]) {\n"
"  FOut o; o.c = half4(fmod(in.pos.x, 64.0) / 64.0, fmod(in.pos.y, 64.0) / 64.0,\n"
"                      float(in.layer), float(s) / 8.0); return o; }\n"
"struct Px { half4 c [[color(0)]]; };\n"
"kernel void tile_resolve(imageblock<Px, imageblock_layout_implicit> ib,\n"
"                         texture2d_array<half, access::write> dst [[texture(0)]],\n"
"                         ushort2 lid [[thread_position_in_threadgroup]],\n"
"                         uint2 gid [[thread_position_in_grid]],\n"
"                         uint layer [[render_target_array_index]]) {\n"
"  ushort n = ib.get_num_samples();\n"
"  half4 sum = 0;\n"
"  for (ushort i = 0; i < n; i++) sum += ib.read(lid, i, imageblock_data_rate::sample).c;\n"
"  if (gid.x < dst.get_width() && gid.y < dst.get_height())\n"
"    dst.write(sum / half(n), gid, layer); }\n"
"struct ROut { half4 c [[color(0)]]; };\n"
"fragment ROut fs_resolve(VOut in [[stage_in]], texture2d_ms_array<half> ms [[texture(0)]]) {\n"
"  uint2 p = uint2(in.pos.xy); half4 sum = 0; uint n = ms.get_num_samples();\n"
"  for (uint i = 0; i < n; i++) sum += ms.read(p, in.layer, i);\n"
"  ROut o; o.c = sum / half(n); return o; }\n";

static double
run(id<MTLCommandQueue> q, void (^encode)(id<MTLCommandBuffer>), int iters)
{
   double total = 0;
   for (int i = 0; i < iters; i++) {
      id<MTLCommandBuffer> cb = [q commandBuffer];
      encode(cb);
      [cb commit];
      [cb waitUntilCompleted];
      if (cb.error) {
         printf("FAIL command buffer: %s\n", cb.error.localizedDescription.UTF8String);
         exit(1);
      }
      if (i > 0)
         total += cb.GPUEndTime - cb.GPUStartTime;
   }
   return total / (iters - 1) * 1000.0;
}

int
main(int argc, char **argv)
{
   @autoreleasepool {
      int W = 1680, H = 1713, S = 2, iters = 20, rate = 0;
      int pos = 0;
      for (int i = 1; i < argc; i++) {
         if (!strcmp(argv[i], "--rate"))
            rate = 1;
         else if (pos == 0) W = atoi(argv[i]), pos++;
         else if (pos == 1) H = atoi(argv[i]), pos++;
         else if (pos == 2) S = atoi(argv[i]), pos++;
         else iters = atoi(argv[i]);
      }
      const NSUInteger tile_px = S >= 4 ? 16 : 32;
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      id<MTLCommandQueue> q = [dev newCommandQueue];
      NSError *err = nil;
      id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
      if (!lib) {
         printf("FAIL compile: %s\n", err.localizedDescription.UTF8String);
         return 1;
      }
      printf("device %s, %dx%dx2 layers, %dx MSAA, tile shaders %s\n", dev.name.UTF8String,
             W, H, S, [dev supportsFamily:MTLGPUFamilyApple4] ? "yes" : "no");

      MTLTextureDescriptor *td = [MTLTextureDescriptor new];
      td.textureType = MTLTextureType2DMultisampleArray;
      td.pixelFormat = MTLPixelFormatRGBA16Float;
      td.width = W; td.height = H; td.arrayLength = 2; td.sampleCount = S;
      td.usage = MTLTextureUsageRenderTarget;
      td.storageMode = MTLStorageModeMemoryless;
      id<MTLTexture> ms_memoryless = [dev newTextureWithDescriptor:td];
      td.storageMode = MTLStorageModePrivate;
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      id<MTLTexture> ms_stored = [dev newTextureWithDescriptor:td];

      MTLTextureDescriptor *rd = [MTLTextureDescriptor new];
      rd.textureType = MTLTextureType2DArray;
      rd.pixelFormat = MTLPixelFormatRGBA16Float;
      rd.width = W; rd.height = H; rd.arrayLength = 2;
      rd.usage = MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      rd.storageMode = MTLStorageModePrivate;
      id<MTLTexture> dst_tile = [dev newTextureWithDescriptor:rd];
      id<MTLTexture> dst_pass = [dev newTextureWithDescriptor:rd];

      id<MTLRasterizationRateMap> map = nil;
      if (rate) {
         MTLRasterizationRateMapDescriptor *md = [MTLRasterizationRateMapDescriptor
            rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 0)];
         float h[3] = {0.25f, 1.0f, 0.25f}, v[3] = {0.25f, 1.0f, 0.25f};
         for (int l = 0; l < 2; l++) {
            MTLRasterizationRateLayerDescriptor *ld = [[MTLRasterizationRateLayerDescriptor alloc]
               initWithSampleCount:MTLSizeMake(3, 3, 0) horizontal:h vertical:v];
            [md setLayer:ld atIndex:l];
         }
         map = [dev newRasterizationRateMapWithDescriptor:md];
         MTLSize ps = [map physicalSizeForLayer:0];
         printf("rate map physical %lux%lu\n", ps.width, ps.height);
      }

      MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
      pd.vertexFunction = [lib newFunctionWithName:@"vs"];
      pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
      pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      pd.rasterSampleCount = S;
      pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      id<MTLRenderPipelineState> draw = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
      if (!draw) { printf("FAIL draw pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLTileRenderPipelineDescriptor *tpd = [MTLTileRenderPipelineDescriptor new];
      tpd.tileFunction = [lib newFunctionWithName:@"tile_resolve"];
      tpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      tpd.rasterSampleCount = S;
      tpd.threadgroupSizeMatchesTileSize = YES;
      id<MTLRenderPipelineState> tile =
         [dev newRenderPipelineStateWithTileDescriptor:tpd options:0 reflection:nil error:&err];
      if (!tile) { printf("FAIL tile pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }

      MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
      rp.vertexFunction = [lib newFunctionWithName:@"vs"];
      rp.fragmentFunction = [lib newFunctionWithName:@"fs_resolve"];
      rp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
      rp.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      id<MTLRenderPipelineState> resolve = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
      if (!resolve) { printf("FAIL resolve pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }

      double t_tile = run(q, ^(id<MTLCommandBuffer> cb) {
         MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
         d.colorAttachments[0].texture = ms_memoryless;
         d.colorAttachments[0].loadAction = MTLLoadActionClear;
         d.colorAttachments[0].storeAction = MTLStoreActionDontCare;
         d.renderTargetArrayLength = 2;
         d.tileWidth = tile_px; d.tileHeight = tile_px;
         if (map) d.rasterizationRateMap = map;
         id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
         [e setRenderPipelineState:draw];
         [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
         [e setRenderPipelineState:tile];
         [e setTileTexture:dst_tile atIndex:0];
         [e dispatchThreadsPerTile:MTLSizeMake(tile_px, tile_px, 1)];
         [e endEncoding];
      }, iters);

      double t_pass = run(q, ^(id<MTLCommandBuffer> cb) {
         MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
         d.colorAttachments[0].texture = ms_stored;
         d.colorAttachments[0].loadAction = MTLLoadActionClear;
         d.colorAttachments[0].storeAction = MTLStoreActionStore;
         d.renderTargetArrayLength = 2;
         if (map) d.rasterizationRateMap = map;
         id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
         [e setRenderPipelineState:draw];
         [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
         [e endEncoding];
         MTLRenderPassDescriptor *d2 = [MTLRenderPassDescriptor renderPassDescriptor];
         d2.colorAttachments[0].texture = dst_pass;
         d2.colorAttachments[0].loadAction = MTLLoadActionDontCare;
         d2.colorAttachments[0].storeAction = MTLStoreActionStore;
         d2.renderTargetArrayLength = 2;
         if (map) d2.rasterizationRateMap = map;
         id<MTLRenderCommandEncoder> e2 = [cb renderCommandEncoderWithDescriptor:d2];
         [e2 setRenderPipelineState:resolve];
         [e2 setFragmentTexture:ms_stored atIndex:0];
         [e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
         [e2 endEncoding];
      }, iters);

      size_t row = (size_t)W * 8, slice = row * H;
      id<MTLBuffer> rb_tile = [dev newBufferWithLength:slice * 2 options:MTLResourceStorageModeShared];
      id<MTLBuffer> rb_pass = [dev newBufferWithLength:slice * 2 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [q commandBuffer];
      id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
      for (int l = 0; l < 2; l++) {
         [b copyFromTexture:dst_tile sourceSlice:l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(W, H, 1) toBuffer:rb_tile destinationOffset:slice * l
         destinationBytesPerRow:row destinationBytesPerImage:slice];
         [b copyFromTexture:dst_pass sourceSlice:l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(W, H, 1) toBuffer:rb_pass destinationOffset:slice * l
         destinationBytesPerRow:row destinationBytesPerImage:slice];
      }
      [b endEncoding];
      [cb commit];
      [cb waitUntilCompleted];

      const __fp16 *a = rb_tile.contents, *p = rb_pass.contents;
      MTLSize phys = map ? [map physicalSizeForLayer:0] : MTLSizeMake(W, H, 1);
      long bad = 0, bad_layer = 0, checked = 0;
      double expect_alpha = (S - 1) / 2.0 / 8.0;
      for (int l = 0; l < 2; l++)
         for (int y = 0; y < (int)phys.height; y++)
            for (int x = 0; x < (int)phys.width; x++) {
               const __fp16 *ta = a + ((size_t)l * H * W + (size_t)y * W + x) * 4;
               const __fp16 *pa = p + ((size_t)l * H * W + (size_t)y * W + x) * 4;
               checked++;
               if (fabs((double)ta[2] - l) > 0.01)
                  bad_layer++;
               if (fabs((double)ta[3] - expect_alpha) > 0.01 ||
                   fabs((double)ta[0] - (double)pa[0]) > 0.01 ||
                   fabs((double)ta[1] - (double)pa[1]) > 0.01) {
                  if (bad < 3)
                     printf("  differ at layer %d (%d,%d): tile %.3f %.3f %.3f %.3f pass %.3f %.3f %.3f %.3f\n",
                            l, x, y, (double)ta[0], (double)ta[1], (double)ta[2], (double)ta[3],
                            (double)pa[0], (double)pa[1], (double)pa[2], (double)pa[3]);
                  bad++;
               }
            }
      printf("checked %ld physical pixels: %ld differ from the two-pass resolve, %ld wrong layer\n",
             checked, bad, bad_layer);
      printf("GPU time per frame: tile resolve, memoryless %.3f ms; store + resolve pass %.3f ms\n",
             t_tile, t_pass);
      printf("MSAA memory avoided: %.1f MB\n", ms_stored.allocatedSize / 1048576.0);
      printf("%s\n", bad || bad_layer ? "FAILED" : "OK");
      return bad || bad_layer;
   }
}
