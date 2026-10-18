// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "render_compute_test_fixture.h"
#include "render/render_camera.h"
#include "render/render_output.h"
#include "util/u_passthrough_calibration.h"
#include "camera.frag.h"
#include "fullscreen.vert.h"
#include "output.frag.h"

namespace {
struct CameraFixture
{
	Fixture f;
	//! Presentation-like destinations (render target only) and the separate
	//! application sources the camera pass samples.
	std::array<Image, 3> outputs{}, sources{};
	std::array<VkSemaphore, 3> complete{};
	VkShaderModule vert{}, frag{}, output_shader{};
	render_camera *renderer{};
	render_output *output{};
	std::array<std::vector<float>, 2> maps;
	std::array<u_passthrough_camera, 2> calibration{};
	static constexpr unsigned map_size = 5, width = 34, height = 13, cw = 12, ch = 8;

	VkShaderModule
	module(const uint32_t *code, size_t size)
	{
		VkShaderModuleCreateInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		si.codeSize = size;
		si.pCode = code;
		VkShaderModule result{};
		REQUIRE(f.vk().vkCreateShaderModule(f.vk().device, &si, nullptr, &result) == VK_SUCCESS);
		return result;
	}
	void
	create_image(Image &image, VkImageUsageFlags usage)
	{
		auto &vk = f.vk();
		image.extent = {width, height};
		REQUIRE(vk_create_image_simple(&vk, image.extent, VK_FORMAT_R8G8B8A8_UNORM, usage, &image.memory,
		                               &image.image) == VK_SUCCESS);
		REQUIRE(vk_create_view(&vk, image.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, color_range,
		                       &image.view) == VK_SUCCESS);
		auto cmd = f.begin();
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
		          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		          VK_PIPELINE_STAGE_TRANSFER_BIT);
		f.finish(cmd);
	}
	//! private_output: sample render_output's private images, sharing its
	//! destinations, as the macOS WSI target does.
	void
	init(bool private_output = false)
	{
		f.init(17, height);
		auto &vk = f.vk();
		vert = module(fullscreen_vert, sizeof(fullscreen_vert));
		frag = module(camera_frag, sizeof(camera_frag));
		output_shader = module(output_frag, sizeof(output_frag));
		VkImage images[3];
		VkImageView views[3], destinations[3];
		for (unsigned i = 0; i < outputs.size(); i++) {
			// TRANSFER_SRC is solely for this test's readback.
			create_image(outputs[i], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			destinations[i] = outputs[i].view;
			if (!private_output) {
				create_image(sources[i], VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
				images[i] = sources[i].image;
				views[i] = sources[i].view;
			}
			VkSemaphoreCreateInfo sem{};
			sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			REQUIRE(vk.vkCreateSemaphore(vk.device, &sem, nullptr, &complete[i]) == VK_SUCCESS);
		}
		for (unsigned eye = 0; eye < 2; eye++) {
			auto &c = calibration[eye];
			c.fx = 3.4 * 1024 / cw;
			c.fy = 3.3 * 1016 / ch;
			c.cx = 6.0 * 1024 / cw - .5;
			c.cy = 4.0 * 1016 / ch - .5;
			c.k[0] = .03;
			c.k[1] = -.01;
			c.head_from_camera.orientation.w = 1;
			maps[eye].resize(map_size * map_size * 4);
			for (unsigned y = 0; y < map_size; y++)
				for (unsigned x = 0; x < map_size; x++) {
					float tx = float(x) / 2 - 1, ty = float(y) / 2 - 1;
					xrt_vec3 ray{tx, -ty, -1};
					xrt_vec2 uv{};
					REQUIRE(u_passthrough_calibration_project(&c, &ray, &uv));
					size_t n = (y * map_size + x) * 4;
					maps[eye][n] = uv.x;
					maps[eye][n + 1] = uv.y;
					maps[eye][n + 2] = tx;
					maps[eye][n + 3] = ty;
				}
			maps[eye][0] = maps[eye][1] = -1;
			maps[eye][2] = maps[eye][3] = NAN;
		}
		if (private_output) {
			output = render_output_create(&vk, vk.main_queue, vert, output_shader, VK_FORMAT_R8G8B8A8_UNORM,
			                              {width, height},
			                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			                              destinations, VK_IMAGE_LAYOUT_GENERAL, 3);
			REQUIRE(output != nullptr);
			for (unsigned i = 0; i < 3; i++) {
				images[i] = render_output_image(output, i);
				views[i] = render_output_view(output, i);
				Image image{images[i], VK_NULL_HANDLE, views[i], {width, height}};
				auto cmd = f.begin();
				f.barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
				          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
				          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
				f.finish(cmd);
			}
		}
		const float *mapping[2] = {maps[0].data(), maps[1].data()};
		renderer = render_camera_create(&vk, vk.main_queue, vert, frag, VK_FORMAT_R8G8B8A8_UNORM,
		                                {width, height}, {cw, ch}, map_size, mapping, images, views,
		                                VK_IMAGE_LAYOUT_GENERAL, destinations, VK_IMAGE_LAYOUT_GENERAL, 3);
		REQUIRE(renderer != nullptr);
	}

	void
	clear(unsigned index, unsigned alpha)
	{
		auto cmd = f.begin();
		auto image = sources[index];
		if (output != nullptr)
			image.image = render_output_image(output, index);
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue value{};
		value.float32[0] = 40.0f / 255;
		value.float32[1] = 30.0f / 255;
		value.float32[2] = 20.0f / 255;
		value.float32[3] = float(alpha) / 255;
		f.vk().vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &color_range);
		f.finish(cmd);
	}
	void
	signal(unsigned index)
	{
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &complete[index];
		vk_queue_lock(f.vk().main_queue);
		VkResult ret = f.vk().vkQueueSubmit(f.vk().main_queue->queue, 1, &submit, VK_NULL_HANDLE);
		vk_queue_unlock(f.vk().main_queue);
		REQUIRE(ret == VK_SUCCESS);
	}
	void
	consume(unsigned index)
	{
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &complete[index];
		submit.pWaitDstStageMask = &stage;
		VkFenceCreateInfo fi{};
		fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence{};
		REQUIRE(f.vk().vkCreateFence(f.vk().device, &fi, nullptr, &fence) == VK_SUCCESS);
		vk_queue_lock(f.vk().main_queue);
		VkResult ret = f.vk().vkQueueSubmit(f.vk().main_queue->queue, 1, &submit, fence);
		vk_queue_unlock(f.vk().main_queue);
		REQUIRE(ret == VK_SUCCESS);
		REQUIRE(f.vk().vkWaitForFences(f.vk().device, 1, &fence, VK_TRUE, 1000000000ull) == VK_SUCCESS);
		f.vk().vkDestroyFence(f.vk().device, fence, nullptr);
	}
	std::vector<uint8_t>
	pixels(unsigned index)
	{
		render_buffer readback{};
		REQUIRE(render_buffer_init(&f.vk(), &readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_memory,
		                           width * height * 4) == VK_SUCCESS);
		REQUIRE(render_buffer_map(&f.vk(), &readback) == VK_SUCCESS);
		auto cmd = f.begin();
		auto &image = outputs[index];
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		          VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {width, height, 1};
		f.vk().vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
		                              1, &copy);
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		f.finish(cmd);
		auto *bytes = static_cast<uint8_t *>(readback.mapped);
		std::vector<uint8_t> result(bytes, bytes + width * height * 4);
		render_buffer_fini(&f.vk(), &readback);
		return result;
	}
	~CameraFixture()
	{
		if (!f.vk().device)
			return;
		CHECK(render_camera_destroy(&renderer) == VK_SUCCESS);
		CHECK(render_output_destroy(&output) == VK_SUCCESS);
		for (auto sem : complete)
			if (sem)
				f.vk().vkDestroySemaphore(f.vk().device, sem, nullptr);
		for (auto *images : {&outputs, &sources})
			for (auto &image : *images) {
				if (image.view)
					f.vk().vkDestroyImageView(f.vk().device, image.view, nullptr);
				if (image.image)
					f.vk().vkDestroyImage(f.vk().device, image.image, nullptr);
				if (image.memory)
					f.vk().vkFreeMemory(f.vk().device, image.memory, nullptr);
			}
		for (auto shader : {vert, frag, output_shader})
			if (shader)
				f.vk().vkDestroyShaderModule(f.vk().device, shader, nullptr);
	}
};

unsigned
camera_value(unsigned eye, unsigned seed, unsigned x, unsigned y)
{
	return 20 + ((x / 4) * 37 + (y / 4) * 53 + eye * 71 + seed * 19) % 180;
}
std::vector<uint8_t>
bc4(unsigned eye, unsigned seed)
{
	// Deliberately padded block rows: verify frame strides are respected.
	std::vector<uint8_t> data(64, 0);
	for (unsigned y = 0; y < 2; y++)
		for (unsigned x = 0; x < 3; x++) {
			data[y * 32 + x * 8] = data[y * 32 + x * 8 + 1] = camera_value(eye, seed, x * 4, y * 4);
		}
	return data;
}
double
camera_sample(unsigned eye, unsigned seed, xrt_vec2 uv)
{
	double x = std::clamp(double(uv.x) * CameraFixture::cw - .5, 0.0, double(CameraFixture::cw - 1));
	double y = std::clamp(double(uv.y) * CameraFixture::ch - .5, 0.0, double(CameraFixture::ch - 1));
	unsigned ix = unsigned(x), iy = unsigned(y);
	double fx = x - ix, fy = y - iy;
	auto value = [&](unsigned dx, unsigned dy) {
		return camera_value(eye, seed, std::min(ix + dx, CameraFixture::cw - 1),
		                    std::min(iy + dy, CameraFixture::ch - 1)) /
		       255.0;
	};
	return (1 - fy) * ((1 - fx) * value(0, 0) + fx * value(1, 0)) +
	       fy * ((1 - fx) * value(0, 1) + fx * value(1, 1));
}
} // namespace

TEST_CASE("Vulkan camera composition preserves stereo, alpha and calibrated rotation", "[aux_render][gpu][camera]")
{
	bool private_output = GENERATE(false, true);
	CAPTURE(private_output);
	CameraFixture cf;
	cf.init(private_output);
	static_assert(sizeof(render_camera_params) == 176);
	for (unsigned round = 0; round < 7; round++) {
		for (unsigned slot = 0; slot < 3; slot++)
			cf.clear(slot, slot == 2 ? 255 : 80);
		std::array<std::vector<uint8_t>, 3> expected;
		// Queue distinct snapshots in all three slots before consuming any result.
		for (unsigned slot = 0; slot < 3; slot++) {
			CAPTURE(round, slot);
			unsigned seed = round * 3 + slot;
			unsigned alpha = slot == 2 ? 255 : 80;
			render_camera_params params{};
			params.brightness = 1.6f;
			params.has_app = slot != 0;
			params.map_size = cf.map_size;
			params.rotate = round != 0;
			auto cal = cf.calibration;
			for (unsigned eye = 0; eye < 2; eye++) {
				xrt_vec3 axis{};
				if ((round + 2) % 3 == 0)
					axis.x = 1;
				else if ((round + 2) % 3 == 1)
					axis.y = 1;
				else
					axis.z = 1;
				xrt_quat rotation{};
				math_quat_from_angle_vector(round ? (round % 2 ? .4f : -.4f) : 0.0f, &axis, &rotation);
				math_quat_invert(&rotation, &cal[eye].head_from_camera.orientation);
				auto &c = params.cameras[eye];
				c.intrinsics[0] = float(cal[eye].fx * cf.cw / 1024);
				c.intrinsics[1] = float(cal[eye].fy * cf.ch / 1016);
				c.intrinsics[2] = float((cal[eye].cx + .5) * cf.cw / 1024 - .5);
				c.intrinsics[3] = float((cal[eye].cy + .5) * cf.ch / 1016 - .5);
				for (unsigned k = 0; k < 4; k++)
					c.k[k] = float(cal[eye].k[k]);
				for (unsigned col = 0; col < 3; col++) {
					xrt_vec3 basis{}, value{};
					if (col == 0)
						basis.x = 1;
					if (col == 1)
						basis.y = 1;
					if (col == 2)
						basis.z = 1;
					math_quat_rotate_vec3(&rotation, &basis, &value);
					c.rows[0][col] = value.x;
					c.rows[1][col] = value.y;
					c.rows[2][col] = value.z;
				}
			}
			expected[slot].resize(cf.width * cf.height * 4);
			for (unsigned y = 0; y < cf.height; y++)
				for (unsigned x = 0; x < cf.width; x++) {
					unsigned eye = x >= cf.width / 2;
					unsigned mx =
					    ((x % (cf.width / 2)) * cf.map_size + cf.map_size / 2) / (cf.width / 2);
					unsigned my = (y * cf.map_size + cf.map_size / 2) / cf.height;
					mx = std::min(mx, cf.map_size - 1);
					my = std::min(my, cf.map_size - 1);
					size_t m = (my * cf.map_size + mx) * 4;
					xrt_vec2 uv{cf.maps[eye][m], cf.maps[eye][m + 1]};
					bool valid = uv.x >= 0 && uv.y >= 0;
					if (params.rotate) {
						xrt_vec3 ray{cf.maps[eye][m + 2], -cf.maps[eye][m + 3], -1};
						valid = u_passthrough_calibration_project(&cal[eye], &ray, &uv);
					}
					double gray =
					    valid ? std::min(1.0, camera_sample(eye, seed, uv) * params.brightness) : 0;
					size_t n = (y * cf.width + x) * 4;
					for (unsigned channel = 0; channel < 3; channel++) {
						double value = params.has_app ? double(40 - channel * 10) / 255 +
						                                    gray * (1 - double(alpha) / 255)
						                              : gray;
						expected[slot][n + channel] =
						    uint8_t(std::lround(std::min(1.0, value) * 255));
					}
					expected[slot][n + 3] = 255;
				}
			auto left = bc4(0, seed), right = bc4(1, seed);
			xrt_frame frames[2]{};
			for (unsigned eye = 0; eye < 2; eye++) {
				frames[eye].format = XRT_FORMAT_BC4;
				frames[eye].width = cf.cw;
				frames[eye].height = cf.ch;
				frames[eye].stride = 32;
				frames[eye].size = 64;
				frames[eye].data = eye ? right.data() : left.data();
			}
			xrt_frame *source[2] = {&frames[0], &frames[1]};
			bool submitted = false;
			cf.signal(slot);
			REQUIRE(render_camera_submit(cf.renderer, slot, cf.complete[slot], source, &params,
			                             &submitted) == VK_SUCCESS);
			REQUIRE(submitted);
			// CPU source and parameters may be discarded immediately after submission.
			std::fill(left.begin(), left.end(), 0);
			std::fill(right.begin(), right.end(), 0);
		}
		for (unsigned slot = 0; slot < 3; slot++) {
			cf.consume(slot);
			auto actual = cf.pixels(slot);
			unsigned max_error = 0;
			for (size_t n = 0; n < actual.size(); n++)
				max_error =
				    std::max(max_error, unsigned(std::abs(int(actual[n]) - int(expected[slot][n]))));
			CAPTURE(round, slot, max_error);
			CHECK(max_error <= 1);
		}
	}
}

TEST_CASE("Camera input rejection leaves the producer semaphore available", "[aux_render][gpu][camera]")
{
	CameraFixture cf;
	cf.init();
	render_camera_params params{};
	params.map_size = cf.map_size;
	auto data = bc4(0, 0);
	xrt_frame frame{};
	frame.format = XRT_FORMAT_BC4;
	frame.width = cf.cw;
	frame.height = cf.ch;
	frame.stride = 1;
	frame.size = data.size();
	frame.data = data.data();
	xrt_frame *frames[2] = {&frame, &frame};
	bool submitted = true;
	cf.signal(0);
	CHECK(render_camera_submit(cf.renderer, 0, cf.complete[0], frames, &params, &submitted) ==
	      VK_ERROR_INITIALIZATION_FAILED);
	CHECK_FALSE(submitted);
	cf.consume(0); // The rejected frame must not consume or re-signal it.
}

TEST_CASE("Camera submissions return before GPU readiness and retain pending storage", "[aux_render][gpu][camera]")
{
	CameraFixture cf;
	cf.init();
	for (unsigned slot = 0; slot < 3; slot++)
		cf.clear(slot, 0);
	auto &vk = cf.f.vk();
	for (unsigned slot = 0; slot < 3; slot++)
		cf.signal(slot);
	REQUIRE(vk.vkDeviceWaitIdle(vk.device) == VK_SUCCESS);
	auto create = reinterpret_cast<PFN_vkCreateEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkCreateEvent"));
	auto wait = reinterpret_cast<PFN_vkCmdWaitEvents>(vk.vkGetDeviceProcAddr(vk.device, "vkCmdWaitEvents"));
	auto set = reinterpret_cast<PFN_vkSetEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkSetEvent"));
	auto destroy = reinterpret_cast<PFN_vkDestroyEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkDestroyEvent"));
	REQUIRE((create && wait && set && destroy));
	VkEventCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	VkEvent event{};
	VkResult created = create(vk.device, &info, nullptr, &event);
	if (created == VK_ERROR_FEATURE_NOT_PRESENT)
		SKIP("Host events unsupported on this portability device");
	REQUIRE(created == VK_SUCCESS);
	struct Gate
	{
		vk_bundle &vk;
		VkEvent event;
		PFN_vkSetEvent set;
		PFN_vkDestroyEvent destroy;
		~Gate()
		{
			set(vk.device, event);
			vk.vkDeviceWaitIdle(vk.device);
			destroy(vk.device, event, nullptr);
		}
	} gate{vk, event, set, destroy};
	auto cmd = cf.f.begin();
	wait(cmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0,
	     nullptr);
	REQUIRE(vk.vkEndCommandBuffer(cmd) == VK_SUCCESS);
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vk_queue_lock(vk.main_queue);
	VkResult ret = vk.vkQueueSubmit(vk.main_queue->queue, 1, &submit, VK_NULL_HANDLE);
	vk_queue_unlock(vk.main_queue);
	REQUIRE(ret == VK_SUCCESS);
	for (unsigned slot = 0; slot < 3; slot++) {
		auto data = bc4(slot % 2, slot);
		xrt_frame frame{};
		frame.format = XRT_FORMAT_BC4;
		frame.width = cf.cw;
		frame.height = cf.ch;
		frame.stride = 32;
		frame.size = data.size();
		frame.data = data.data();
		xrt_frame *frames[2] = {&frame, &frame};
		render_camera_params params{};
		params.map_size = cf.map_size;
		params.brightness = 1;

		bool submitted = false;
		REQUIRE(render_camera_submit(cf.renderer, slot, cf.complete[slot], frames, &params, &submitted) ==
		        VK_SUCCESS);
		REQUIRE(submitted);
	}
	// A reused output slot must not overwrite pending camera or uniform storage.
	render_camera_params replacement{};
	replacement.map_size = cf.map_size;
	auto newer = bc4(1, 7);
	xrt_frame replacement_frame{};
	replacement_frame.format = XRT_FORMAT_BC4;
	replacement_frame.width = cf.cw;
	replacement_frame.height = cf.ch;
	replacement_frame.stride = 32;
	replacement_frame.size = newer.size();
	replacement_frame.data = newer.data();
	xrt_frame *replacement_frames[2] = {&replacement_frame, &replacement_frame};
	bool resubmitted = true;
	CHECK(render_camera_submit(cf.renderer, 0, cf.complete[0], replacement_frames, &replacement, &resubmitted) ==
	      VK_TIMEOUT);
	CHECK_FALSE(resubmitted);
	// Host has not released the gate: a synchronous bridge cannot reach here.
	// Destruction must return a bounded timeout without freeing any resources.
	auto *pending = cf.renderer;
	CHECK(render_camera_destroy(&cf.renderer) == VK_TIMEOUT);
	REQUIRE(cf.renderer == pending);
	REQUIRE(set(vk.device, event) == VK_SUCCESS);
	for (unsigned slot = 0; slot < 3; slot++) {
		cf.consume(slot);
		auto pixels = cf.pixels(slot);
		unsigned x = cf.width / 4, y = cf.height / 2;
		unsigned mx = unsigned((double(x) + .5) * 2 / cf.width * cf.map_size);
		unsigned my = unsigned((double(y) + .5) / cf.height * cf.map_size);
		size_t m = (my * cf.map_size + mx) * 4;
		xrt_vec2 uv{cf.maps[0][m], cf.maps[0][m + 1]};
		int expected = int(std::lround(camera_sample(slot % 2, slot, uv) * 255));
		CHECK(std::abs(int(pixels[(y * cf.width + x) * 4]) - expected) <= 1);
	}
	CHECK(render_camera_destroy(&cf.renderer) == VK_SUCCESS);
	CHECK(cf.renderer == nullptr);
}

namespace {
VKAPI_ATTR VkResult VKAPI_CALL
camera_submit_oom(VkQueue, uint32_t, const VkSubmitInfo *, VkFence)
{
	return VK_ERROR_OUT_OF_HOST_MEMORY;
}
} // namespace

TEST_CASE("Camera queue OOM preserves synchronization and permits retry", "[aux_render][gpu][camera]")
{
	CameraFixture cf;
	cf.init();
	cf.clear(0, 0);
	auto data = bc4(0, 3);
	xrt_frame frame{};
	frame.format = XRT_FORMAT_BC4;
	frame.width = cf.cw;
	frame.height = cf.ch;
	frame.stride = 32;
	frame.size = data.size();
	frame.data = data.data();
	xrt_frame *frames[2] = {&frame, &frame};
	render_camera_params params{};
	params.map_size = cf.map_size;
	params.brightness = 1;
	bool submitted = true;
	cf.signal(0);
	{
		struct Override
		{
			vk_bundle &vk;
			PFN_vkQueueSubmit original;
			~Override()
			{
				vk.vkQueueSubmit = original;
			}
		} restore{cf.f.vk(), cf.f.vk().vkQueueSubmit};
		cf.f.vk().vkQueueSubmit = camera_submit_oom;
		CHECK(render_camera_submit(cf.renderer, 0, cf.complete[0], frames, &params, &submitted) ==
		      VK_ERROR_OUT_OF_HOST_MEMORY);
		CHECK_FALSE(submitted);
	}
	cf.consume(0); // The failed call must leave the original signal untouched.
	cf.signal(0);
	REQUIRE(render_camera_submit(cf.renderer, 0, cf.complete[0], frames, &params, &submitted) == VK_SUCCESS);
	CHECK(submitted);
	cf.consume(0);
	auto pixels = cf.pixels(0);
	CHECK(std::any_of(pixels.begin(), pixels.end(), [](uint8_t v) { return v != 0 && v != 255; }));
}
