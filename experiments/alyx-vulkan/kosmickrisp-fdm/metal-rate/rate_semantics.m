// Empirical check of Metal rate-map semantics used by the KK FDM stage 2 design:
// fragment [[position]] space, viewport space, layer selection and physical size.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct V { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
"vertex V vs(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
"  V v; v.pos = float4(p * 2.0 - 1.0, 0.5, 1.0); v.layer = iid; return v; }\n"
"struct O { float4 c [[color(0)]]; };\n"
"fragment O fs(V in [[stage_in]], constant rasterization_rate_map_data &map [[buffer(0)]],\n"
"              device atomic_uint *count [[buffer(1)]]) {\n"
"  rasterization_rate_map_decoder dec(map);\n"
"  float2 scr = dec.map_physical_to_screen_coordinates(in.pos.xy, in.layer);\n"
"  atomic_fetch_add_explicit(count, 1, memory_order_relaxed);\n"
"  O o; o.c = float4(in.pos.x, in.pos.y, scr.x, scr.y); return o; }\n";

int main(void) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    const uint W = 672, H = 768, L = 2;
    printf("device %s supports 2-layer rate map: %d\n", dev.name.UTF8String,
           [dev supportsRasterizationRateMapWithLayerCount:2]);
    // Alyx ultra square map: 21x24 texels of 32 px, centre cols 6..14, rows 8..15.
    float h[22], v[25];
    for (int i = 0; i <= 21; i++) { int a = i - 1, b = i; bool ca = a >= 6 && a <= 14, cb = b >= 6 && b <= 14; h[i] = (ca || cb) ? 1.0f : 0.22f; }
    for (int i = 0; i <= 24; i++) { int a = i - 1, b = i; bool ca = a >= 8 && a <= 15, cb = b >= 8 && b <= 15; v[i] = (ca || cb) ? 1.0f : 0.22f; }
    MTLRasterizationRateLayerDescriptor *ld =
      [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(22, 25, 0) horizontal:h vertical:v];
    MTLRasterizationRateMapDescriptor *md =
      [MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 0)];
    md.layers[0] = ld; md.layers[1] = ld;
    id<MTLRasterizationRateMap> map = [dev newRasterizationRateMapWithDescriptor:md];
    MTLSize ps = [map physicalSizeForLayer:0];
    MTLSize gran = map.physicalGranularity;
    printf("physical size %lux%lu (screen %ux%u) granularity %lux%lu, %.1f%% of fragments\n",
           ps.width, ps.height, W, H, gran.width, gran.height, 100.0 * ps.width * ps.height / (W * H));
    MTLCoordinate2D c0 = [map mapScreenToPhysicalCoordinates:(MTLCoordinate2D){336, 384} forLayer:0];
    printf("screen centre (336,384) -> physical (%.2f,%.2f)\n", c0.x, c0.y);

    MTLSizeAndAlign sa = map.parameterBufferSizeAndAlign;
    id<MTLBuffer> params = [dev newBufferWithLength:sa.size options:MTLResourceStorageModeShared];
    [map copyParameterDataToBuffer:params offset:0];
    id<MTLBuffer> count = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *(uint32_t *)count.contents = 0;

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) { printf("pipeline: %s\n", err.description.UTF8String); return 2; }

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
    td.textureType = MTLTextureType2DArray; td.arrayLength = L;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.renderTargetArrayLength = L;
    rp.rasterizationRateMap = map;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setViewport:(MTLViewport){0, 0, W, H, 0, 1}];
    [enc setFragmentBuffer:params offset:0 atIndex:0];
    [enc setFragmentBuffer:count offset:0 atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:L];
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("gpu error %s\n", cb.error.description.UTF8String); return 2; }

    uint32_t frags = *(uint32_t *)count.contents;
    printf("fragment invocations %u (full rate would be %u): %.1f%%\n", frags, W * H * L, 100.0 * frags / (W * H * L));
    for (uint layer = 0; layer < L; layer++) {
      float *px = malloc(W * H * 16);
      [rt getBytes:px bytesPerRow:W * 16 bytesPerImage:W * H * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 slice:layer];
      float maxpx = 0, maxpy = 0, maxsx = 0, maxsy = 0; uint written = 0, maxwx = 0, maxwy = 0;
      for (uint y = 0; y < H; y++) for (uint x = 0; x < W; x++) {
        float *p = px + (y * W + x) * 4; if (p[0] < 0) continue;
        written++; if (x > maxwx) maxwx = x; if (y > maxwy) maxwy = y;
        if (p[0] > maxpx) maxpx = p[0]; if (p[1] > maxpy) maxpy = p[1];
        if (p[2] > maxsx) maxsx = p[2]; if (p[3] > maxsy) maxsy = p[3];
      }
      float *mid = px + ((uint)c0.y * W + (uint)c0.x) * 4;
      printf("layer %u: texels written %u, extent %ux%u; max [[position]] (%.1f,%.1f); max screen (%.1f,%.1f); at physical centre pos (%.1f,%.1f) screen (%.1f,%.1f)\n",
             layer, written, maxwx + 1, maxwy + 1, maxpx, maxpy, maxsx, maxsy, mid[0], mid[1], mid[2], mid[3]);
      free(px);
    }
    return 0;
  }
}
