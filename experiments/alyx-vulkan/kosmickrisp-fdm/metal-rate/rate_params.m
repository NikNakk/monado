#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
int main(void){@autoreleasepool{id<MTLDevice> d=MTLCreateSystemDefaultDevice();
 for(int n=2;n<=128;n*=2){float*h=calloc(n,4),*v=calloc(n,4);for(int i=0;i<n;i++)h[i]=v[i]=(i%3)?1:0.25f;
 MTLRasterizationRateLayerDescriptor*l=[[MTLRasterizationRateLayerDescriptor alloc]initWithSampleCount:MTLSizeMake(n,n,0) horizontal:h vertical:v];
 MTLRasterizationRateMapDescriptor*m=[MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(2048,2048,0)];
 m.layers[0]=l;m.layers[1]=l;id<MTLRasterizationRateMap> r=[d newRasterizationRateMapWithDescriptor:m];
 MTLSizeAndAlign sa=r.parameterBufferSizeAndAlign;printf("samples %3d x2 layers: param size %lu align %lu\n",n,sa.size,sa.align);}}}
