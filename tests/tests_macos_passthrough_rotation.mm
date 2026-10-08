// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../src/xrt/compositor/main/comp_window_macos_passthrough_shader.h"
#include "util/u_passthrough_calibration.h"
#include "math/m_api.h"
#include "catch_amalgamated.hpp"

TEST_CASE("Metal camera reprojection agrees with CPU fisheye projection for yaw pitch and roll")
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			SKIP("No Metal device available");
		}
		NSString *source = [[NSString stringWithUTF8String:macos_passthrough_shader_source]
		    stringByAppendingString:
		        @"\nkernel void check_rotation(const device float2 *rays [[buffer(0)]], device float2 *uv "
		        @"[[buffer(1)]], constant Camera &camera [[buffer(2)]], uint index "
		        @"[[thread_position_in_grid]]) { uv[index]=camera_uv(rays[index],camera); }\n"];
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		INFO((error == nil ? "" : [[error description] UTF8String]));
		REQUIRE(library != nil);
		id<MTLFunction> function = [library newFunctionWithName:@"check_rotation"];
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
		                                                                             error:&error];
		REQUIRE(pipeline != nil);
		id<MTLCommandQueue> queue = [device newCommandQueue];
		REQUIRE(queue != nil);
		constexpr size_t count = 121;
		struct xrt_vec2 rays[count];
		for (size_t i = 0; i < count; i++) {
			rays[i] = {(float)(i % 11) / 5 - 1, (float)(i / 11) / 5 - 1};
		}
		id<MTLBuffer> input = [device newBufferWithBytes:rays
		                                          length:sizeof(rays)
		                                         options:MTLResourceStorageModeShared];
		id<MTLBuffer> output = [device newBufferWithLength:sizeof(rays) options:MTLResourceStorageModeShared];
		for (int eye = 0; eye < 2; eye++) {
			struct u_passthrough_camera camera = {};
			camera.fx = 380;
			camera.fy = 379;
			camera.cx = 503.4;
			camera.cy = 502.8;
			camera.k[0] = .03;
			camera.k[1] = -.014;
			camera.k[2] = .006;
			camera.k[3] = -.0017;
			struct xrt_vec3 camera_axis = {0, 1, 0};
			math_quat_from_angle_vector(eye == 0 ? .25f : -.25f, &camera_axis,
			                            &camera.head_from_camera.orientation);
			for (struct xrt_vec3 axis : {xrt_vec3{1, 0, 0}, xrt_vec3{0, 1, 0}, xrt_vec3{0, 0, 1}}) {
				for (float angle : {-.4f, 0.0f, .4f}) {
					struct xrt_quat capture, display, camera_from_display, inverse_capture,
					    capture_from_display;
					math_quat_from_angle_vector(.1f, &camera_axis, &capture);
					math_quat_from_angle_vector(angle, &axis, &display);
					REQUIRE(u_passthrough_calibration_rotation(&camera, &capture, &display,
					                                           &camera_from_display));
					struct
					{
						float intrinsics[4];
						float k[4];
						float rows[3][4];
					} params = {};
					static_assert(sizeof(params) == 80);
					params.intrinsics[0] = camera.fx;
					params.intrinsics[1] = camera.fy;
					params.intrinsics[2] = camera.cx;
					params.intrinsics[3] = camera.cy;
					for (size_t k = 0; k < 4; k++)
						params.k[k] = camera.k[k];
					for (int column = 0; column < 3; column++) {
						struct xrt_vec3 basis = {}, rotated;
						if (column == 0)
							basis.x = 1;
						if (column == 1)
							basis.y = 1;
						if (column == 2)
							basis.z = 1;
						math_quat_rotate_vec3(&camera_from_display, &basis, &rotated);
						params.rows[0][column] = rotated.x;
						params.rows[1][column] = rotated.y;
						params.rows[2][column] = rotated.z;
					}
					id<MTLCommandBuffer> command = [queue commandBuffer];
					id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
					[encoder setComputePipelineState:pipeline];
					[encoder setBuffer:input offset:0 atIndex:0];
					[encoder setBuffer:output offset:0 atIndex:1];
					[encoder setBytes:&params length:sizeof(params) atIndex:2];
					[encoder dispatchThreads:MTLSizeMake(count, 1, 1)
					    threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
					[encoder endEncoding];
					[command commit];
					[command waitUntilCompleted];
					REQUIRE(command.status == MTLCommandBufferStatusCompleted);
					math_quat_invert(&capture, &inverse_capture);
					math_quat_rotate(&inverse_capture, &display, &capture_from_display);
					const auto *actual = static_cast<const xrt_vec2 *>([output contents]);
					for (size_t i = 0; i < count; i++) {
						struct xrt_vec3 display_ray = {rays[i].x, -rays[i].y, -1}, capture_ray;
						math_quat_rotate_vec3(&capture_from_display, &display_ray,
						                      &capture_ray);
						struct xrt_vec2 expected;
						if (u_passthrough_calibration_project(&camera, &capture_ray,
						                                      &expected)) {
							REQUIRE(actual[i].x == Catch::Approx(expected.x).margin(2e-6));
							REQUIRE(actual[i].y == Catch::Approx(expected.y).margin(2e-6));
						} else {
							REQUIRE(actual[i].x < 0);
						}
					}
				}
			}
		}
		[input release];
		[output release];
		[queue release];
		[pipeline release];
		[function release];
		[library release];
		[device release];
	}
}
