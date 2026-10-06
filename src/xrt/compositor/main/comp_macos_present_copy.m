// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Final copy of a presented image into a CAMetalLayer drawable.
 * @ingroup comp_main
 */

#include "main/comp_macos_present_copy.h"

#include "util/u_logging.h"

#include <stdlib.h>

#define PRESENT_COPY_MAX_PIPELINES 4

enum present_copy_mode
{
	PRESENT_COPY_PASS = 0,
	//! sRGB source into a non-sRGB destination: re-encode what sampling decoded.
	PRESENT_COPY_ENCODE = 1,
	//! Non-sRGB source into an sRGB destination: decode what the store will encode.
	PRESENT_COPY_DECODE = 2,
};

struct present_copy_pipeline
{
	MTLPixelFormat format;
	enum present_copy_mode mode;
	id<MTLRenderPipelineState> state;
};

struct comp_macos_present_copy
{
	id<MTLDevice> device;
	id<MTLLibrary> library;
	id<MTLSamplerState> sampler;
	struct present_copy_pipeline pipelines[PRESENT_COPY_MAX_PIPELINES];
	uint32_t pipeline_count;
};

static NSString *const present_copy_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "constant uint mode [[function_constant(0)]];\n"
     "struct v2f { float4 position [[position]]; float2 uv; };\n"
     "vertex v2f present_copy_vs(uint vid [[vertex_id]]) {\n"
     "  float2 uv = float2((vid << 1) & 2, vid & 2);\n"
     "  v2f out;\n"
     "  out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
     "  out.uv = uv;\n"
     "  return out;\n"
     "}\n"
     "static float3 encode_srgb(float3 c) {\n"
     "  return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);\n"
     "}\n"
     "static float3 decode_srgb(float3 c) {\n"
     "  return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);\n"
     "}\n"
     "fragment float4 present_copy_fs(v2f in [[stage_in]], texture2d<float> src [[texture(0)]],\n"
     "                                sampler s [[sampler(0)]]) {\n"
     "  float4 c = src.sample(s, in.uv);\n"
     "  if (mode == 1) { c.rgb = encode_srgb(saturate(c.rgb)); }\n"
     "  if (mode == 2) { c.rgb = decode_srgb(saturate(c.rgb)); }\n"
     "  return c;\n"
     "}\n";

static bool
is_srgb(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm_sRGB:
	case MTLPixelFormatBGRA8Unorm_sRGB:
	case MTLPixelFormatBGR10_XR_sRGB:
	case MTLPixelFormatBGRA10_XR_sRGB: return true;
	default: return false;
	}
}

static id<MTLRenderPipelineState>
get_pipeline(struct comp_macos_present_copy *pc, MTLPixelFormat format, enum present_copy_mode mode)
{
	for (uint32_t i = 0; i < pc->pipeline_count; i++) {
		if (pc->pipelines[i].format == format && pc->pipelines[i].mode == mode) {
			return pc->pipelines[i].state;
		}
	}
	if (pc->pipeline_count == PRESENT_COPY_MAX_PIPELINES) {
		U_LOG_E("Present copy: too many destination formats");
		return nil;
	}

	NSError *error = nil;
	if (pc->library == nil) {
		pc->library = [pc->device newLibraryWithSource:present_copy_source options:nil error:&error];
		if (pc->library == nil) {
			U_LOG_E("Present copy: shader compile failed: %s", [[error localizedDescription] UTF8String]);
			return nil;
		}
	}

	uint32_t mode_value = (uint32_t)mode;
	MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
	[constants setConstantValue:&mode_value type:MTLDataTypeUInt atIndex:0];
	id<MTLFunction> vs = [pc->library newFunctionWithName:@"present_copy_vs"];
	id<MTLFunction> fs = [pc->library newFunctionWithName:@"present_copy_fs" constantValues:constants error:&error];
	[constants release];

	id<MTLRenderPipelineState> state = nil;
	if (vs != nil && fs != nil) {
		MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
		[desc setVertexFunction:vs];
		[desc setFragmentFunction:fs];
		[[[desc colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:format];
		state = [pc->device newRenderPipelineStateWithDescriptor:desc error:&error];
		[desc release];
	}
	[vs release];
	[fs release];
	if (state == nil) {
		U_LOG_E("Present copy: pipeline creation failed: %s",
		        error != nil ? [[error localizedDescription] UTF8String] : "no function");
		return nil;
	}

	pc->pipelines[pc->pipeline_count++] = (struct present_copy_pipeline){
	    .format = format,
	    .mode = mode,
	    .state = state,
	};
	return state;
}

struct comp_macos_present_copy *
comp_macos_present_copy_create(id<MTLDevice> device)
{
	if (device == nil) {
		return NULL;
	}
	struct comp_macos_present_copy *pc = calloc(1, sizeof(*pc));
	if (pc == NULL) {
		return NULL;
	}
	pc->device = [device retain];

	MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
	[desc setMinFilter:MTLSamplerMinMagFilterLinear];
	[desc setMagFilter:MTLSamplerMinMagFilterLinear];
	[desc setSAddressMode:MTLSamplerAddressModeClampToEdge];
	[desc setTAddressMode:MTLSamplerAddressModeClampToEdge];
	pc->sampler = [device newSamplerStateWithDescriptor:desc];
	[desc release];
	if (pc->sampler == nil) {
		comp_macos_present_copy_destroy(&pc);
		return NULL;
	}
	return pc;
}

void
comp_macos_present_copy_destroy(struct comp_macos_present_copy **pc_ptr)
{
	struct comp_macos_present_copy *pc = *pc_ptr;
	if (pc == NULL) {
		return;
	}
	for (uint32_t i = 0; i < pc->pipeline_count; i++) {
		[pc->pipelines[i].state release];
	}
	[pc->sampler release];
	[pc->library release];
	[pc->device release];
	free(pc);
	*pc_ptr = NULL;
}

bool
comp_macos_present_copy_needs_draw(id<MTLTexture> src, id<MTLTexture> dst)
{
	return [src pixelFormat] != [dst pixelFormat] || [src width] != [dst width] || [src height] != [dst height];
}

bool
comp_macos_present_copy_encode(struct comp_macos_present_copy *pc,
                               id<MTLCommandBuffer> command_buffer,
                               id<MTLTexture> src,
                               id<MTLTexture> dst)
{
	if (pc == NULL || command_buffer == nil || src == nil || dst == nil) {
		return false;
	}

	if (!comp_macos_present_copy_needs_draw(src, dst)) {
		id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
		if (blit == nil) {
			return false;
		}
		[blit copyFromTexture:src
		         sourceSlice:0
		         sourceLevel:0
		        sourceOrigin:MTLOriginMake(0, 0, 0)
		          sourceSize:MTLSizeMake([src width], [src height], 1)
		           toTexture:dst
		    destinationSlice:0
		    destinationLevel:0
		   destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		return true;
	}

	bool src_srgb = is_srgb([src pixelFormat]);
	bool dst_srgb = is_srgb([dst pixelFormat]);
	enum present_copy_mode mode = PRESENT_COPY_PASS;
	if (src_srgb && !dst_srgb) {
		mode = PRESENT_COPY_ENCODE;
	} else if (!src_srgb && dst_srgb) {
		mode = PRESENT_COPY_DECODE;
	}
	id<MTLRenderPipelineState> pipeline = get_pipeline(pc, [dst pixelFormat], mode);
	if (pipeline == nil) {
		return false;
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassColorAttachmentDescriptor *color = [[rp colorAttachments] objectAtIndexedSubscript:0];
	[color setTexture:dst];
	[color setLoadAction:MTLLoadActionDontCare];
	[color setStoreAction:MTLStoreActionStore];
	id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
	if (encoder == nil) {
		return false;
	}
	[encoder setRenderPipelineState:pipeline];
	[encoder setFragmentTexture:src atIndex:0];
	[encoder setFragmentSamplerState:pc->sampler atIndex:0];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	return true;
}
