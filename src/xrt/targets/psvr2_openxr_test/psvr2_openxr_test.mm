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
	PFN_xrCreateActionSet create_action_set = nullptr;
	PFN_xrDestroyActionSet destroy_action_set = nullptr;
	PFN_xrCreateAction create_action = nullptr;
	PFN_xrDestroyAction destroy_action = nullptr;
	PFN_xrStringToPath string_to_path = nullptr;
	PFN_xrSuggestInteractionProfileBindings suggest_interaction_profile_bindings = nullptr;
	PFN_xrAttachSessionActionSets attach_session_action_sets = nullptr;
	PFN_xrCreateActionSpace create_action_space = nullptr;
	PFN_xrSyncActions sync_actions = nullptr;
	PFN_xrGetActionStatePose get_action_state_pose = nullptr;
	PFN_xrCreatePassthroughFB create_passthrough = nullptr;
	PFN_xrDestroyPassthroughFB destroy_passthrough = nullptr;
	PFN_xrPassthroughStartFB passthrough_start = nullptr;
	PFN_xrCreatePassthroughLayerFB create_passthrough_layer = nullptr;
	PFN_xrDestroyPassthroughLayerFB destroy_passthrough_layer = nullptr;
	PFN_xrPassthroughLayerResumeFB passthrough_layer_resume = nullptr;
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
	LOAD_XR("xrCreateActionSet", create_action_set);
	LOAD_XR("xrDestroyActionSet", destroy_action_set);
	LOAD_XR("xrCreateAction", create_action);
	LOAD_XR("xrDestroyAction", destroy_action);
	LOAD_XR("xrStringToPath", string_to_path);
	LOAD_XR("xrSuggestInteractionProfileBindings", suggest_interaction_profile_bindings);
	LOAD_XR("xrAttachSessionActionSets", attach_session_action_sets);
	LOAD_XR("xrCreateActionSpace", create_action_space);
	LOAD_XR("xrSyncActions", sync_actions);
	LOAD_XR("xrGetActionStatePose", get_action_state_pose);
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

	const simd_float4 ground_color = make_float4(0.12f, 0.38f, 0.42f, 1.0f);
	const simd_float4 marker_color = make_float4(0.82f, 0.86f, 0.90f, 1.0f);
	const simd_float4 front_color = make_float4(1.0f, 0.78f, 0.16f, 1.0f);
	const simd_float4 right_color = make_float4(0.28f, 0.92f, 0.46f, 1.0f);
	const simd_float4 back_color = make_float4(0.22f, 0.64f, 1.0f, 1.0f);
	const simd_float4 left_color = make_float4(0.86f, 0.42f, 0.98f, 1.0f);
	const simd_float4 near_color = make_float4(0.28f, 0.92f, 0.46f, 1.0f);
	const simd_float4 mid_color = make_float4(0.22f, 0.64f, 1.0f, 1.0f);
	const simd_float4 far_color = make_float4(0.86f, 0.42f, 0.98f, 1.0f);

	// Two circular ground-reference bands make translation/parallax visible in
	// every yaw direction without a forward-only floor grid.
	const float floor_y = -0.80f;
	for (int angle_degrees = 0; angle_degrees < 360; angle_degrees += 15) {
		const float radians = (float)angle_degrees * (float)M_PI / 180.0f;
		for (float radius : {1.25f, 3.75f}) {
			add_world_box(scene, sinf(radians) * radius, floor_y, cosf(radians) * radius,
			              make_float3(0.045f, 0.010f, 0.045f), ground_color);
		}
	}

	// A complete 360-degree marker ring. Five markers at each 15-degree sector
	// make the scene equally useful for central and peripheral comparisons even
	// after a 90-, 180-, or 270-degree yaw from the starting orientation.
	const float marker_radius = 2.4f;
	for (int angle_degrees = 0; angle_degrees < 360; angle_degrees += 15) {
		const float radians = (float)angle_degrees * (float)M_PI / 180.0f;
		const float x = sinf(radians) * marker_radius;
		const float z = cosf(radians) * marker_radius;
		simd_float4 sector_color = marker_color;
		if (angle_degrees == 0) {
			sector_color = front_color;
		} else if (angle_degrees == 90) {
			sector_color = right_color;
		} else if (angle_degrees == 180) {
			sector_color = back_color;
		} else if (angle_degrees == 270) {
			sector_color = left_color;
		}
		for (int y_step = -2; y_step <= 2; ++y_step) {
			const bool cardinal_centre = y_step == 0 && (angle_degrees % 90) == 0;
			const float size = cardinal_centre ? 0.085f : 0.050f;
			add_world_box(scene, x, 0.30f * (float)y_step, z, make_float3(size, size, size), sector_color);
		}
	}

	// Constant-angular-size depth targets every 60 degrees. Their physical size
	// grows with distance, giving the same near/far diagnostic anywhere around
	// a broad yaw sweep rather than only around initial forward.
	const std::array<float, 4> distances = {0.75f, 1.5f, 3.0f, 6.0f};
	for (size_t distance_index = 0; distance_index < distances.size(); ++distance_index) {
		const float distance = distances[distance_index];
		const float size = std::max(0.026f, distance * 0.035f);
		const simd_float4 color = distance_index == 0 ? near_color
		                              : distance_index < 3 ? mid_color
		                                                   : far_color;
		for (int angle_degrees = 0; angle_degrees < 360; angle_degrees += 60) {
			const float radians = (float)angle_degrees * (float)M_PI / 180.0f;
			add_world_box(scene, sinf(radians) * distance, 0.72f, cosf(radians) * distance,
			              make_float3(size, size, size), color);
		}
	}

	// Tall cardinal references make the initial forward/right/back/left axes
	// easy to reacquire during large yaw sweeps. Colour encodes the sector.
	const std::array<simd_float4, 4> cardinal_colors = {front_color, right_color, back_color, left_color};
	for (size_t cardinal = 0; cardinal < cardinal_colors.size(); ++cardinal) {
		const float radians = (float)(cardinal * 90) * (float)M_PI / 180.0f;
		add_world_box(scene, sinf(radians) * 2.65f, 0.0f, cosf(radians) * 2.65f,
		              make_float3(0.035f, 1.25f, 0.035f), cardinal_colors[cardinal]);
	}

	// Retain a world-locked fixation cross on the initial forward axis. The
	// magenta head-locked cross remains available independently in every view.
	add_world_box(scene, 0.0f, 0.0f, 2.0f, make_float3(0.30f, 0.018f, 0.018f), front_color);
	add_world_box(scene, 0.0f, 0.0f, 2.0f, make_float3(0.018f, 0.30f, 0.018f), front_color);

	scene.initialized = true;
	fprintf(stderr, "psvr2-openxr-test: diagnostic world contains %zu world-locked boxes over 360 degrees\n",
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
	XrSwapchain depth_handle = XR_NULL_HANDLE;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<XrSwapchainImageMetalKHR> images;
	std::vector<XrSwapchainImageMetalKHR> depth_images;
	id<MTLTexture> depth_texture = nil;
};

struct gaze_calibration_target
{
	float yaw_deg;
	float pitch_deg;
};

struct gaze_calibration_state
{
	size_t target_index = 0;
	XrTime target_started = 0;
	std::vector<float> current_yaw_samples;
	std::vector<float> current_pitch_samples;
	std::vector<float> measured_yaw;
	std::vector<float> measured_pitch;
	float prior_yaw_gain = 1.0f;
	float prior_yaw_offset_deg = 0.0f;
	float prior_pitch_gain = 1.0f;
	float prior_pitch_offset_deg = 0.0f;
	bool prior_loaded = false;
	bool finished = false;
};

static const std::array<gaze_calibration_target, 9> k_gaze_calibration_targets = {{
    {0.0f, 0.0f},
    {-20.0f, 15.0f},
    {20.0f, -15.0f},
    {-20.0f, 0.0f},
    {20.0f, 0.0f},
    {20.0f, 15.0f},
    {-20.0f, -15.0f},
    {0.0f, 15.0f},
    {0.0f, -15.0f},
}};

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
	bool submit_depth_layer = false;
	bool submit_passthrough = false;
	bool passthrough_only = false;
	bool test_gaze = false;
	bool gaze_calibrate = false;
	bool gaze_supported = false;
	gaze_calibration_state gaze_calibration;
	XrActionSet gaze_action_set = XR_NULL_HANDLE;
	XrAction gaze_action = XR_NULL_HANDLE;
	XrSpace gaze_space = XR_NULL_HANDLE;
	XrPath gaze_subaction_path = XR_NULL_PATH;
	uint64_t gaze_frame_count = 0;
	uint64_t gaze_valid_count = 0;
	XrPassthroughFB passthrough = XR_NULL_HANDLE;
	XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;
	id<MTLCommandQueue> command_queue = nil;
	MTLPixelFormat color_format = MTLPixelFormatInvalid;
	std::vector<XrViewConfigurationView> view_configuration;
	std::vector<XrView> views;
	std::vector<XrCompositionLayerProjectionView> projection_views;
	std::vector<XrCompositionLayerDepthInfoKHR> depth_infos;
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
	if (app.submit_depth_layer && !has_extension(app.xr, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) {
		fatal("runtime does not expose XR_KHR_composition_layer_depth");
	}
	if (app.submit_passthrough && !has_extension(app.xr, XR_FB_PASSTHROUGH_EXTENSION_NAME)) {
		fatal("runtime does not expose XR_FB_passthrough");
	}
	if (app.test_gaze && !has_extension(app.xr, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
		fatal("runtime does not expose XR_EXT_eye_gaze_interaction");
	}

	std::vector<const char *> extensions = {XR_KHR_METAL_ENABLE_EXTENSION_NAME};
	if (app.submit_depth_layer) {
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	}
	if (app.submit_passthrough) {
		extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
	}
	if (app.test_gaze) {
		extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
	}

	XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "PSVR2 OpenXR Test");
	create_info.applicationInfo.applicationVersion = 1;
	snprintf(create_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "Monado diagnostic");
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	create_info.enabledExtensionCount = (uint32_t)extensions.size();
	create_info.enabledExtensionNames = extensions.data();
	check_xr(app.xr.create_instance(&create_info, &app.instance), "xrCreateInstance");
	load_instance_xr_functions(app.xr, app.instance);
	if (app.submit_passthrough) {
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrCreatePassthroughFB",
		             &app.xr.create_passthrough);
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrDestroyPassthroughFB",
		             &app.xr.destroy_passthrough);
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrPassthroughStartFB",
		             &app.xr.passthrough_start);
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrCreatePassthroughLayerFB",
		             &app.xr.create_passthrough_layer);
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrDestroyPassthroughLayerFB",
		             &app.xr.destroy_passthrough_layer);
		load_xr_proc(app.xr.get_instance_proc_addr, app.instance, "xrPassthroughLayerResumeFB",
		             &app.xr.passthrough_layer_resume);
	}

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

	XrSystemEyeGazeInteractionPropertiesEXT gaze_properties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
	XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
	if (app.test_gaze) {
		properties.next = &gaze_properties;
	}
	check_xr(app.xr.get_system_properties(app.instance, app.system_id, &properties), "xrGetSystemProperties");
	fprintf(stderr, "psvr2-openxr-test: system %s\n", properties.systemName);
	if (app.test_gaze) {
		app.gaze_supported = gaze_properties.supportsEyeGazeInteraction == XR_TRUE;
		fprintf(stderr, "psvr2-openxr-test: eye gaze interaction %s\n",
		        app.gaze_supported ? "supported" : "NOT supported");
		if (!app.gaze_supported) {
			fatal("runtime system does not report eye gaze support; ensure PSVR2_GAZE_STREAMS=1 reaches monado-service");
		}
	}

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

static bool
read_existing_gaze_calibration(gaze_calibration_state &state);

static void
create_gaze_resources(application &app)
{
	if (!app.test_gaze) {
		return;
	}

	check_xr(app.xr.string_to_path(app.instance, "/user/eyes_ext", &app.gaze_subaction_path),
	         "xrStringToPath(/user/eyes_ext)");

	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	snprintf(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "gaze");
	snprintf(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Eye gaze");
	set_info.priority = 0;
	check_xr(app.xr.create_action_set(app.instance, &set_info, &app.gaze_action_set), "xrCreateActionSet(gaze)");

	XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
	action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
	snprintf(action_info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", "gaze_pose");
	snprintf(action_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", "Gaze pose");
	action_info.countSubactionPaths = 1;
	action_info.subactionPaths = &app.gaze_subaction_path;
	check_xr(app.xr.create_action(app.gaze_action_set, &action_info, &app.gaze_action), "xrCreateAction(gaze)");

	XrPath interaction_profile = XR_NULL_PATH;
	XrPath gaze_binding_path = XR_NULL_PATH;
	check_xr(app.xr.string_to_path(app.instance, "/interaction_profiles/ext/eye_gaze_interaction",
	                               &interaction_profile),
	         "xrStringToPath(eye gaze profile)");
	check_xr(app.xr.string_to_path(app.instance, "/user/eyes_ext/input/gaze_ext/pose", &gaze_binding_path),
	         "xrStringToPath(gaze pose binding)");

	XrActionSuggestedBinding binding{app.gaze_action, gaze_binding_path};
	XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile = interaction_profile;
	suggested.countSuggestedBindings = 1;
	suggested.suggestedBindings = &binding;
	check_xr(app.xr.suggest_interaction_profile_bindings(app.instance, &suggested),
	         "xrSuggestInteractionProfileBindings(eye gaze)");

	XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach_info.countActionSets = 1;
	attach_info.actionSets = &app.gaze_action_set;
	check_xr(app.xr.attach_session_action_sets(app.session, &attach_info), "xrAttachSessionActionSets(gaze)");

	XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
	space_info.action = app.gaze_action;
	space_info.subactionPath = app.gaze_subaction_path;
	space_info.poseInActionSpace.orientation.w = 1.0f;
	check_xr(app.xr.create_action_space(app.session, &space_info, &app.gaze_space), "xrCreateActionSpace(gaze)");

	if (app.gaze_calibrate) {
		(void)read_existing_gaze_calibration(app.gaze_calibration);
		fprintf(stderr,
		        "psvr2-openxr-test: gaze calibration mode: follow each target with your eyes; "
		        "blue=settle, green=capture\n");
		if (app.gaze_calibration.prior_loaded) {
			fprintf(stderr,
			        "psvr2-openxr-test: existing user calibration will be refined "
			        "(yaw %.5fx %+0.3f deg, pitch %.5fx %+0.3f deg)\n",
			        app.gaze_calibration.prior_yaw_gain, app.gaze_calibration.prior_yaw_offset_deg,
			        app.gaze_calibration.prior_pitch_gain, app.gaze_calibration.prior_pitch_offset_deg);
		}
		fprintf(stderr,
		        "psvr2-openxr-test: calibration assumes PSVR2_GAZE_* environment overrides are unset\n");
	} else {
		fprintf(stderr,
		        "psvr2-openxr-test: gaze action ready; marker is bright yellow at 2 m along the reported gaze ray\n");
	}
}

static void
create_passthrough_resources(application &app)
{
	if (!app.submit_passthrough) {
		return;
	}

	XrPassthroughCreateInfoFB passthrough_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
	check_xr(app.xr.create_passthrough(app.session, &passthrough_info, &app.passthrough),
	         "xrCreatePassthroughFB");
	check_xr(app.xr.passthrough_start(app.passthrough), "xrPassthroughStartFB");

	XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
	layer_info.passthrough = app.passthrough;
	layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	check_xr(app.xr.create_passthrough_layer(app.session, &layer_info, &app.passthrough_layer),
	         "xrCreatePassthroughLayerFB");
	check_xr(app.xr.passthrough_layer_resume(app.passthrough_layer), "xrPassthroughLayerResumeFB");

	fprintf(stderr, "psvr2-openxr-test: XR_FB_passthrough running%s\n",
	        app.passthrough_only ? " (camera only)" : " behind diagnostic scene");
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

static MTLPixelFormat
choose_depth_swapchain_format(application &app)
{
	uint32_t format_count = 0;
	check_xr(app.xr.enumerate_swapchain_formats(app.session, 0, &format_count, nullptr),
	         "xrEnumerateSwapchainFormats(depth count)");
	std::vector<int64_t> formats(format_count);
	check_xr(app.xr.enumerate_swapchain_formats(app.session, format_count, &format_count, formats.data()),
	         "xrEnumerateSwapchainFormats(depth list)");

	if (std::find(formats.begin(), formats.end(), (int64_t)MTLPixelFormatDepth32Float) != formats.end()) {
		return MTLPixelFormatDepth32Float;
	}
	fatal("runtime did not expose MTLPixelFormatDepth32Float for an OpenXR depth swapchain");
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

		if (app.submit_depth_layer) {
			XrSwapchainCreateInfo depth_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			depth_create_info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			depth_create_info.format = (int64_t)choose_depth_swapchain_format(app);
			depth_create_info.sampleCount = 1;
			depth_create_info.width = swapchain.width;
			depth_create_info.height = swapchain.height;
			depth_create_info.faceCount = 1;
			depth_create_info.arraySize = 1;
			depth_create_info.mipCount = 1;
			check_xr(app.xr.create_swapchain(app.session, &depth_create_info, &swapchain.depth_handle),
			         "xrCreateSwapchain(depth)");

			uint32_t depth_image_count = 0;
			check_xr(app.xr.enumerate_swapchain_images(swapchain.depth_handle, 0, &depth_image_count, nullptr),
			         "xrEnumerateSwapchainImages(depth count)");
			swapchain.depth_images.resize(depth_image_count);
			for (XrSwapchainImageMetalKHR &image : swapchain.depth_images) {
				image = {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR};
			}
			check_xr(app.xr.enumerate_swapchain_images(
			             swapchain.depth_handle, depth_image_count, &depth_image_count,
			             reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchain.depth_images.data())),
			         "xrEnumerateSwapchainImages(depth list)");
		} else {
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
	}

	app.views.resize(view_count);
	app.projection_views.resize(view_count);
	app.depth_infos.resize(view_count);
	for (uint32_t i = 0; i < view_count; ++i) {
		app.views[i] = {XR_TYPE_VIEW};
		app.projection_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		app.depth_infos[i] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
	}
	app.frame_instances.reserve(app.renderer.max_instances);
	app.renderer.initialize(device, app.color_format);

	fprintf(stderr, "psvr2-openxr-test: %u views, %ux%u per eye, Metal format %lld%s\n", view_count,
	        app.swapchains[0].width, app.swapchains[0].height, (long long)app.color_format,
	        app.submit_depth_layer ? ", XR_KHR_composition_layer_depth enabled" : "");
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


static bool
read_existing_gaze_calibration(gaze_calibration_state &state)
{
	const char *home = getenv("HOME");
	if (home == nullptr) {
		return false;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/Library/Application Support/monado/psvr2/gaze_user_calibration.txt", home);
	FILE *file = fopen(path, "r");
	if (file == nullptr) {
		return false;
	}

	char magic[64] = {};
	float yaw_gain = 1.0f;
	float yaw_offset = 0.0f;
	float pitch_gain = 1.0f;
	float pitch_offset = 0.0f;
	int fields = fscanf(file, "%63s %f %f %f %f", magic, &yaw_gain, &yaw_offset, &pitch_gain, &pitch_offset);
	fclose(file);
	if (fields != 5 || strcmp(magic, "PSVR2_GAZE_USER_CALIBRATION_V1") != 0) {
		return false;
	}

	state.prior_yaw_gain = yaw_gain;
	state.prior_yaw_offset_deg = yaw_offset;
	state.prior_pitch_gain = pitch_gain;
	state.prior_pitch_offset_deg = pitch_offset;
	state.prior_loaded = true;
	return true;
}

static float
median_sample(std::vector<float> values)
{
	if (values.empty()) {
		return 0.0f;
	}
	std::sort(values.begin(), values.end());
	const size_t mid = values.size() / 2;
	if ((values.size() & 1u) != 0) {
		return values[mid];
	}
	return 0.5f * (values[mid - 1] + values[mid]);
}

static bool
fit_linear_calibration(const std::vector<float> &measured,
                       bool yaw_axis,
                       float *out_gain,
                       float *out_offset,
                       float *out_rms)
{
	if (measured.size() != k_gaze_calibration_targets.size()) {
		return false;
	}

	double sx = 0.0;
	double sy = 0.0;
	double sxx = 0.0;
	double sxy = 0.0;
	for (size_t i = 0; i < measured.size(); ++i) {
		const double x = measured[i];
		const double y = yaw_axis ? k_gaze_calibration_targets[i].yaw_deg
		                          : k_gaze_calibration_targets[i].pitch_deg;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	const double n = (double)measured.size();
	const double denominator = n * sxx - sx * sx;
	if (fabs(denominator) < 1e-6) {
		return false;
	}
	const double gain = (n * sxy - sx * sy) / denominator;
	const double offset = (sy - gain * sx) / n;

	double squared_error = 0.0;
	for (size_t i = 0; i < measured.size(); ++i) {
		const double expected = yaw_axis ? k_gaze_calibration_targets[i].yaw_deg
		                                 : k_gaze_calibration_targets[i].pitch_deg;
		const double error = gain * measured[i] + offset - expected;
		squared_error += error * error;
	}
	*out_gain = (float)gain;
	*out_offset = (float)offset;
	*out_rms = (float)sqrt(squared_error / n);
	return true;
}

static bool
write_gaze_calibration(float yaw_gain, float yaw_offset, float pitch_gain, float pitch_offset)
{
	NSString *directory =
	    [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/monado/psvr2"];
	NSError *error = nil;
	if (![[NSFileManager defaultManager] createDirectoryAtPath:directory
	                              withIntermediateDirectories:YES
	                                               attributes:nil
	                                                    error:&error]) {
		fprintf(stderr, "psvr2-openxr-test: could not create gaze calibration directory: %s\n",
		        [[error localizedDescription] UTF8String]);
		return false;
	}

	NSString *path = [directory stringByAppendingPathComponent:@"gaze_user_calibration.txt"];
	NSString *contents =
	    [NSString stringWithFormat:@"PSVR2_GAZE_USER_CALIBRATION_V1 %.9g %.9g %.9g %.9g\n",
	                               yaw_gain, yaw_offset, pitch_gain, pitch_offset];
	if (![contents writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
		fprintf(stderr, "psvr2-openxr-test: could not write gaze calibration: %s\n",
		        [[error localizedDescription] UTF8String]);
		return false;
	}
	fprintf(stderr, "psvr2-openxr-test: saved gaze calibration to %s\n", [path fileSystemRepresentation]);
	return true;
}

static bool
locate_gaze_relative_to_view(application &app,
                             XrTime predicted_display_time,
                             float *out_yaw_deg,
                             float *out_pitch_deg)
{
	XrActiveActionSet active_set{app.gaze_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
	sync_info.countActiveActionSets = 1;
	sync_info.activeActionSets = &active_set;
	if (XR_FAILED(app.xr.sync_actions(app.session, &sync_info))) {
		return false;
	}

	XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
	get_info.action = app.gaze_action;
	get_info.subactionPath = app.gaze_subaction_path;
	XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
	if (XR_FAILED(app.xr.get_action_state_pose(app.session, &get_info, &pose_state)) ||
	    pose_state.isActive != XR_TRUE) {
		return false;
	}

	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(app.xr.locate_space(app.gaze_space, app.view_space, predicted_display_time, &location))) {
		return false;
	}
	const XrSpaceLocationFlags required =
	    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((location.locationFlags & required) != required) {
		return false;
	}

	const simd_float3 direction =
	    rotate_vector(location.pose.orientation, make_float3(0.0f, 0.0f, -1.0f));
	*out_yaw_deg = atan2f(direction.x, -direction.z) * 180.0f / (float)M_PI;
	*out_pitch_deg =
	    atan2f(direction.y, sqrtf(direction.x * direction.x + direction.z * direction.z)) *
	    180.0f / (float)M_PI;
	return true;
}

static void
finish_gaze_calibration(application &app)
{
	gaze_calibration_state &state = app.gaze_calibration;
	float residual_yaw_gain = 1.0f;
	float residual_yaw_offset = 0.0f;
	float yaw_rms = 0.0f;
	float residual_pitch_gain = 1.0f;
	float residual_pitch_offset = 0.0f;
	float pitch_rms = 0.0f;

	if (!fit_linear_calibration(state.measured_yaw, true, &residual_yaw_gain, &residual_yaw_offset, &yaw_rms) ||
	    !fit_linear_calibration(state.measured_pitch, false, &residual_pitch_gain, &residual_pitch_offset,
	                            &pitch_rms)) {
		fatal("could not solve gaze calibration");
	}

	/*
	 * The OpenXR samples already include any calibration currently loaded by
	 * the service. Compose the fitted residual correction with that prior
	 * transform so rerunning calibration refines rather than double-applies it.
	 */
	const float yaw_gain = residual_yaw_gain * state.prior_yaw_gain;
	const float yaw_offset =
	    residual_yaw_gain * state.prior_yaw_offset_deg + residual_yaw_offset;
	const float pitch_gain = residual_pitch_gain * state.prior_pitch_gain;
	const float pitch_offset =
	    residual_pitch_gain * state.prior_pitch_offset_deg + residual_pitch_offset;

	fprintf(stderr,
	        "\npsvr2-openxr-test: gaze calibration complete\n"
	        "  residual fit: yaw %.5fx %+0.3f deg (RMS %.3f deg), "
	        "pitch %.5fx %+0.3f deg (RMS %.3f deg)\n"
	        "  saved absolute calibration: yaw %.5fx %+0.3f deg, pitch %.5fx %+0.3f deg\n",
	        residual_yaw_gain, residual_yaw_offset, yaw_rms,
	        residual_pitch_gain, residual_pitch_offset, pitch_rms,
	        yaw_gain, yaw_offset, pitch_gain, pitch_offset);

	if (!isfinite(yaw_gain) || yaw_gain < 0.5f || yaw_gain > 1.5f ||
	    !isfinite(pitch_gain) || pitch_gain < 0.5f || pitch_gain > 1.5f ||
	    fabsf(yaw_offset) > 20.0f || fabsf(pitch_offset) > 20.0f) {
		fatal("calibration solution is outside safety bounds; not saving");
	}
	if (!write_gaze_calibration(yaw_gain, yaw_offset, pitch_gain, pitch_offset)) {
		fatal("failed to save gaze calibration");
	}

	fprintf(stderr,
	        "psvr2-openxr-test: restart monado-service to load the new calibration, then verify with --gaze\n");
	state.finished = true;
	app.exit_requested = true;
}

static void
update_gaze_calibration(application &app, XrTime predicted_display_time, const XrPosef &head_pose)
{
	gaze_calibration_state &state = app.gaze_calibration;
	if (state.finished || state.target_index >= k_gaze_calibration_targets.size()) {
		return;
	}

	const gaze_calibration_target target = k_gaze_calibration_targets[state.target_index];
	if (state.target_started == 0) {
		state.target_started = predicted_display_time;
		state.current_yaw_samples.clear();
		state.current_pitch_samples.clear();
		fprintf(stderr, "psvr2-openxr-test: calibration target %zu/%zu: yaw %+0.1f deg, pitch %+0.1f deg\n",
		        state.target_index + 1, k_gaze_calibration_targets.size(), target.yaw_deg, target.pitch_deg);
	}

	const XrDuration elapsed = predicted_display_time - state.target_started;
	const XrDuration settle_ns = 900000000;
	const XrDuration capture_ns = 1200000000;
	const bool collecting = elapsed >= settle_ns && elapsed < settle_ns + capture_ns;

	if (collecting) {
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		if (locate_gaze_relative_to_view(app, predicted_display_time, &yaw_deg, &pitch_deg)) {
			state.current_yaw_samples.push_back(yaw_deg);
			state.current_pitch_samples.push_back(pitch_deg);
		}
	}

	const float yaw = target.yaw_deg * (float)M_PI / 180.0f;
	const float pitch = target.pitch_deg * (float)M_PI / 180.0f;
	const simd_float3 local_direction =
	    make_float3(sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
	const simd_float3 world_direction = rotate_vector(head_pose.orientation, local_direction);
	const simd_float3 position = xr_position(head_pose.position) + world_direction * 2.0f;
	const simd_float3 right = rotate_vector(head_pose.orientation, make_float3(1.0f, 0.0f, 0.0f));
	const simd_float3 up = rotate_vector(head_pose.orientation, make_float3(0.0f, 1.0f, 0.0f));
	const simd_float4 color =
	    collecting ? make_float4(0.20f, 1.0f, 0.30f, 1.0f) : make_float4(0.15f, 0.75f, 1.0f, 1.0f);
	app.frame_instances.push_back(
	    {basis_model(position, right, up, -world_direction, make_float3(0.022f, 0.022f, 0.022f)), color});

	if (elapsed >= settle_ns + capture_ns) {
		if (state.current_yaw_samples.size() < 30 || state.current_pitch_samples.size() < 30) {
			fprintf(stderr,
			        "psvr2-openxr-test: insufficient valid gaze samples (%zu); repeating target\n",
			        state.current_yaw_samples.size());
			state.target_started = 0;
			return;
		}

		const float measured_yaw = median_sample(state.current_yaw_samples);
		const float measured_pitch = median_sample(state.current_pitch_samples);
		state.measured_yaw.push_back(measured_yaw);
		state.measured_pitch.push_back(measured_pitch);
		fprintf(stderr,
		        "psvr2-openxr-test: captured target %zu: measured yaw %+0.2f, pitch %+0.2f deg "
		        "(%zu samples)\n",
		        state.target_index + 1, measured_yaw, measured_pitch, state.current_yaw_samples.size());

		state.target_index++;
		state.target_started = 0;
		if (state.target_index == k_gaze_calibration_targets.size()) {
			finish_gaze_calibration(app);
		}
	}
}

static void
append_gaze_marker(application &app, XrTime predicted_display_time)
{
	if (!app.test_gaze || app.gaze_space == XR_NULL_HANDLE) {
		return;
	}

	XrActiveActionSet active_set{app.gaze_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
	sync_info.countActiveActionSets = 1;
	sync_info.activeActionSets = &active_set;
	XrResult sync_result = app.xr.sync_actions(app.session, &sync_info);
	if (XR_FAILED(sync_result)) {
		return;
	}

	XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
	get_info.action = app.gaze_action;
	get_info.subactionPath = app.gaze_subaction_path;
	XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
	if (XR_FAILED(app.xr.get_action_state_pose(app.session, &get_info, &pose_state)) || pose_state.isActive != XR_TRUE) {
		app.gaze_frame_count++;
		if ((app.gaze_frame_count % 120) == 0) {
			fprintf(stderr, "psvr2-openxr-test: gaze action inactive\n");
		}
		return;
	}

	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	XrResult locate_result = app.xr.locate_space(app.gaze_space, app.app_space, predicted_display_time, &location);
	app.gaze_frame_count++;
	const XrSpaceLocationFlags required =
	    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(locate_result) || (location.locationFlags & required) != required) {
		if ((app.gaze_frame_count % 120) == 0) {
			fprintf(stderr, "psvr2-openxr-test: gaze active but pose invalid (flags=0x%llx)\n",
			        (unsigned long long)location.locationFlags);
		}
		return;
	}

	app.gaze_valid_count++;
	const simd_float3 origin = xr_position(location.pose.position);
	const simd_float3 direction =
	    rotate_vector(location.pose.orientation, make_float3(0.0f, 0.0f, -1.0f));
	const simd_float3 target = origin + direction * 2.0f;
	const simd_float3 right =
	    rotate_vector(location.pose.orientation, make_float3(1.0f, 0.0f, 0.0f));
	const simd_float3 up =
	    rotate_vector(location.pose.orientation, make_float3(0.0f, 1.0f, 0.0f));
	const simd_float3 back = -direction;
	const simd_float4 color = make_float4(1.0f, 0.95f, 0.05f, 1.0f);

	app.frame_instances.push_back(
	    {basis_model(target, right, up, back, make_float3(0.025f, 0.025f, 0.025f)), color});

	if ((app.gaze_valid_count % 120) == 1) {
		fprintf(stderr,
		        "psvr2-openxr-test: gaze valid dir=(%+.3f,%+.3f,%+.3f) target2m=(%+.2f,%+.2f,%+.2f)\n",
		        direction.x, direction.y, direction.z, target.x, target.y, target.z);
	}
}

static void
render_views(application &app, XrTime predicted_display_time)
{
	const XrPosef head_pose = head_pose_for_frame(app, predicted_display_time);
	if (!app.scene.initialized) {
		initialize_scene(app.scene, head_pose);
	}
	if (app.gaze_calibrate) {
		app.frame_instances.clear();
		update_gaze_calibration(app, predicted_display_time, head_pose);
	} else {
		app.frame_instances = app.scene.world_instances;
		append_head_locked_cross(app.frame_instances, head_pose);
		append_gaze_marker(app, predicted_display_time);
	}
	if (app.frame_instances.size() > app.renderer.max_instances) {
		fatal("diagnostic scene exceeded Metal instance buffer capacity");
	}
	memcpy([app.renderer.instance_buffer contents], app.frame_instances.data(),
	       app.frame_instances.size() * sizeof(instance_data));

	std::vector<uint32_t> image_indices(app.swapchains.size(), 0);
	std::vector<uint32_t> depth_image_indices(app.swapchains.size(), 0);
	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].handle, &acquire_info, &image_indices[i]),
		         "xrAcquireSwapchainImage");
		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		check_xr(app.xr.wait_swapchain_image(app.swapchains[i].handle, &wait_info), "xrWaitSwapchainImage");

		if (app.submit_depth_layer) {
			check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].depth_handle, &acquire_info,
			                                         &depth_image_indices[i]),
			         "xrAcquireSwapchainImage(depth)");
			check_xr(app.xr.wait_swapchain_image(app.swapchains[i].depth_handle, &wait_info),
			         "xrWaitSwapchainImage(depth)");
		}
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
		render_pass.colorAttachments[0].clearColor =
		    app.submit_passthrough ? MTLClearColorMake(0.0, 0.0, 0.0, 0.0)
		                           : MTLClearColorMake(0.012, 0.018, 0.024, 1.0);
		id<MTLTexture> depth_texture = swapchain.depth_texture;
		if (app.submit_depth_layer) {
			if (depth_image_indices[i] >= swapchain.depth_images.size()) {
				fatal("OpenXR returned an out-of-range depth swapchain image index");
			}
			depth_texture = (__bridge id<MTLTexture>)swapchain.depth_images[depth_image_indices[i]].texture;
			if (depth_texture == nil) {
				fatal("OpenXR returned a nil Metal depth swapchain texture");
			}
		}
		render_pass.depthAttachment.texture = depth_texture;
		render_pass.depthAttachment.loadAction = MTLLoadActionClear;
		render_pass.depthAttachment.storeAction =
		    app.submit_depth_layer ? MTLStoreActionStore : MTLStoreActionDontCare;
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
		if (app.submit_depth_layer) {
			check_xr(app.xr.release_swapchain_image(swapchain.depth_handle, &release_info),
			         "xrReleaseSwapchainImage(depth)");
		}
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

				if (app.submit_depth_layer) {
					XrCompositionLayerDepthInfoKHR &depth_info = app.depth_infos[i];
					depth_info = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
					depth_info.subImage.swapchain = app.swapchains[i].depth_handle;
					depth_info.subImage.imageRect.offset = {0, 0};
					depth_info.subImage.imageRect.extent = {(int32_t)app.swapchains[i].width,
					                                          (int32_t)app.swapchains[i].height};
					depth_info.subImage.imageArrayIndex = 0;
					depth_info.minDepth = 0.0f;
					depth_info.maxDepth = 1.0f;
					depth_info.nearZ = 0.05f;
					depth_info.farZ = 100.0f;
					projection_view.next = &depth_info;
				}
			}
			layer.space = app.app_space;
			layer.viewCount = (uint32_t)app.projection_views.size();
			layer.views = app.projection_views.data();
			if (app.submit_passthrough) {
				layer.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			}
			submit_projection = !app.passthrough_only;
		}
	}

	XrCompositionLayerPassthroughFB passthrough_layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
	passthrough_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	passthrough_layer.space = XR_NULL_HANDLE;
	passthrough_layer.layerHandle = app.passthrough_layer;

	const XrCompositionLayerBaseHeader *layers[2] = {};
	uint32_t layer_count = 0;
	if (app.submit_passthrough) {
		layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthrough_layer);
	}
	if (submit_projection) {
		layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
	}

	XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = layer_count;
	end_info.layers = layer_count > 0 ? layers : nullptr;
	check_xr(app.xr.end_frame(app.session, &end_info), "xrEndFrame");
}

static void
cleanup(application &app)
{
	app.renderer.shutdown();
	for (view_swapchain &swapchain : app.swapchains) {
		[swapchain.depth_texture release];
		swapchain.depth_texture = nil;
		if (swapchain.depth_handle != XR_NULL_HANDLE && app.xr.destroy_swapchain != nullptr) {
			app.xr.destroy_swapchain(swapchain.depth_handle);
			swapchain.depth_handle = XR_NULL_HANDLE;
		}
		if (swapchain.handle != XR_NULL_HANDLE && app.xr.destroy_swapchain != nullptr) {
			app.xr.destroy_swapchain(swapchain.handle);
			swapchain.handle = XR_NULL_HANDLE;
		}
	}
	if (app.gaze_space != XR_NULL_HANDLE && app.xr.destroy_space != nullptr) {
		app.xr.destroy_space(app.gaze_space);
		app.gaze_space = XR_NULL_HANDLE;
	}
	if (app.gaze_action != XR_NULL_HANDLE && app.xr.destroy_action != nullptr) {
		app.xr.destroy_action(app.gaze_action);
		app.gaze_action = XR_NULL_HANDLE;
	}
	if (app.gaze_action_set != XR_NULL_HANDLE && app.xr.destroy_action_set != nullptr) {
		app.xr.destroy_action_set(app.gaze_action_set);
		app.gaze_action_set = XR_NULL_HANDLE;
	}
	if (app.view_space != XR_NULL_HANDLE && app.xr.destroy_space != nullptr) {
		app.xr.destroy_space(app.view_space);
		app.view_space = XR_NULL_HANDLE;
	}
	if (app.app_space != XR_NULL_HANDLE && app.xr.destroy_space != nullptr) {
		app.xr.destroy_space(app.app_space);
		app.app_space = XR_NULL_HANDLE;
	}
	if (app.passthrough_layer != XR_NULL_HANDLE && app.xr.destroy_passthrough_layer != nullptr) {
		app.xr.destroy_passthrough_layer(app.passthrough_layer);
		app.passthrough_layer = XR_NULL_HANDLE;
	}
	if (app.passthrough != XR_NULL_HANDLE && app.xr.destroy_passthrough != nullptr) {
		app.xr.destroy_passthrough(app.passthrough);
		app.passthrough = XR_NULL_HANDLE;
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
	bool submit_depth_layer = false;
	bool submit_passthrough = false;
	bool passthrough_only = false;
	bool test_gaze = false;
	bool gaze_calibrate = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
			loader_path = argv[++i];
		} else if (strcmp(argv[i], "--depth-layer") == 0) {
			submit_depth_layer = true;
		} else if (strcmp(argv[i], "--passthrough") == 0) {
			submit_passthrough = true;
		} else if (strcmp(argv[i], "--passthrough-only") == 0) {
			submit_passthrough = true;
			passthrough_only = true;
		} else if (strcmp(argv[i], "--gaze") == 0) {
			test_gaze = true;
		} else if (strcmp(argv[i], "--gaze-calibrate") == 0) {
			test_gaze = true;
			gaze_calibrate = true;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
			        "Usage: %s [--loader /path/to/libopenxr_loader.1.dylib] [--depth-layer] "
			        "[--passthrough|--passthrough-only] [--gaze|--gaze-calibrate]\n"
			        "  --depth-layer submits the rendered Depth32Float attachment through "
			        "XR_KHR_composition_layer_depth.\n"
			        "  --passthrough submits XR_FB_passthrough behind the diagnostic scene.\n"
			        "  --passthrough-only submits only XR_FB_passthrough.\n"
			        "  --gaze enables XR_EXT_eye_gaze_interaction and draws a yellow gaze marker.\n"
			        "  --gaze-calibrate runs a 9-point head-relative calibration and saves it for the driver.\n"
			        "Environment: XR_RUNTIME_JSON selects the runtime; PSVR2_OPENXR_LOADER selects the loader. "
			        "PSVR2_CAMERA_STREAMS=1 enables the PS VR2 BC4 camera source; "
			        "PSVR2_GAZE_STREAMS=1 enables the gaze USB stream.\n",
			        argv[0]);
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	application app;
	app.submit_depth_layer = submit_depth_layer;
	app.submit_passthrough = submit_passthrough;
	app.passthrough_only = passthrough_only;
	app.test_gaze = test_gaze;
	app.gaze_calibrate = gaze_calibrate;
	app.loader = open_openxr_loader(loader_path);
	fprintf(stderr, "psvr2-openxr-test: OpenXR loader %s\n", app.loader.path.c_str());
	load_global_xr_functions(app.loader, app.xr);
	create_instance(app);
	create_system_and_session(app);
	create_gaze_resources(app);
	create_passthrough_resources(app);
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
