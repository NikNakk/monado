// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Spike benchmark, not built by CMake:
//   clang -O2 -fobjc-arc spike_shared_texture_cost.m -framework Foundation -framework Metal -o
//   spike_shared_texture_cost
// Compare ordinary and shareable Metal textures: properties, creation time,
// and GPU time to render into and sample from them.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdlib.h>

static NSString *const kShader =
    @""
     "#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; uint layer [[render_target_array_index]]; };\n"
     "vertex V vs(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
     "  float2 uv = float2((vid << 1) & 2, vid & 2);\n"
     "  V o; o.p = float4(uv * 2.0 - 1.0, 0.5, 1.0); o.uv = uv; o.layer = iid & 1; return o; }\n"
     "fragment float4 fs_fill(V i [[stage_in]]) { return float4(i.uv, 0.25, 0.5); }\n"
     "fragment float4 fs_sample(V i [[stage_in]], texture2d_array<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear);\n"
     "  return t.sample(s, i.uv, i.layer); }\n"
     "fragment float4 fs_sample_depth(V i [[stage_in]], depth2d_array<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear);\n"
     "  return float4(t.sample(s, i.uv, i.layer)); }\n";

static double
now_ms(void)
{
	static mach_timebase_info_data_t timebase;
	if (timebase.denom == 0)
		mach_timebase_info(&timebase);
	return (double)mach_absolute_time() * timebase.numer / timebase.denom / 1e6;
}

static int
cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double
median(double *v, int n)
{
	qsort(v, n, sizeof(double), cmp);
	return v[n / 2];
}

static double
gpu_ms(id<MTLCommandBuffer> cb)
{
	[cb commit];
	[cb waitUntilCompleted];
	return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
}

int
main(void)
{
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		id<MTLCommandQueue> q = [dev newCommandQueue];
		NSError *err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:kShader options:nil error:&err];
		if (!lib) {
			printf("shader: %s\n", err.description.UTF8String);
			return 1;
		}
		const NSUInteger W = 2000, H = 2040, LAYERS = 2; // one PS VR2 eye per layer
		enum
		{
			OVERDRAW = 8,
			ITER = 40
		};

		struct
		{
			const char *name;
			MTLPixelFormat f;
			bool depth;
		} formats[] = {{"BGRA8", MTLPixelFormatBGRA8Unorm, false},
		               {"BGRA8_sRGB", MTLPixelFormatBGRA8Unorm_sRGB, false},
		               {"RGBA16", MTLPixelFormatRGBA16Unorm, false},
		               {"D32F", MTLPixelFormatDepth32Float, true}};

		printf("device=%s size=%lux%lu layers=%lu overdraw=%d iterations=%d\n", dev.name.UTF8String,
		       (unsigned long)W, (unsigned long)H, (unsigned long)LAYERS, OVERDRAW, ITER);

		for (int fi = 0; fi < 4; fi++) {
			MTLTextureDescriptor *d = [MTLTextureDescriptor new];
			d.textureType = MTLTextureType2DArray;
			d.pixelFormat = formats[fi].f;
			d.width = W;
			d.height = H;
			d.arrayLength = LAYERS;
			d.storageMode = MTLStorageModePrivate;
			d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

			// Creation time, interleaved.
			double create[2][ITER];
			for (int i = 0; i < ITER; i++) {
				for (int k = 0; k < 2; k++) {
					@autoreleasepool {
						double t0 = now_ms();
						id<MTLTexture> t = k ? [dev newSharedTextureWithDescriptor:d]
						                     : [dev newTextureWithDescriptor:d];
						create[k][i] = now_ms() - t0;
						(void)t;
					}
				}
			}

			// Creation plus first use (a clear), wall clock: ordinary allocation may be deferred.
			double first[2][ITER];
			for (int i = 0; i < ITER; i++) {
				for (int kk = 0; kk < 2; kk++) {
					int k = (kk + i) & 1;
					@autoreleasepool {
						double t0 = now_ms();
						id<MTLTexture> t = k ? [dev newSharedTextureWithDescriptor:d]
						                     : [dev newTextureWithDescriptor:d];
						MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
						if (formats[fi].depth) {
							rp.depthAttachment.texture = t;
							rp.depthAttachment.loadAction = MTLLoadActionClear;
							rp.depthAttachment.storeAction = MTLStoreActionStore;
						} else {
							rp.colorAttachments[0].texture = t;
							rp.colorAttachments[0].loadAction = MTLLoadActionClear;
							rp.colorAttachments[0].storeAction = MTLStoreActionStore;
						}
						rp.renderTargetArrayLength = LAYERS;
						id<MTLCommandBuffer> cb = [q commandBuffer];
						[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
						[cb commit];
						[cb waitUntilCompleted];
						first[k][i] = now_ms() - t0;
					}
				}
			}
			printf("  create+first clear, wall ms (median): ordinary=%.3f shareable=%.3f\n",
			       median(first[0], ITER), median(first[1], ITER));

			id<MTLTexture> tex[2] = {[dev newTextureWithDescriptor:d],
			                         [dev newSharedTextureWithDescriptor:d]};
			// A reopened shared texture is what the receiving process renders with.
			MTLSharedTextureHandle *handle = [tex[1] newSharedTextureHandle];
			id<MTLTexture> reopened = [dev newSharedTextureWithHandle:handle];
			id<MTLTexture> all[3] = {tex[0], tex[1], reopened};
			const char *kind[3] = {"ordinary", "shareable", "reopened"};

			printf("   (above: %s)\n", formats[fi].name);
			for (int k = 0; k < 3; k++) {
				printf(
				    "  %-9s storage=%lu shareable=%d iosurface=%d gpuOptimized=%d compression=%ld "
				    "allocatedMB=%.1f\n",
				    kind[k], (unsigned long)all[k].storageMode, all[k].isShareable,
				    all[k].iosurface != NULL, all[k].allowGPUOptimizedContents,
				    (long)all[k].compressionType, all[k].allocatedSize / 1048576.0);
			}
			printf("  create ms (median): ordinary=%.3f shareable=%.3f\n", median(create[0], ITER),
			       median(create[1], ITER));

			// Pipelines.
			MTLRenderPipelineDescriptor *pf = [MTLRenderPipelineDescriptor new];
			pf.vertexFunction = [lib newFunctionWithName:@"vs"];
			pf.fragmentFunction = [lib newFunctionWithName:@"fs_fill"];
			pf.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
			id<MTLDepthStencilState> dss = nil;
			if (formats[fi].depth) {
				pf.depthAttachmentPixelFormat = formats[fi].f;
				pf.fragmentFunction = nil;
				MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
				ds.depthCompareFunction = MTLCompareFunctionAlways;
				ds.depthWriteEnabled = YES;
				dss = [dev newDepthStencilStateWithDescriptor:ds];
			} else {
				pf.colorAttachments[0].pixelFormat = formats[fi].f;
				pf.colorAttachments[0].blendingEnabled = YES; // forces a read of the target
				pf.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
				pf.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			}
			id<MTLRenderPipelineState> fill = [dev newRenderPipelineStateWithDescriptor:pf error:&err];
			MTLRenderPipelineDescriptor *ps = [MTLRenderPipelineDescriptor new];
			ps.vertexFunction = [lib newFunctionWithName:@"vs"];
			ps.fragmentFunction =
			    [lib newFunctionWithName:formats[fi].depth ? @"fs_sample_depth" : @"fs_sample"];
			ps.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
			ps.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			id<MTLRenderPipelineState> sample = [dev newRenderPipelineStateWithDescriptor:ps error:&err];
			if (!fill || !sample) {
				printf("pipeline: %s\n", err.description.UTF8String);
				return 1;
			}
			MTLTextureDescriptor *od = [d copy];
			od.pixelFormat = MTLPixelFormatBGRA8Unorm;
			id<MTLTexture> out = [dev newTextureWithDescriptor:od];

			double tf[3][ITER], ts[3][ITER];
			for (int i = -4; i < ITER; i++) { // four warm-up rounds
				for (int kk = 0; kk < 3; kk++) {
					int k = (kk + (i & 3)) % 3; // rotate order
					@autoreleasepool {
						MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
						if (formats[fi].depth) {
							rp.depthAttachment.texture = all[k];
							rp.depthAttachment.loadAction = MTLLoadActionClear;
							rp.depthAttachment.storeAction = MTLStoreActionStore;
						} else {
							rp.colorAttachments[0].texture = all[k];
							rp.colorAttachments[0].loadAction = MTLLoadActionClear;
							rp.colorAttachments[0].storeAction = MTLStoreActionStore;
						}
						rp.renderTargetArrayLength = LAYERS;
						id<MTLCommandBuffer> cb = [q commandBuffer];
						id<MTLRenderCommandEncoder> e =
						    [cb renderCommandEncoderWithDescriptor:rp];
						[e setRenderPipelineState:fill];
						if (dss)
							[e setDepthStencilState:dss];
						[e drawPrimitives:MTLPrimitiveTypeTriangle
						      vertexStart:0
						      vertexCount:3
						    instanceCount:LAYERS * OVERDRAW];
						[e endEncoding];
						double a = gpu_ms(cb);

						MTLRenderPassDescriptor *rs = [MTLRenderPassDescriptor new];
						rs.colorAttachments[0].texture = out;
						rs.colorAttachments[0].loadAction = MTLLoadActionDontCare;
						rs.colorAttachments[0].storeAction = MTLStoreActionStore;
						rs.renderTargetArrayLength = LAYERS;
						cb = [q commandBuffer];
						e = [cb renderCommandEncoderWithDescriptor:rs];
						[e setRenderPipelineState:sample];
						[e setFragmentTexture:all[k] atIndex:0];
						[e drawPrimitives:MTLPrimitiveTypeTriangle
						      vertexStart:0
						      vertexCount:3
						    instanceCount:LAYERS * OVERDRAW];
						[e endEncoding];
						double b = gpu_ms(cb);
						if (i >= 0) {
							tf[k][i] = a;
							ts[k][i] = b;
						}
					}
				}
			}
			double mf[3], ms[3];
			for (int k = 0; k < 3; k++) {
				mf[k] = median(tf[k], ITER);
				ms[k] = median(ts[k], ITER);
			}
			for (int k = 0; k < 3; k++) {
				printf(
				    "  %-9s render-into GPU ms: median=%.3f p10=%.3f p90=%.3f (%+.1f%%)   sample-from "
				    "GPU ms: median=%.3f p10=%.3f p90=%.3f (%+.1f%%)\n",
				    kind[k], mf[k], tf[k][ITER / 10], tf[k][ITER * 9 / 10], (mf[k] / mf[0] - 1) * 100,
				    ms[k], ts[k][ITER / 10], ts[k][ITER * 9 / 10], (ms[k] / ms[0] - 1) * 100);
			}
		}
	}
	return 0;
}
