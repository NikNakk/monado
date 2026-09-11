// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Standalone macOS OpenXR/Metal scene for PS VR2 motion diagnostics.
 *
 * This deliberately depends only on Monado's bundled OpenXR headers at build
 * time. The Khronos OpenXR loader is opened with dlopen at runtime, then the
 * application talks to whichever runtime XR_RUNTIME_JSON selects.
 */

#define XR_USE_GRAPHICS_API_METAL 1
#define XR_NO_PROTOTYPES 1

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <simd/simd.h>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef PSVR2_OPENXR_LOADER_DEFAULT
#define PSVR2_OPENXR_LOADER_DEFAULT ""
#endif

static volatile sig_atomic_t g_stop_requested = 0;

static void
handle_signal(int signal_number)
{
	(void)signal_number;
	g_stop_requested = 1;
}

[[noreturn]] static void
fatal(const char *message)
{
	fprintf(stderr, "psvr2-openxr-test: %s\n", message);
	exit(EXIT_FAILURE);
}

static void
check_xr(XrResult result, const char *operation)
{
	if (XR_FAILED(result)) {
		fprintf(stderr, "psvr2-openxr-test: %s failed with XrResult %d\n", operation, (int)result);
		exit(EXIT_FAILURE);
	}
}

struct xr_api
{
	PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties enumerate_instance_extension_properties = nullptr;
	PFN_xrCreateInstance create_instance = nullptr;
	PFN_xrDestroyInstance destroy_instance = nullptr;
	PFN_xrGetInstanceProperties get_instance_properties = nullptr;
	PFN_xrGetSystem get_system = nullptr;
	PFN_xrGetSystemProperties get_system_properties = nullptr;
	PFN_xrGetMetalGraphicsRequirementsKHR get_metal_graphics_requirements = nullptr;
	PFN_xrCreateSession create_session = nullptr;
	PFN_xrDestroySession destroy_session = nullptr;
	PFN_xrEnumerateViewConfigurationViews enumerate_view_configuration_views = nullptr;
	PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats = nullptr;
	PFN_xrCreateSwapchain create_swapchain = nullptr;
	PFN_xrDestroySwapchain destroy_swapchain = nullptr;
	PFN_xrEnumerateSwapchainImages enumerate_swapchain_images = nullptr;
	PFN_xrCreateReferenceSpace create_reference_space = nullptr;
	PFN_xrDestroySpace destroy_space = nullptr;
	PFN_xrLocateSpace locate_space = nullptr;
	PFN_xrPollEvent poll_event = nullptr;
	PFN_xrBeginSession begin_session = nullptr;
	PFN_xrEndSession end_session = nullptr;
	PFN_xrWaitFrame wait_frame = nullptr;
	PFN_xrBeginFrame begin_frame = nullptr;
	PFN_xrEndFrame end_frame = nullptr;
	PFN_xrLocateViews locate_views = nullptr;
	PFN_xrAcquireSwapchainImage acquire_swapchain_image = nullptr;
	PFN_xrWaitSwapchainImage wait_swapchain_image = nullptr;
	PFN_xrReleaseSwapchainImage release_swapchain_image = nullptr;
};

template <typename T>
static void
load_xr_proc(PFN_xrGetInstanceProcAddr get_instance_proc_addr,
             XrInstance instance,
             const char *name,
             T *out_function)
{
	PFN_xrVoidFunction function = nullptr;
	XrResult result = get_instance_proc_addr(instance, name, &function);
	if (XR_FAILED(result) || function == nullptr) {
		fprintf(stderr, "psvr2-openxr-test: could not load %s (XrResult %d)\n", name, (int)result);
		exit(EXIT_FAILURE);
	}
	*out_function = reinterpret_cast<T>(function);
}

struct loader_handle
{
	void *handle = nullptr;
	std::string path;
};

static loader_handle
open_openxr_loader(const char *command_line_path)
{
	std::vector<std::string> candidates;
	if (command_line_path != nullptr && command_line_path[0] != '\0') {
		candidates.emplace_back(command_line_path);
	}
	const char *environment_path = getenv("PSVR2_OPENXR_LOADER");
	if (environment_path != nullptr && environment_path[0] != '\0') {
		candidates.emplace_back(environment_path);
	}
	if (PSVR2_OPENXR_LOADER_DEFAULT[0] != '\0') {
		candidates.emplace_back(PSVR2_OPENXR_LOADER_DEFAULT);
	}
	candidates.emplace_back("libopenxr_loader.1.dylib");
	candidates.emplace_back("libopenxr_loader.dylib");
	candidates.emplace_back("/opt/homebrew/lib/libopenxr_loader.1.dylib");
	candidates.emplace_back("/opt/homebrew/lib/libopenxr_loader.dylib");
	candidates.emplace_back("/usr/local/lib/libopenxr_loader.1.dylib");
	candidates.emplace_back("/usr/local/lib/libopenxr_loader.dylib");

	for (const std::string &candidate : candidates) {
		void *handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (handle != nullptr) {
			return {handle, candidate};
		}
	}

	fprintf(stderr,
	        "psvr2-openxr-test: could not find the Khronos OpenXR loader.\n"
	        "Set PSVR2_OPENXR_LOADER=/path/to/libopenxr_loader.1.dylib or pass --loader PATH.\n");
	exit(EXIT_FAILURE);
}

static void
load_global_xr_functions(loader_handle &loader, xr_api &xr)
{
	void *symbol = dlsym(loader.handle, "xrGetInstanceProcAddr");
	if (symbol == nullptr) {
		fatal("OpenXR loader does not export xrGetInstanceProcAddr");
	}
	xr.get_instance_proc_addr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(symbol);
	load_xr_proc(xr.get_instance_proc_addr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
	             &xr.enumerate_instance_extension_properties);
	load_xr_proc(xr.get_instance_proc_addr, XR_NULL_HANDLE, "xrCreateInstance", &xr.create_instance);
}

static void
load_instance_xr_functions(xr_api &xr, XrInstance instance)
{
#define LOAD_XR(name, member) load_xr_proc(xr.get_instance_proc_addr, instance, name, &xr.member)
	LOAD_XR("xrDestroyInstance", destroy_instance);
	LOAD_XR("xrGetInstanceProperties", get_instance_properties);
	LOAD_XR("xrGetSystem", get_system);
	LOAD_XR("xrGetSystemProperties", get_system_properties);
	LOAD_XR("xrGetMetalGraphicsRequirementsKHR", get_metal_graphics_requirements);
	LOAD_XR("xrCreateSession", create_session);
	LOAD_XR("xrDestroySession", destroy_session);
	LOAD_XR("xrEnumerateViewConfigurationViews", enumerate_view_configuration_views);
	LOAD_XR("xrEnumerateSwapchainFormats", enumerate_swapchain_formats);
	LOAD_XR("xrCreateSwapchain", create_swapchain);
	LOAD_XR("xrDestroySwapchain", destroy_swapchain);
	LOAD_XR("xrEnumerateSwapchainImages", enumerate_swapchain_images);
	LOAD_XR("xrCreateReferenceSpace", create_reference_space);
	LOAD_XR("xrDestroySpace", destroy_space);
	LOAD_XR("xrLocateSpace", locate_space);
	LOAD_XR("xrPollEvent", poll_event);
	LOAD_XR("xrBeginSession", begin_session);
	LOAD_XR("xrEndSession", end_session);
	LOAD_XR("xrWaitFrame", wait_frame);
	LOAD_XR("xrBeginFrame", begin_frame);
	LOAD_XR("xrEndFrame", end_frame);
	LOAD_XR("xrLocateViews", locate_views);
	LOAD_XR("xrAcquireSwapchainImage", acquire_swapchain_image);
	LOAD_XR("xrWaitSwapchainImage", wait_swapchain_image);
	LOAD_XR("xrReleaseSwapchainImage", release_swapchain_image);
#undef LOAD_XR
}

static simd_float3
make_float3(float x, float y, float z)
{
	simd_float3 value = {x, y, z};
	return value;
}

static simd_float4
make_float4(float x, float y, float z, float w)
{
	simd_float4 value = {x, y, z, w};
	return value;
}

static simd_float3
xr_position(const XrVector3f &position)
{
	return make_float3(position.x, position.y, position.z);
}

static simd_float3
rotate_vector(const XrQuaternionf &orientation, simd_float3 vector)
{
	simd_float3 q = make_float3(orientation.x, orientation.y, orientation.z);
	simd_float3 twice_cross = 2.0f * simd_cross(q, vector);
	return vector + orientation.w * twice_cross + simd_cross(q, twice_cross);
}

static matrix_float4x4
projection_matrix(const XrFovf &fov, float near_z, float far_z)
{
	const float left = tanf(fov.angleLeft);
	const float right = tanf(fov.angleRight);
	const float down = tanf(fov.angleDown);
	const float up = tanf(fov.angleUp);
	const float width = right - left;
	const float height = up - down;

	matrix_float4x4 matrix = {};
	matrix.columns[0] = make_float4(2.0f / width, 0.0f, 0.0f, 0.0f);
	matrix.columns[1] = make_float4(0.0f, 2.0f / height, 0.0f, 0.0f);
	matrix.columns[2] = make_float4((right + left) / width, (up + down) / height,
	                                far_z / (near_z - far_z), -1.0f);
	matrix.columns[3] = make_float4(0.0f, 0.0f, (far_z * near_z) / (near_z - far_z), 0.0f);
	return matrix;
}

static matrix_float4x4
view_matrix(const XrPosef &pose)
{
	const simd_float3 right = rotate_vector(pose.orientation, make_float3(1.0f, 0.0f, 0.0f));
	const simd_float3 up = rotate_vector(pose.orientation, make_float3(0.0f, 1.0f, 0.0f));
	const simd_float3 back = rotate_vector(pose.orientation, make_float3(0.0f, 0.0f, 1.0f));
	const simd_float3 position = xr_position(pose.position);

	matrix_float4x4 matrix = {};
	matrix.columns[0] = make_float4(right.x, up.x, back.x, 0.0f);
	matrix.columns[1] = make_float4(right.y, up.y, back.y, 0.0f);
	matrix.columns[2] = make_float4(right.z, up.z, back.z, 0.0f);
	matrix.columns[3] = make_float4(-simd_dot(right, position), -simd_dot(up, position),
	                                -simd_dot(back, position), 1.0f);
	return matrix;
}

struct alignas(16) instance_data
{
	matrix_float4x4 model;
	simd_float4 color;
};

struct diagnostic_scene
{
	bool initialized = false;
	simd_float3 origin = {};
	simd_float3 right = {};
	simd_float3 up = {};
	simd_float3 forward = {};
	std::vector<instance_data> world_instances;
};

static matrix_float4x4
basis_model(simd_float3 position,
            simd_float3 right,
            simd_float3 up,
            simd_float3 back,
            simd_float3 scale)
{
	matrix_float4x4 matrix = {};
	matrix.columns[0] = make_float4(right.x * scale.x, right.y * scale.x, right.z * scale.x, 0.0f);
	matrix.columns[1] = make_float4(up.x * scale.y, up.y * scale.y, up.z * scale.y, 0.0f);
	matrix.columns[2] = make_float4(back.x * scale.z, back.y * scale.z, back.z * scale.z, 0.0f);
	matrix.columns[3] = make_float4(position.x, position.y, position.z, 1.0f);
	return matrix;
}

static void
add_world_box(diagnostic_scene &scene,
              float x,
              float y,
              float z,
              simd_float3 scale,
              simd_float4 color)
{
	const simd_float3 position = scene.origin + scene.right * x + scene.up * y + scene.forward * z;
	const simd_float3 back = -scene.forward;
	scene.world_instances.push_back({basis_model(position, scene.right, scene.up, back, scale), color});
}

static void
initialize_scene(diagnostic_scene &scene, const XrPosef &head_pose)
{
	scene.origin = xr_position(head_pose.position);
	scene.up = make_float3(0.0f, 1.0f, 0.0f);
	scene.forward = rotate_vector(head_pose.orientation, make_float3(0.0f, 0.0f, -1.0f));
	scene.forward.y = 0.0f;
	if (simd_length(scene.forward) < 0.001f) {
		scene.forward = make_float3(0.0f, 0.0f, -1.0f);
	} else {
		scene.forward = simd_normalize(scene.forward);
	}
	scene.right = simd_normalize(simd_cross(scene.forward, scene.up));

	const simd_float4 grid_color = make_float4(0.12f, 0.38f, 0.42f, 1.0f);
	const simd_float4 marker_color = make_float4(0.82f, 0.86f, 0.90f, 1.0f);
	const simd_float4 center_color = make_float4(1.0f, 0.78f, 0.16f, 1.0f);
	const simd_float4 near_color = make_float4(0.28f, 0.92f, 0.46f, 1.0f);
	const simd_float4 mid_color = make_float4(0.22f, 0.64f, 1.0f, 1.0f);
	const simd_float4 far_color = make_float4(0.86f, 0.42f, 0.98f, 1.0f);

	// Dotted-looking floor grid made from very thin boxes. It gives strong
	// translation/parallax cues without using textures or line rendering.
	const float floor_y = -0.80f;
	for (int x_step = -5; x_step <= 5; ++x_step) {
		add_world_box(scene, 0.5f * (float)x_step, floor_y, 3.25f, make_float3(0.008f, 0.008f, 5.5f),
		              grid_color);
	}
	for (int z_step = 1; z_step <= 12; ++z_step) {
		add_world_box(scene, 0.0f, floor_y, 0.5f * (float)z_step, make_float3(5.0f, 0.008f, 0.008f),
		              grid_color);
	}

	// Symmetric equal-radius markers. During yaw these give comparable centre
	// and peripheral targets on the left and right sides of the display.
	for (int angle_degrees = -40; angle_degrees <= 40; angle_degrees += 10) {
		const float radians = (float)angle_degrees * (float)M_PI / 180.0f;
		const float radius = 2.4f;
		const float x = sinf(radians) * radius;
		const float z = cosf(radians) * radius;
		for (int y_step = -2; y_step <= 2; ++y_step) {
			const bool centre = angle_degrees == 0 && y_step == 0;
			add_world_box(scene, x, 0.30f * (float)y_step, z,
			              make_float3(centre ? 0.085f : 0.050f, centre ? 0.085f : 0.050f,
			                          centre ? 0.085f : 0.050f),
			              centre ? center_color : marker_color);
		}
	}

	// Constant-angular-size depth targets at three azimuths. Their physical
	// size grows with distance so translation can be compared without an obvious
	// size cue dominating the observation.
	const std::array<float, 4> distances = {0.75f, 1.5f, 3.0f, 6.0f};
	const std::array<float, 3> angles = {-25.0f, 0.0f, 25.0f};
	for (size_t distance_index = 0; distance_index < distances.size(); ++distance_index) {
		const float distance = distances[distance_index];
		const float size = std::max(0.026f, distance * 0.035f);
		const simd_float4 color = distance_index == 0 ? near_color
		                              : distance_index < 3 ? mid_color
		                                                   : far_color;
		for (float angle_degrees : angles) {
			const float radians = angle_degrees * (float)M_PI / 180.0f;
			add_world_box(scene, sinf(radians) * distance, 0.58f, cosf(radians) * distance,
			              make_float3(size, size, size), color);
		}
	}

	// World-locked fixation cross and symmetric peripheral vertical references.
	add_world_box(scene, 0.0f, 0.0f, 2.0f, make_float3(0.30f, 0.018f, 0.018f), center_color);
	add_world_box(scene, 0.0f, 0.0f, 2.0f, make_float3(0.018f, 0.30f, 0.018f), center_color);
	for (float angle_degrees : {-45.0f, 45.0f}) {
		const float radians = angle_degrees * (float)M_PI / 180.0f;
		add_world_box(scene, sinf(radians) * 2.5f, 0.0f, cosf(radians) * 2.5f,
		              make_float3(0.035f, 1.35f, 0.035f), marker_color);
	}

	scene.initialized = true;
	fprintf(stderr, "psvr2-openxr-test: diagnostic world contains %zu world-locked boxes\n",
	        scene.world_instances.size());
}

static void
append_head_locked_cross(std::vector<instance_data> &instances, const XrPosef &head_pose)
{
	const simd_float3 head_position = xr_position(head_pose.position);
	const simd_float3 right = rotate_vector(head_pose.orientation, make_float3(1.0f, 0.0f, 0.0f));
	const simd_float3 up = rotate_vector(head_pose.orientation, make_float3(0.0f, 1.0f, 0.0f));
	const simd_float3 back = rotate_vector(head_pose.orientation, make_float3(0.0f, 0.0f, 1.0f));
	const simd_float3 centre = head_position + rotate_vector(head_pose.orientation, make_float3(0.0f, 0.0f, -0.55f));
	const simd_float4 color = make_float4(1.0f, 0.12f, 0.55f, 1.0f);

	instances.push_back({basis_model(centre, right, up, back, make_float3(0.090f, 0.006f, 0.006f)), color});
	instances.push_back({basis_model(centre, right, up, back, make_float3(0.006f, 0.090f, 0.006f)), color});
	instances.push_back({basis_model(centre, right, up, back, make_float3(0.012f, 0.012f, 0.012f)), color});
}

static const simd_float4 k_cube_vertices[] = {
    // -Z
    {-0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, -0.5f, 1.0f},
    {-0.5f, -0.5f, -0.5f, 1.0f}, {-0.5f, 0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, -0.5f, 1.0f},
    // +Z
    {-0.5f, -0.5f, 0.5f, 1.0f}, {0.5f, -0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f},
    {-0.5f, -0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {-0.5f, 0.5f, 0.5f, 1.0f},
    // -X
    {-0.5f, -0.5f, -0.5f, 1.0f}, {-0.5f, -0.5f, 0.5f, 1.0f}, {-0.5f, 0.5f, 0.5f, 1.0f},
    {-0.5f, -0.5f, -0.5f, 1.0f}, {-0.5f, 0.5f, 0.5f, 1.0f}, {-0.5f, 0.5f, -0.5f, 1.0f},
    // +X
    {0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, -0.5f, 0.5f, 1.0f},
    {0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f},
    // -Y
    {-0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, 0.5f, 1.0f}, {-0.5f, -0.5f, 0.5f, 1.0f},
    {-0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, 0.5f, 1.0f},
    // +Y
    {-0.5f, 0.5f, -0.5f, 1.0f}, {-0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f},
    {-0.5f, 0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, -0.5f, 1.0f},
};

static const char *k_metal_shader = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct InstanceData {
    float4x4 model;
    float4 color;
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut vertex_main(uint vertex_id [[vertex_id]],
                             uint instance_id [[instance_id]],
                             const device float4 *vertices [[buffer(0)]],
                             const device InstanceData *instances [[buffer(1)]],
                             constant float4x4 &view_projection [[buffer(2)]])
{
    VertexOut out;
    out.position = view_projection * instances[instance_id].model * vertices[vertex_id];
    out.color = instances[instance_id].color;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]])
{
    return in.color;
}
)METAL";

struct metal_renderer
{
	id<MTLRenderPipelineState> pipeline = nil;
	id<MTLDepthStencilState> depth_state = nil;
	id<MTLBuffer> cube_vertex_buffer = nil;
	id<MTLBuffer> instance_buffer = nil;
	size_t max_instances = 256;

	void initialize(id<MTLDevice> device, MTLPixelFormat color_format)
	{
		NSError *error = nil;
		NSString *source = [NSString stringWithUTF8String:k_metal_shader];
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		if (library == nil) {
			fprintf(stderr, "psvr2-openxr-test: Metal shader compilation failed: %s\n",
			        error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
			exit(EXIT_FAILURE);
		}
		id<MTLFunction> vertex_function = [library newFunctionWithName:@"vertex_main"];
		id<MTLFunction> fragment_function = [library newFunctionWithName:@"fragment_main"];
		if (vertex_function == nil || fragment_function == nil) {
			fatal("could not find compiled Metal shader entry points");
		}

		MTLRenderPipelineDescriptor *pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
		pipeline_descriptor.vertexFunction = vertex_function;
		pipeline_descriptor.fragmentFunction = fragment_function;
		pipeline_descriptor.colorAttachments[0].pixelFormat = color_format;
		pipeline_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
		pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
		[pipeline_descriptor release];
		[vertex_function release];
		[fragment_function release];
		[library release];
		if (pipeline == nil) {
			fprintf(stderr, "psvr2-openxr-test: Metal pipeline creation failed: %s\n",
			        error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
			exit(EXIT_FAILURE);
		}

		MTLDepthStencilDescriptor *depth_descriptor = [[MTLDepthStencilDescriptor alloc] init];
		depth_descriptor.depthCompareFunction = MTLCompareFunctionLess;
		depth_descriptor.depthWriteEnabled = YES;
		depth_state = [device newDepthStencilStateWithDescriptor:depth_descriptor];
		[depth_descriptor release];
		if (depth_state == nil) {
			fatal("could not create Metal depth state");
		}

		cube_vertex_buffer = [device newBufferWithBytes:k_cube_vertices
		                                      length:sizeof(k_cube_vertices)
		                                     options:MTLResourceStorageModeShared];
		instance_buffer = [device newBufferWithLength:max_instances * sizeof(instance_data)
		                                  options:MTLResourceStorageModeShared];
		if (cube_vertex_buffer == nil || instance_buffer == nil) {
			fatal("could not allocate Metal geometry buffers");
		}
	}

	void shutdown()
	{
		[instance_buffer release];
		instance_buffer = nil;
		[cube_vertex_buffer release];
		cube_vertex_buffer = nil;
		[depth_state release];
		depth_state = nil;
		[pipeline release];
		pipeline = nil;
	}
};

struct view_swapchain
{
	XrSwapchain handle = XR_NULL_HANDLE;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<XrSwapchainImageMetalKHR> images;
	id<MTLTexture> depth_texture = nil;
};

struct application
{
	loader_handle loader;
	xr_api xr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace app_space = XR_NULL_HANDLE;
	XrSpace view_space = XR_NULL_HANDLE;
	XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
	bool session_running = false;
	bool exit_requested = false;
	id<MTLCommandQueue> command_queue = nil;
	MTLPixelFormat color_format = MTLPixelFormatInvalid;
	std::vector<XrViewConfigurationView> view_configuration;
	std::vector<XrView> views;
	std::vector<XrCompositionLayerProjectionView> projection_views;
	std::vector<view_swapchain> swapchains;
	metal_renderer renderer;
	diagnostic_scene scene;
	std::vector<instance_data> frame_instances;
};

static bool
has_extension(const xr_api &xr, const char *extension_name)
{
	uint32_t extension_count = 0;
	check_xr(xr.enumerate_instance_extension_properties(nullptr, 0, &extension_count, nullptr),
	         "xrEnumerateInstanceExtensionProperties(count)");
	std::vector<XrExtensionProperties> extensions(extension_count);
	for (XrExtensionProperties &extension : extensions) {
		extension = {XR_TYPE_EXTENSION_PROPERTIES};
	}
	check_xr(xr.enumerate_instance_extension_properties(nullptr, extension_count, &extension_count, extensions.data()),
	         "xrEnumerateInstanceExtensionProperties(list)");
	for (const XrExtensionProperties &extension : extensions) {
		if (strcmp(extension.extensionName, extension_name) == 0) {
			return true;
		}
	}
	return false;
}

static void
create_instance(application &app)
{
	if (!has_extension(app.xr, XR_KHR_METAL_ENABLE_EXTENSION_NAME)) {
		fatal("runtime does not expose XR_KHR_metal_enable");
	}
	const char *extensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "PSVR2 OpenXR Test");
	create_info.applicationInfo.applicationVersion = 1;
	snprintf(create_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "Monado diagnostic");
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create_info.enabledExtensionCount = 1;
	create_info.enabledExtensionNames = extensions;
	check_xr(app.xr.create_instance(&create_info, &app.instance), "xrCreateInstance");
	load_instance_xr_functions(app.xr, app.instance);

	XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
	check_xr(app.xr.get_instance_properties(app.instance, &instance_properties), "xrGetInstanceProperties");
	fprintf(stderr, "psvr2-openxr-test: runtime %s %u.%u.%u\n", instance_properties.runtimeName,
	        XR_VERSION_MAJOR(instance_properties.runtimeVersion), XR_VERSION_MINOR(instance_properties.runtimeVersion),
	        XR_VERSION_PATCH(instance_properties.runtimeVersion));
}

static void
create_system_and_session(application &app)
{
	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	check_xr(app.xr.get_system(app.instance, &system_info, &app.system_id), "xrGetSystem");

	XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
	check_xr(app.xr.get_system_properties(app.instance, app.system_id, &properties), "xrGetSystemProperties");
	fprintf(stderr, "psvr2-openxr-test: system %s\n", properties.systemName);

	XrGraphicsRequirementsMetalKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
	check_xr(app.xr.get_metal_graphics_requirements(app.instance, app.system_id, &requirements),
	         "xrGetMetalGraphicsRequirementsKHR");
	id<MTLDevice> device = (__bridge id<MTLDevice>)requirements.metalDevice;
	if (device == nil) {
		fatal("runtime returned a nil Metal device");
	}
	app.command_queue = [device newCommandQueue];
	if (app.command_queue == nil) {
		fatal("could not create Metal command queue");
	}

	XrGraphicsBindingMetalKHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_METAL_KHR};
	graphics_binding.commandQueue = (__bridge void *)app.command_queue;
	XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
	session_info.next = &graphics_binding;
	session_info.systemId = app.system_id;
	check_xr(app.xr.create_session(app.instance, &session_info, &app.session), "xrCreateSession");

	XrReferenceSpaceCreateInfo local_space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	local_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	local_space_info.poseInReferenceSpace.orientation.w = 1.0f;
	check_xr(app.xr.create_reference_space(app.session, &local_space_info, &app.app_space),
	         "xrCreateReferenceSpace(LOCAL)");

	XrReferenceSpaceCreateInfo view_space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	view_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	view_space_info.poseInReferenceSpace.orientation.w = 1.0f;
	check_xr(app.xr.create_reference_space(app.session, &view_space_info, &app.view_space),
	         "xrCreateReferenceSpace(VIEW)");
}

static MTLPixelFormat
choose_swapchain_format(application &app)
{
	uint32_t format_count = 0;
	check_xr(app.xr.enumerate_swapchain_formats(app.session, 0, &format_count, nullptr),
	         "xrEnumerateSwapchainFormats(count)");
	std::vector<int64_t> formats(format_count);
	check_xr(app.xr.enumerate_swapchain_formats(app.session, format_count, &format_count, formats.data()),
	         "xrEnumerateSwapchainFormats(list)");

	const std::array<MTLPixelFormat, 4> preferred = {
	    MTLPixelFormatBGRA8Unorm,
	    MTLPixelFormatRGBA8Unorm,
	    MTLPixelFormatBGRA8Unorm_sRGB,
	    MTLPixelFormatRGBA8Unorm_sRGB,
	};
	for (MTLPixelFormat candidate : preferred) {
		if (std::find(formats.begin(), formats.end(), (int64_t)candidate) != formats.end()) {
			return candidate;
		}
	}
	fatal("runtime did not expose a supported 8-bit Metal swapchain format");
}

static void
create_swapchains(application &app)
{
	uint32_t view_count = 0;
	check_xr(app.xr.enumerate_view_configuration_views(app.instance, app.system_id,
	                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count,
	                                                   nullptr),
	         "xrEnumerateViewConfigurationViews(count)");
	if (view_count != 2) {
		fatal("diagnostic application currently requires PRIMARY_STEREO with two views");
	}
	app.view_configuration.resize(view_count);
	for (XrViewConfigurationView &view : app.view_configuration) {
		view = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	}
	check_xr(app.xr.enumerate_view_configuration_views(app.instance, app.system_id,
	                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
	                                                   &view_count, app.view_configuration.data()),
	         "xrEnumerateViewConfigurationViews(list)");

	app.color_format = choose_swapchain_format(app);
	app.swapchains.resize(view_count);
	id<MTLDevice> device = [app.command_queue device];
	for (uint32_t i = 0; i < view_count; ++i) {
		view_swapchain &swapchain = app.swapchains[i];
		swapchain.width = app.view_configuration[i].recommendedImageRectWidth;
		swapchain.height = app.view_configuration[i].recommendedImageRectHeight;

		XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		create_info.format = (int64_t)app.color_format;
		create_info.sampleCount = 1;
		create_info.width = swapchain.width;
		create_info.height = swapchain.height;
		create_info.faceCount = 1;
		create_info.arraySize = 1;
		create_info.mipCount = 1;
		check_xr(app.xr.create_swapchain(app.session, &create_info, &swapchain.handle), "xrCreateSwapchain");

		uint32_t image_count = 0;
		check_xr(app.xr.enumerate_swapchain_images(swapchain.handle, 0, &image_count, nullptr),
		         "xrEnumerateSwapchainImages(count)");
		swapchain.images.resize(image_count);
		for (XrSwapchainImageMetalKHR &image : swapchain.images) {
			image = {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR};
		}
		check_xr(app.xr.enumerate_swapchain_images(
		             swapchain.handle, image_count, &image_count,
		             reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchain.images.data())),
		         "xrEnumerateSwapchainImages(list)");

		MTLTextureDescriptor *depth_descriptor =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
		                                                      width:swapchain.width
		                                                     height:swapchain.height
		                                                  mipmapped:NO];
		depth_descriptor.usage = MTLTextureUsageRenderTarget;
		depth_descriptor.storageMode = MTLStorageModePrivate;
		swapchain.depth_texture = [device newTextureWithDescriptor:depth_descriptor];
		if (swapchain.depth_texture == nil) {
			fatal("could not create per-view Metal depth texture");
		}
	}

	app.views.resize(view_count);
	app.projection_views.resize(view_count);
	for (uint32_t i = 0; i < view_count; ++i) {
		app.views[i] = {XR_TYPE_VIEW};
		app.projection_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
	}
	app.frame_instances.reserve(app.renderer.max_instances);
	app.renderer.initialize(device, app.color_format);

	fprintf(stderr, "psvr2-openxr-test: %u views, %ux%u per eye, Metal format %lld\n", view_count,
	        app.swapchains[0].width, app.swapchains[0].height, (long long)app.color_format);
}

static XrPosef
head_pose_for_frame(application &app, XrTime predicted_display_time)
{
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	XrResult result = app.xr.locate_space(app.view_space, app.app_space, predicted_display_time, &location);
	if (XR_SUCCEEDED(result) &&
	    (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
	        (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return location.pose;
	}

	XrPosef fallback = app.views[0].pose;
	fallback.position.x = 0.5f * (app.views[0].pose.position.x + app.views[1].pose.position.x);
	fallback.position.y = 0.5f * (app.views[0].pose.position.y + app.views[1].pose.position.y);
	fallback.position.z = 0.5f * (app.views[0].pose.position.z + app.views[1].pose.position.z);
	return fallback;
}

static void
render_views(application &app, XrTime predicted_display_time)
{
	const XrPosef head_pose = head_pose_for_frame(app, predicted_display_time);
	if (!app.scene.initialized) {
		initialize_scene(app.scene, head_pose);
	}
	app.frame_instances = app.scene.world_instances;
	append_head_locked_cross(app.frame_instances, head_pose);
	if (app.frame_instances.size() > app.renderer.max_instances) {
		fatal("diagnostic scene exceeded Metal instance buffer capacity");
	}
	memcpy([app.renderer.instance_buffer contents], app.frame_instances.data(),
	       app.frame_instances.size() * sizeof(instance_data));

	std::vector<uint32_t> image_indices(app.swapchains.size(), 0);
	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].handle, &acquire_info, &image_indices[i]),
		         "xrAcquireSwapchainImage");
		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		check_xr(app.xr.wait_swapchain_image(app.swapchains[i].handle, &wait_info), "xrWaitSwapchainImage");
	}

	id<MTLCommandBuffer> command_buffer = [app.command_queue commandBuffer];
	if (command_buffer == nil) {
		fatal("could not allocate Metal command buffer");
	}

	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		view_swapchain &swapchain = app.swapchains[i];
		if (image_indices[i] >= swapchain.images.size()) {
			fatal("OpenXR returned an out-of-range swapchain image index");
		}
		id<MTLTexture> color_texture = (__bridge id<MTLTexture>)swapchain.images[image_indices[i]].texture;
		if (color_texture == nil) {
			fatal("OpenXR returned a nil Metal swapchain texture");
		}

		MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
		render_pass.colorAttachments[0].texture = color_texture;
		render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		render_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.012, 0.018, 0.024, 1.0);
		render_pass.depthAttachment.texture = swapchain.depth_texture;
		render_pass.depthAttachment.loadAction = MTLLoadActionClear;
		render_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
		render_pass.depthAttachment.clearDepth = 1.0;

		id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:render_pass];
		if (encoder == nil) {
			fatal("could not create Metal render command encoder");
		}
		[encoder setRenderPipelineState:app.renderer.pipeline];
		[encoder setDepthStencilState:app.renderer.depth_state];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setViewport:(MTLViewport){0.0, 0.0, (double)swapchain.width, (double)swapchain.height, 0.0, 1.0}];
		[encoder setVertexBuffer:app.renderer.cube_vertex_buffer offset:0 atIndex:0];
		[encoder setVertexBuffer:app.renderer.instance_buffer offset:0 atIndex:1];
		const matrix_float4x4 projection = projection_matrix(app.views[i].fov, 0.05f, 100.0f);
		const matrix_float4x4 view = view_matrix(app.views[i].pose);
		const matrix_float4x4 view_projection = simd_mul(projection, view);
		[encoder setVertexBytes:&view_projection length:sizeof(view_projection) atIndex:2];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle
		            vertexStart:0
		            vertexCount:sizeof(k_cube_vertices) / sizeof(k_cube_vertices[0])
		          instanceCount:app.frame_instances.size()];
		[encoder endEncoding];
	}
	[command_buffer commit];

	for (view_swapchain &swapchain : app.swapchains) {
		XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		check_xr(app.xr.release_swapchain_image(swapchain.handle, &release_info), "xrReleaseSwapchainImage");
	}
}

static bool
poll_events(application &app)
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (app.xr.poll_event(app.instance, &event) == XR_SUCCESS) {
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged *state_changed =
			    reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
			app.session_state = state_changed->state;
			if (state_changed->state == XR_SESSION_STATE_READY && !app.session_running) {
				XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
				begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				check_xr(app.xr.begin_session(app.session, &begin_info), "xrBeginSession");
				app.session_running = true;
				fprintf(stderr, "psvr2-openxr-test: session running; Ctrl-C to quit\n");
			} else if (state_changed->state == XR_SESSION_STATE_STOPPING && app.session_running) {
				check_xr(app.xr.end_session(app.session), "xrEndSession");
				app.session_running = false;
			} else if (state_changed->state == XR_SESSION_STATE_EXITING ||
			           state_changed->state == XR_SESSION_STATE_LOSS_PENDING) {
				return false;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: return false;
		default: break;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
	return true;
}

static void
render_frame(application &app)
{
	XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	check_xr(app.xr.wait_frame(app.session, &wait_info, &frame_state), "xrWaitFrame");
	XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
	check_xr(app.xr.begin_frame(app.session, &begin_info), "xrBeginFrame");

	bool submit_projection = false;
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	if (frame_state.shouldRender == XR_TRUE) {
		XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = app.app_space;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;
		check_xr(app.xr.locate_views(app.session, &locate_info, &view_state, (uint32_t)app.views.size(), &view_count,
		                             app.views.data()),
		         "xrLocateViews");
		const XrViewStateFlags required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		if (view_count == app.views.size() && (view_state.viewStateFlags & required) == required) {
			@autoreleasepool {
				render_views(app, frame_state.predictedDisplayTime);
			}
			for (size_t i = 0; i < app.projection_views.size(); ++i) {
				XrCompositionLayerProjectionView &projection_view = app.projection_views[i];
				projection_view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				projection_view.pose = app.views[i].pose;
				projection_view.fov = app.views[i].fov;
				projection_view.subImage.swapchain = app.swapchains[i].handle;
				projection_view.subImage.imageRect.offset = {0, 0};
				projection_view.subImage.imageRect.extent = {(int32_t)app.swapchains[i].width,
				                                            (int32_t)app.swapchains[i].height};
				projection_view.subImage.imageArrayIndex = 0;
			}
			layer.space = app.app_space;
			layer.viewCount = (uint32_t)app.projection_views.size();
			layer.views = app.projection_views.data();
			submit_projection = true;
		}
	}

	const XrCompositionLayerBaseHeader *layers[] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer),
	};
	XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = submit_projection ? 1u : 0u;
	end_info.layers = submit_projection ? layers : nullptr;
	check_xr(app.xr.end_frame(app.session, &end_info), "xrEndFrame");
}

static void
cleanup(application &app)
{
	app.renderer.shutdown();
	for (view_swapchain &swapchain : app.swapchains) {
		[swapchain.depth_texture release];
		swapchain.depth_texture = nil;
		if (swapchain.handle != XR_NULL_HANDLE && app.xr.destroy_swapchain != nullptr) {
			app.xr.destroy_swapchain(swapchain.handle);
			swapchain.handle = XR_NULL_HANDLE;
		}
	}
	if (app.view_space != XR_NULL_HANDLE && app.xr.destroy_space != nullptr) {
		app.xr.destroy_space(app.view_space);
		app.view_space = XR_NULL_HANDLE;
	}
	if (app.app_space != XR_NULL_HANDLE && app.xr.destroy_space != nullptr) {
		app.xr.destroy_space(app.app_space);
		app.app_space = XR_NULL_HANDLE;
	}
	if (app.session != XR_NULL_HANDLE && app.xr.destroy_session != nullptr) {
		app.xr.destroy_session(app.session);
		app.session = XR_NULL_HANDLE;
	}
	[app.command_queue release];
	app.command_queue = nil;
	if (app.instance != XR_NULL_HANDLE && app.xr.destroy_instance != nullptr) {
		app.xr.destroy_instance(app.instance);
		app.instance = XR_NULL_HANDLE;
	}
	if (app.loader.handle != nullptr) {
		dlclose(app.loader.handle);
		app.loader.handle = nullptr;
	}
}

static int
run(int argc, char **argv)
{
	const char *loader_path = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
			loader_path = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
			        "Usage: %s [--loader /path/to/libopenxr_loader.1.dylib]\n"
			        "Environment: XR_RUNTIME_JSON selects the runtime; PSVR2_OPENXR_LOADER selects the loader.\n",
			        argv[0]);
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	application app;
	app.loader = open_openxr_loader(loader_path);
	fprintf(stderr, "psvr2-openxr-test: OpenXR loader %s\n", app.loader.path.c_str());
	load_global_xr_functions(app.loader, app.xr);
	create_instance(app);
	create_system_and_session(app);
	create_swapchains(app);

	while (!g_stop_requested && !app.exit_requested) {
		if (!poll_events(app)) {
			break;
		}
		if (!app.session_running) {
			usleep(10000);
			continue;
		}
		render_frame(app);
	}

	cleanup(app);
	return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	@autoreleasepool {
		return run(argc, argv);
	}
}
