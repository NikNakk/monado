// Physical size for Alyx's ultra square map under different sample densities.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
static int centre_x(int t) { return t >= 6 && t <= 14; }
static int centre_y(int t) { return t >= 8 && t <= 15; }
int main(void) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    const int W = 672, H = 768, MW = 21, MH = 24;
    for (int per = 1; per <= 16; per *= 2) {
      for (int lowq = 0; lowq < 3; lowq++) {
        float low = lowq == 0 ? 0.22f : lowq == 1 ? 0.25f : 0.5f;
        int nh = MW * per + 1, nv = MH * per + 1;
        float *h = malloc(nh * 4), *v = malloc(nv * 4);
        for (int i = 0; i < nh; i++) {
          // sample i sits at x = i/per texels; conservative: max of texels it touches
          int a = (i - 1) / per, b = i / per; if (i == 0) a = 0; if (b >= MW) b = MW - 1;
          if (i % per) a = b = i / per;
          h[i] = (centre_x(a) || centre_x(b)) ? 1.0f : low;
        }
        for (int i = 0; i < nv; i++) {
          int a = (i - 1) / per, b = i / per; if (i == 0) a = 0; if (b >= MH) b = MH - 1;
          if (i % per) a = b = i / per;
          v[i] = (centre_y(a) || centre_y(b)) ? 1.0f : low;
        }
        MTLRasterizationRateLayerDescriptor *ld = [[MTLRasterizationRateLayerDescriptor alloc]
          initWithSampleCount:MTLSizeMake(nh, nv, 0) horizontal:h vertical:v];
        MTLRasterizationRateMapDescriptor *md = [MTLRasterizationRateMapDescriptor
          rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(W, H, 0) layer:ld];
        id<MTLRasterizationRateMap> map = [dev newRasterizationRateMapWithDescriptor:md];
        if (!map) { printf("per=%2d low=%.2f: map creation failed\n", per, low); continue; }
        MTLSize ps = [map physicalSizeForLayer:0];
        printf("samples/texel %2d, low quality %.2f: physical %4lux%-4lu = %5.1f%% of fragments\n",
               per, low, ps.width, ps.height, 100.0 * ps.width * ps.height / (W * H));
        free(h); free(v);
      }
    }
    MTLSize maxs = [[[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(1,1,0)] maxSampleCount];
    printf("max sample count %lux%lu\n", maxs.width, maxs.height);
  }
}
