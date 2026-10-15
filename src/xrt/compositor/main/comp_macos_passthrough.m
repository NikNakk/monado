// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_macos_passthrough.h"
#include "main/comp_compositor.h"
#include "util/u_debug.h"
#include "util/u_frame_share.h"
#include "xrt/xrt_frame.h"
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>

#define MACOS_PASSTHROUGH_WIDTH 1024
#define MACOS_PASSTHROUGH_HEIGHT 1016
#define MACOS_PASSTHROUGH_MAP_SIZE 512
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_fov_deg, "XRT_MACOS_PASSTHROUGH_FOV_DEG", 150)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_convergence_milli, "XRT_MACOS_PASSTHROUGH_CONVERGENCE_MILLI", 100)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_brightness_percent, "XRT_MACOS_PASSTHROUGH_BRIGHTNESS_PERCENT", 160)

struct macos_passthrough_sink
{
	struct xrt_frame_sink base;
	struct comp_macos_passthrough *pt;
	uint32_t eye;
};
struct comp_macos_passthrough
{
	struct comp_compositor *c;
	id<MTLDevice> device;
	MTLPixelFormat format;
	bool is_service;
	id<MTLTexture> passthrough_camera_textures[2];
	id<MTLTexture> passthrough_uv_maps[2];
	id<MTLRenderPipelineState> passthrough_pipeline;
	struct macos_passthrough_sink passthrough_sinks[2];
	struct xrt_frame *passthrough_frames[2];
	int64_t passthrough_uploaded_timestamp[2];
	pthread_mutex_t passthrough_mutex;
	atomic_bool passthrough_shutdown;
	bool passthrough_sinks_attached;
	uint64_t encoded_frames;
};

static void
macos_passthrough_sink_push_frame(struct xrt_frame_sink *sink, struct xrt_frame *frame)
{
	struct macos_passthrough_sink *pts = container_of(sink, struct macos_passthrough_sink, base);
	struct comp_macos_passthrough *pt = pts->pt;

	if (atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire)) {
		return;
	}
	if (frame == NULL || frame->format != XRT_FORMAT_BC4 || frame->width != MACOS_PASSTHROUGH_WIDTH ||
	    frame->height != MACOS_PASSTHROUGH_HEIGHT) {
		return;
	}

	pthread_mutex_lock(&pt->passthrough_mutex);
	if (!atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire)) {
		xrt_frame_reference(&pt->passthrough_frames[pts->eye], frame);
	}
	pthread_mutex_unlock(&pt->passthrough_mutex);

	// In the service: also pass the frame to clients compositing in-process.
	u_passthrough_share_push(pts->eye, frame);
}

static bool
macos_passthrough_create_uv_maps(struct comp_macos_passthrough *pt)
{
	struct xrt_device *xdev = pt->c->xdev;
	if (xdev == NULL || xdev->compute_distortion == NULL) {
		return false;
	}

	const float fx = 0.3585564f;
	const float fy = 0.3762281f;
	const float camera_width_ratio = 1016.0f / 1024.0f;
	float fov_deg = (float)debug_get_num_option_macos_passthrough_fov_deg();
	float convergence = (float)debug_get_num_option_macos_passthrough_convergence_milli() / 1000.0f;
	if (fov_deg < 90.0f)
		fov_deg = 90.0f;
	if (fov_deg > 190.0f)
		fov_deg = 190.0f;
	const float fov_rad = fov_deg * 3.14159265358979323846f / 180.0f;

	id<MTLDevice> device = pt->device;
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float
	                                                                                width:MACOS_PASSTHROUGH_MAP_SIZE
	                                                                               height:MACOS_PASSTHROUGH_MAP_SIZE
	                                                                            mipmapped:NO];
	[desc setUsage:MTLTextureUsageShaderRead];
	[desc setStorageMode:MTLStorageModeManaged];

	size_t count = (size_t)MACOS_PASSTHROUGH_MAP_SIZE * MACOS_PASSTHROUGH_MAP_SIZE;
	float *map = malloc(count * 2 * sizeof(float));
	if (map == NULL) {
		return false;
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		const float cx = eye == 0 ? 0.6603788f : 0.3396213f;
		const float shift = eye == 0 ? convergence : -convergence;

		for (uint32_t y = 0; y < MACOS_PASSTHROUGH_MAP_SIZE; y++) {
			float v = ((float)y + 0.5f) / (float)MACOS_PASSTHROUGH_MAP_SIZE;
			for (uint32_t x = 0; x < MACOS_PASSTHROUGH_MAP_SIZE; x++) {
				float u = ((float)x + 0.5f) / (float)MACOS_PASSTHROUGH_MAP_SIZE;
				struct xrt_uv_triplet distortion = {0};
				size_t index = ((size_t)y * MACOS_PASSTHROUGH_MAP_SIZE + x) * 2;

				if (xrt_device_compute_distortion(xdev, eye, u, v, &distortion) != XRT_SUCCESS) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
					continue;
				}

				/* Recover the green-channel tangent ray from the existing
				 * PS VR2 optical distortion mapping, then apply GAV's proven
				 * equidistant camera model. */
				float tan_x = (distortion.g.x - cx) / fx;
				float tan_y_down = (distortion.g.y - 0.5f) / fy;
				float len = sqrtf(tan_x * tan_x + tan_y_down * tan_y_down + 1.0f);
				float dir_x = tan_x / len;
				float dir_y = -tan_y_down / len;
				float neg_dir_z = 1.0f / len;
				float theta = acosf(fmaxf(-1.0f, fminf(1.0f, neg_dir_z)));
				float radius = theta / fov_rad;
				float xy_len = sqrtf(dir_x * dir_x + dir_y * dir_y);

				if (radius > 0.5f || xy_len < 1e-7f) {
					if (radius <= 0.5f && xy_len < 1e-7f) {
						map[index + 0] = (0.5f + shift) * camera_width_ratio;
						map[index + 1] = 0.5f;
					} else {
						map[index + 0] = -1.0f;
						map[index + 1] = -1.0f;
					}
					continue;
				}

				float cam_u = (0.5f + radius * (dir_x / xy_len) + shift) * camera_width_ratio;
				float cam_v = 0.5f - radius * (dir_y / xy_len);
				if (cam_u < 0.0f || cam_u > 1.0f || cam_v < 0.0f || cam_v > 1.0f) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
				} else {
					map[index + 0] = cam_u;
					map[index + 1] = cam_v;
				}
			}
		}

		pt->passthrough_uv_maps[eye] = [device newTextureWithDescriptor:desc];
		if (pt->passthrough_uv_maps[eye] == nil) {
			free(map);
			return false;
		}
		MTLRegion region = MTLRegionMake2D(0, 0, MACOS_PASSTHROUGH_MAP_SIZE, MACOS_PASSTHROUGH_MAP_SIZE);
		[pt->passthrough_uv_maps[eye] replaceRegion:region
		                                mipmapLevel:0
		                                  withBytes:map
		                                bytesPerRow:MACOS_PASSTHROUGH_MAP_SIZE * 2 * sizeof(float)];
	}

	free(map);
	return true;
}

static bool
macos_passthrough_init(struct comp_macos_passthrough *pt)
{
	/*
	 * Check for a camera source first: the UV maps below sample the device's
	 * distortion for every texel, which for an IPC device (a client
	 * compositing in-process) is half a million round trips to the service.
	 * IPC devices have no passthrough sinks, as the cameras are the service's.
	 */
	struct xrt_device *xdev = pt->c->xdev;
	if (xdev == NULL || xdev->set_passthrough_sinks == NULL) {
		COMP_INFO(pt->c, "PS VR2 passthrough unavailable: the head device has no camera source here");
		return false;
	}

	id<MTLDevice> device = pt->device;

	MTLTextureDescriptor *cam_desc =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC4_RUnorm
	                                                       width:MACOS_PASSTHROUGH_WIDTH
	                                                      height:MACOS_PASSTHROUGH_HEIGHT
	                                                   mipmapped:NO];
	[cam_desc setUsage:MTLTextureUsageShaderRead];
	[cam_desc setStorageMode:MTLStorageModeManaged];

	for (uint32_t eye = 0; eye < 2; eye++) {
		pt->passthrough_camera_textures[eye] = [device newTextureWithDescriptor:cam_desc];
		if (pt->passthrough_camera_textures[eye] == nil) {
			COMP_WARN(pt->c, "Could not create PS VR2 BC4 passthrough texture %u", eye);
			return false;
		}
	}

	static const char *shader_source =
	    "#include <metal_stdlib>\n"
	    "using namespace metal;\n"
	    "struct VSOut { float4 pos [[position]]; float2 uv; };\n"
	    "vertex VSOut psvr2_pt_vs(uint vid [[vertex_id]]) {\n"
	    "  float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
	    "  VSOut o; o.pos=float4(p[vid],0,1);"
	    "  o.uv=float2(p[vid].x*0.5+0.5, 1.0-(p[vid].y*0.5+0.5)); return o;\n"
	    "}\n"
	    "struct Params { float brightness; uint has_app; uint map_size; uint pad; };\n"
	    "fragment float4 psvr2_pt_fs(VSOut in [[stage_in]],"
	    " texture2d<float> app [[texture(0)]], texture2d<float> cam_l [[texture(1)]],"
	    " texture2d<float> cam_r [[texture(2)]], texture2d<float, access::read> map_l [[texture(3)]],"
	    " texture2d<float, access::read> map_r [[texture(4)]], constant Params &params [[buffer(0)]]) {\n"
	    "  constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);\n"
	    "  bool right = in.uv.x >= 0.5; float2 local_uv=float2(right ? (in.uv.x-0.5)*2.0 : in.uv.x*2.0, in.uv.y);\n"
	    "  uint2 mi=uint2(min(uint(local_uv.x*float(params.map_size)), params.map_size-1),"
	    "                 min(uint(local_uv.y*float(params.map_size)), params.map_size-1));\n"
	    "  float2 cuv = right ? map_r.read(mi).rg : map_l.read(mi).rg;\n"
	    "  float3 camera=float3(0.0);"
	    "  if (cuv.x >= 0.0 && cuv.y >= 0.0) { float g=(right ? cam_r.sample(s,cuv).r : cam_l.sample(s,cuv).r);"
	    "    camera=float3(saturate(g*params.brightness)); }\n"
	    "  if (params.has_app != 0) { float4 a=app.sample(s,in.uv); return float4(a.rgb + camera*(1.0-a.a), 1.0); "
	    "}\n"
	    "  return float4(camera,1.0);\n"
	    "}\n";

	NSError *error = nil;
	NSString *source = [NSString stringWithUTF8String:shader_source];
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	if (library == nil) {
		COMP_WARN(pt->c, "Could not compile PS VR2 passthrough Metal shader: %s",
		          error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
		return false;
	}
	id<MTLFunction> vs = [library newFunctionWithName:@"psvr2_pt_vs"];
	id<MTLFunction> fs = [library newFunctionWithName:@"psvr2_pt_fs"];
	MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
	[pipeline_desc setVertexFunction:vs];
	[pipeline_desc setFragmentFunction:fs];
	MTLRenderPipelineColorAttachmentDescriptor *pipeline_color =
	    [[pipeline_desc colorAttachments] objectAtIndexedSubscript:0];
	[pipeline_color setPixelFormat:pt->format];
	pt->passthrough_pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
	[pipeline_desc release];
	[vs release];
	[fs release];
	[library release];

	if (pt == NULL || atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire) ||
	    pt->passthrough_pipeline == nil) {
		COMP_WARN(pt->c, "Could not create PS VR2 passthrough Metal pipeline: %s",
		          error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
		return false;
	}
	if (!macos_passthrough_create_uv_maps(pt)) {
		COMP_WARN(pt->c, "Could not create PS VR2 passthrough UV maps");
		return false;
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		pt->passthrough_sinks[eye].base.push_frame = macos_passthrough_sink_push_frame;
		pt->passthrough_sinks[eye].pt = pt;
		pt->passthrough_sinks[eye].eye = eye;
	}

	xrt_result_t xret =
	    xdev->set_passthrough_sinks(xdev, &pt->passthrough_sinks[0].base, &pt->passthrough_sinks[1].base);
	if (xret == XRT_SUCCESS) {
		pt->passthrough_sinks_attached = true;
		// Clients hosted by this service can have the frames too.
		if (pt->is_service) {
			u_passthrough_share_set_source_available(true);
		}
		COMP_INFO(pt->c, "PS VR2 BC4 passthrough attached (FOV %d deg, convergence %.3f)",
		          (int)debug_get_num_option_macos_passthrough_fov_deg(),
		          (double)debug_get_num_option_macos_passthrough_convergence_milli() / 1000.0);
		return true;
	}

	COMP_WARN(pt->c,
	          "PS VR2 passthrough camera source unavailable (set PSVR2_CAMERA_STREAMS=1 before starting Monado)");
	return false;
}

static bool
macos_passthrough_upload(struct comp_macos_passthrough *pt)
{
	struct xrt_frame *frames[2] = {NULL, NULL};

	pthread_mutex_lock(&pt->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&frames[eye], pt->passthrough_frames[eye]);
	}
	pthread_mutex_unlock(&pt->passthrough_mutex);

	bool ready = true;
	for (uint32_t eye = 0; eye < 2; eye++) {
		struct xrt_frame *frame = frames[eye];
		if (frame == NULL) {
			ready = false;
			continue;
		}
		if (frame->timestamp != pt->passthrough_uploaded_timestamp[eye]) {
			MTLRegion region = MTLRegionMake2D(0, 0, MACOS_PASSTHROUGH_WIDTH, MACOS_PASSTHROUGH_HEIGHT);
			[pt->passthrough_camera_textures[eye] replaceRegion:region
			                                        mipmapLevel:0
			                                          withBytes:frame->data
			                                        bytesPerRow:frame->stride];
			pt->passthrough_uploaded_timestamp[eye] = frame->timestamp;
		}
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&frames[eye], NULL);
	}
	return ready;
}

bool
comp_macos_passthrough_ready(struct comp_macos_passthrough *pt)
{
	return pt != NULL && !atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire) &&
	       pt->passthrough_pipeline != nil && pt->passthrough_sinks_attached && macos_passthrough_upload(pt);
}

bool
comp_macos_passthrough_encode(struct comp_macos_passthrough *pt,
                              id<MTLCommandBuffer> command_buffer,
                              id<MTLTexture> target,
                              id<MTLTexture> app_texture,
                              bool has_application_layers)
{
	if (pt == NULL || atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire) ||
	    pt->passthrough_pipeline == nil || !pt->passthrough_sinks_attached || !macos_passthrough_upload(pt)) {
		return false;
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassColorAttachmentDescriptor *color = [[rp colorAttachments] objectAtIndexedSubscript:0];
	[color setTexture:target];
	[color setLoadAction:MTLLoadActionDontCare];
	[color setStoreAction:MTLStoreActionStore];

	id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
	if (encoder == nil) {
		return false;
	}

	struct
	{
		float brightness;
		uint32_t has_app;
		uint32_t map_size;
		uint32_t pad;
	} params = {
	    .brightness = (float)debug_get_num_option_macos_passthrough_brightness_percent() / 100.0f,
	    .has_app = has_application_layers ? 1u : 0u,
	    .map_size = MACOS_PASSTHROUGH_MAP_SIZE,
	    .pad = 0,
	};

	[encoder setRenderPipelineState:pt->passthrough_pipeline];
	[encoder setFragmentTexture:app_texture atIndex:0];
	[encoder setFragmentTexture:pt->passthrough_camera_textures[0] atIndex:1];
	[encoder setFragmentTexture:pt->passthrough_camera_textures[1] atIndex:2];
	[encoder setFragmentTexture:pt->passthrough_uv_maps[0] atIndex:3];
	[encoder setFragmentTexture:pt->passthrough_uv_maps[1] atIndex:4];
	[encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	if (pt->encoded_frames++ == 0) {
		COMP_INFO(pt->c, "Camera passthrough first encoded frame; left=%lld right=%lld",
		          (long long)pt->passthrough_uploaded_timestamp[0],
		          (long long)pt->passthrough_uploaded_timestamp[1]);
	}
	return true;
}


struct comp_macos_passthrough *
comp_macos_passthrough_create(struct comp_compositor *c, id<MTLDevice> device, MTLPixelFormat format, bool is_service)
{
	struct comp_macos_passthrough *pt = calloc(1, sizeof(*pt));
	if (pt == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&pt->passthrough_mutex, NULL) != 0) {
		free(pt);
		return NULL;
	}
	atomic_init(&pt->passthrough_shutdown, false);
	pt->c = c;
	pt->device = [device retain];
	pt->format = format;
	pt->is_service = is_service;
	if (!macos_passthrough_init(pt)) {
		comp_macos_passthrough_destroy(&pt);
	}
	return pt;
}

void
comp_macos_passthrough_stop(struct comp_macos_passthrough *pt)
{
	if (pt == NULL) {
		return;
	}
	atomic_store_explicit(&pt->passthrough_shutdown, true, memory_order_release);
	struct xrt_device *xdev = pt->c->xdev;
	if (pt->passthrough_sinks_attached && xdev != NULL && xdev->set_passthrough_sinks != NULL) {
		(void)xdev->set_passthrough_sinks(xdev, NULL, NULL);
		pt->passthrough_sinks_attached = false;
		if (pt->is_service) {
			u_passthrough_share_set_source_available(false);
		}
	}
}

void
comp_macos_passthrough_destroy(struct comp_macos_passthrough **ptr)
{
	struct comp_macos_passthrough *pt = *ptr;
	if (pt == NULL) {
		return;
	}
	comp_macos_passthrough_stop(pt);
	COMP_INFO(pt->c, "Camera passthrough encoded frames: %llu", (unsigned long long)pt->encoded_frames);
	pthread_mutex_lock(&pt->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&pt->passthrough_frames[eye], NULL);
	}
	pthread_mutex_unlock(&pt->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++) {
		[pt->passthrough_camera_textures[eye] release];
		[pt->passthrough_uv_maps[eye] release];
	}
	[pt->passthrough_pipeline release];
	[pt->device release];
	pthread_mutex_destroy(&pt->passthrough_mutex);
	free(pt);
	*ptr = NULL;
}
