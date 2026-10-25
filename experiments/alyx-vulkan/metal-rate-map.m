// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// API feasibility only: no Vulkan extension and no rendering/performance claim.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device || ![device supportsRasterizationRateMapWithLayerCount:2]) return 1;
        MTLRasterizationRateMapDescriptor *descriptor =
            [MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(2000, 2040, 1)];
        for (unsigned eye = 0; eye < 2; eye++) {
            MTLRasterizationRateLayerDescriptor *layer = [[MTLRasterizationRateLayerDescriptor alloc]
                initWithSampleCount:MTLSizeMake(16, 16, 1)];
            for (unsigned i = 0; i < 16; i++) {
                layer.horizontalSampleStorage[i] = abs((int)i - (eye ? 9 : 6)) <= 2 ? 1 : 0.25;
                layer.verticalSampleStorage[i] = abs((int)i - 8) <= 2 ? 1 : 0.25;
            }
            [descriptor setLayer:layer atIndex:eye];
        }
        id<MTLRasterizationRateMap> map = [device newRasterizationRateMapWithDescriptor:descriptor];
        if (!map || map.layerCount != 2) return 2;
        MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
        pass.rasterizationRateMap = map;
        if (pass.rasterizationRateMap != map) return 3;
        for (unsigned eye = 0; eye < 2; eye++) {
            MTLSize size = [map physicalSizeForLayer:eye];
            if (size.width >= 2000 || size.height >= 2040) return 4;
            MTLCoordinate2D original = {1000, 1020};
            MTLCoordinate2D physical = [map mapScreenToPhysicalCoordinates:original forLayer:eye];
            MTLCoordinate2D logical = [map mapPhysicalToScreenCoordinates:physical forLayer:eye];
            if (fabs(logical.x-original.x) > 1 || fabs(logical.y-original.y) > 1) return 5;
            printf("PASS %s eye=%u logical=2000x2040 physical=%lux%lu center=%f,%f Metal4 descriptor identity=1\n",
                device.name.UTF8String, eye, (unsigned long)size.width, (unsigned long)size.height, physical.x, physical.y);
        }
    }
    return 0;
}
