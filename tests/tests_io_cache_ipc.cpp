// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Real IPC cache transport with synthetic devices, without hardware.
 */
#include "catch_amalgamated.hpp"
#include "b_io_cache.h"
#include "client/ipc_client.h"
#include "client/ipc_client_io_cache.h"
#include "ipc_client_generated.h"
// Client and server headers use the same logging macro names. This fixture
// exercises both sides in one translation unit; no logging macros are used here.
#undef IPC_TRACE
#undef IPC_DEBUG
#undef IPC_INFO
#undef IPC_WARN
#undef IPC_ERROR
#undef IPC_CHK_AND_RET
#undef IPC_CHK_WITH_GOTO
#undef IPC_CHK_WITH_RET
#undef IPC_CHK_ONLY_PRINT
#undef IPC_CHK_ALWAYS_RET
#include "ipc_server_generated.h"
#include "server/ipc_server_thread_shutdown.h"
#include "xrt/xrt_io_cache.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"
#include "os/os_threading.h"
#include "util/u_hashmap.h"
#include "util/u_time.h"
#include <oxr/actions/oxr_binding_registry.h>
#include <oxr/actions/oxr_binding_profile.h>
#include <oxr/actions/oxr_input.h>
#include <oxr/actions/oxr_input_transform.h>
#include <oxr/actions/oxr_resolver_for_bindings.h>
#include <oxr/actions/oxr_session_attached_actions.h>
#include <oxr/actions/oxr_session_action_context.h>
#include <oxr/oxr_logger.h>
#include <oxr/oxr_objects.h>
#include <oxr/path/oxr_path_store.h>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
struct TestSystem : xrt_system_devices
{
	std::atomic<uint64_t> generation{1};
	TestSystem() : xrt_system_devices{}
	{
		get_roles = [](xrt_system_devices *base, xrt_system_roles *roles) {
			*roles = XRT_SYSTEM_ROLES_INIT;
			roles->generation_id = static_cast<TestSystem *>(base)->generation.load();
			roles->left = 0;
			return XRT_SUCCESS;
		};
		create_io_cache = b_io_cache_create;
	}
};

struct TestDevice : xrt_device
{
	std::atomic<float> sample{0.25f};
	std::atomic<int> last_output{XRT_OUTPUT_NAME_INVALID};
	std::atomic<float> amplitude{0};
	std::vector<float> pcm_samples;
	xrt_input input_storage[2] = {};
	xrt_output output_storage[1] = {};
	xrt_binding_input_pair input_pairs[2] = {
	    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
	    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
	};
	xrt_binding_output_pair output_pairs[1] = {
	    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_PSSENSE_VIBRATION},
	};
	xrt_binding_profile fallback = {};
	TestDevice() : xrt_device{}
	{
		id.val = 42;
		name = XRT_DEVICE_PSSENSE;
		inputs = input_storage;
		input_count = 2;
		inputs[0].name = XRT_INPUT_PSSENSE_TRIGGER_VALUE;
		inputs[1].name = XRT_INPUT_PSSENSE_GRIP_POSE;
		outputs = output_storage;
		output_count = 1;
		outputs[0].name = XRT_OUTPUT_NAME_PSSENSE_VIBRATION;
		fallback.name = XRT_DEVICE_SIMPLE_CONTROLLER;
		fallback.inputs = input_pairs;
		fallback.input_count = 2;
		fallback.outputs = output_pairs;
		fallback.output_count = 1;
		binding_profiles = &fallback;
		binding_profile_count = 1;
		update_inputs = [](xrt_device *base) {
			auto *self = static_cast<TestDevice *>(base);
			self->inputs[0].value.vec1.x = self->sample.load();
			self->inputs[0].timestamp = 123456;
			self->inputs[0].active = true;
			return XRT_SUCCESS;
		};
		set_output = [](xrt_device *base, xrt_output_name output, const xrt_output_value *value) {
			auto *self = static_cast<TestDevice *>(base);
			self->last_output = output;
			if (value->type == XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION) {
				const auto &pcm = value->pcm_vibration;
				self->pcm_samples.assign(pcm.buffer, pcm.buffer + pcm.buffer_size);
				*pcm.samples_consumed = pcm.buffer_size;
			} else {
				self->amplitude = value->vibration.amplitude;
			}
			return XRT_SUCCESS;
		};
	}
};

struct TestOverseer : xrt_space_overseer
{
	std::atomic<int> last_input{XRT_INPUT_NAME_INVALID};
	TestOverseer() : xrt_space_overseer{}
	{
		create_pose_space = [](xrt_space_overseer *base, xrt_device *, xrt_input_name input, xrt_space **out) {
			static_cast<TestOverseer *>(base)->last_input = input;
			auto *space = new xrt_space{};
			space->reference.count = 1;
			space->destroy = [](xrt_space *value) { delete value; };
			*out = space;
			return XRT_SUCCESS;
		};
	}
};

struct Transport
{
	TestDevice device;
	TestSystem system;
	TestOverseer overseer;
	std::unique_ptr<ipc_server> server = std::make_unique<ipc_server>();
	std::unique_ptr<ipc_client_state> state = std::make_unique<ipc_client_state>();
	ipc_connection connection = {};
	xrt_io_cache *cache = nullptr;
	std::thread worker;
	std::atomic<int> dispatch_result{XRT_SUCCESS};
	Transport(bool index_profile = false)
	{
		os_mutex_init(&server->global_state.lock);
		if (index_profile) {
			device.fallback.name = XRT_DEVICE_INDEX_CONTROLLER;
			device.input_pairs[0].from = XRT_INPUT_INDEX_TRIGGER_VALUE;
			device.input_pairs[1].from = XRT_INPUT_INDEX_GRIP_POSE;
			device.output_pairs[0].from = XRT_OUTPUT_NAME_INDEX_HAPTIC;
		}
		system.static_xdev_count = 1;
		system.static_xdevs[0] = &device;
		system.static_roles.head = &device;
		system.static_roles.eyes = &device;
		server->xsysd = &system;
		server->xso = &overseer;
		server->log_level = U_LOGGING_ERROR;
		state->server = server.get();
		int sockets[2];
		REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
		connection.imc.ipc_handle = sockets[0];
		connection.imc.log_level = U_LOGGING_ERROR;
		state->imc.ipc_handle = sockets[1];
		os_mutex_init(&connection.mutex);
		os_mutex_init(&connection.send_mutex);
		worker = std::thread([this] {
			for (;;) {
				enum ipc_command command;
				if (recv(state->imc.ipc_handle, &command, sizeof(command), MSG_PEEK | MSG_WAITALL) !=
				    static_cast<ssize_t>(sizeof(command))) {
					break;
				}
				size_t size = ipc_command_size(command);
				alignas(8) unsigned char bytes[IPC_BUF_SIZE] = {};
				if (size == 0 || size > sizeof(bytes) ||
				    recv(state->imc.ipc_handle, bytes, size, MSG_WAITALL) != (ssize_t)size) {
					dispatch_result = XRT_ERROR_IPC_FAILURE;
					break;
				}
				auto result = ipc_dispatch(state.get(), reinterpret_cast<ipc_command_t *>(bytes));
				if (result != XRT_SUCCESS) {
					dispatch_result = result;
					break;
				}
			}
		});
		uint32_t id = 0;
		REQUIRE(ipc_call_io_cache_create(&connection, &id) == XRT_SUCCESS);
		REQUIRE(ipc_client_io_cache_create(&connection, id, &cache) == XRT_SUCCESS);
	}
	~Transport()
	{
		xrt_io_cache_destroy(&cache);
		shutdown(connection.imc.ipc_handle, SHUT_RDWR);
		worker.join();
		close(connection.imc.ipc_handle);
		close(state->imc.ipc_handle);
		os_mutex_destroy(&connection.mutex);
		os_mutex_destroy(&connection.send_mutex);
		os_mutex_destroy(&server->global_state.lock);
	}
};
} // namespace

TEST_CASE("shutdown cancels stalled handler exchanges without waiting for the global lock")
{
	struct BlockedHandler
	{
		std::unique_ptr<ipc_server> server = std::make_unique<ipc_server>();
		TestDevice device;
		TestSystem system;
		int peer = -1;
		bool haptic = false;
		xrt_result_t result = XRT_SUCCESS;
		BlockedHandler()
		{
			os_mutex_init(&server->global_state.lock);
			for (auto &thread : server->threads) {
				thread.ics.imc.ipc_handle = XRT_IPC_HANDLE_INVALID;
			}
			system.static_xdev_count = 1;
			system.static_xdevs[0] = &device;
			server->xsysd = &system;
			server->log_level = U_LOGGING_ERROR;
		}
		~BlockedHandler()
		{
			ipc_server_stop_and_join_client_threads(server.get());
			if (peer >= 0) {
				close(peer);
			}
			auto handle = server->threads[0].ics.imc.ipc_handle;
			if (handle >= 0) {
				close(handle);
			}
			os_mutex_destroy(&server->global_state.lock);
		}
		static void *
		run(void *ptr)
		{
			auto &f = *static_cast<BlockedHandler *>(ptr);
			auto &thread = f.server->threads[0];
			ipc_server_client_thread_set_running(f.server.get(), &thread);
			if (f.haptic) {
				ipc_pcm_haptic_buffer samples = {};
				samples.num_samples = 64;
				samples.sample_rate = 320;
				f.result = ipc_handle_device_set_haptic_output(
				    &thread.ics, 0, XRT_OUTPUT_NAME_PSSENSE_VIBRATION, &samples);
			} else {
				xrt_pose offset = XRT_POSE_IDENTITY;
				f.result = ipc_handle_space_locate_spaces(&thread.ics, 0, &offset, 64, 1);
			}
			return nullptr;
		}
	} f;
	SECTION("space locations") {}
	SECTION("PCM samples")
	{
		f.haptic = true;
	}
	int sockets[2];
	REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	f.peer = sockets[0];
	auto &thread = f.server->threads[0];
	thread.ics.server = f.server.get();
	thread.ics.objects.xdevs[0] = &f.device;
	thread.ics.imc.ipc_handle = sockets[1];
	thread.ics.imc.log_level = U_LOGGING_ERROR;
	thread.state = IPC_THREAD_STARTING;
	int started = os_thread_start(&thread.thread, BlockedHandler::run, &f);
	if (started != 0) {
		thread.state = IPC_THREAD_READY;
	}
	REQUIRE(started == 0);
	ipc_message_channel channel = {f.peer, U_LOGGING_ERROR};
	xrt_result_t allocated = XRT_ERROR_ALLOCATION;
	REQUIRE(ipc_receive(&channel, &allocated, sizeof(allocated)) == XRT_SUCCESS);
	REQUIRE(allocated == XRT_SUCCESS);
	// Deliberately withhold the payload after the real handler acknowledges allocation.
	ipc_server_stop_and_join_client_threads(f.server.get());
	CHECK(f.result == XRT_ERROR_IPC_FAILURE);
	CHECK(thread.state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get());
}

TEST_CASE("IO cache PCM transport bounds samples and completes blocked output exchanges")
{
	Transport f;
	bool blocked = false;
	SECTION("native samples are capped to the protocol capacity") {}
	SECTION("blocked outputs consume the exchange without calling the device")
	{
		blocked = true;
	}
	f.state->client_state.io_blocks.block_outputs = blocked;
	std::vector<float> samples(IPC_MAX_PCM_HAPTIC_SAMPLES + 17);
	for (size_t i = 0; i < samples.size(); ++i) {
		samples[i] = static_cast<float>(i % 17) / 17.0f;
	}
	uint32_t consumed = UINT32_MAX;
	xrt_output_value value = {};
	value.type = XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION;
	value.pcm_vibration.buffer = samples.data();
	value.pcm_vibration.buffer_size = static_cast<uint32_t>(samples.size());
	value.pcm_vibration.sample_rate = 8000;
	value.pcm_vibration.samples_consumed = &consumed;
	REQUIRE(xrt_io_cache_set_output(f.cache, f.device.id, XRT_OUTPUT_NAME_PSSENSE_VIBRATION, &value) ==
	        XRT_SUCCESS);
	if (blocked) {
		CHECK(consumed == 0);
		CHECK(f.device.last_output == XRT_OUTPUT_NAME_INVALID);
	} else {
		CHECK(consumed == IPC_MAX_PCM_HAPTIC_SAMPLES);
		CHECK(f.device.last_output == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
		REQUIRE(f.device.pcm_samples.size() == IPC_MAX_PCM_HAPTIC_SAMPLES);
		CHECK(std::equal(f.device.pcm_samples.begin(), f.device.pcm_samples.end(), samples.begin()));
	}
	// A subsequent command must still be aligned after the variable-length exchange.
	xrt_io_cache_info info = {};
	CHECK(xrt_io_cache_get_info(f.cache, &info) == XRT_SUCCESS);
	CHECK(info.device_count == 1);
}

TEST_CASE("IO cache IPC carries compatibility metadata and native sample types")
{
	Transport f;
	xrt_io_cache_info info = {};
	REQUIRE(xrt_io_cache_get_info(f.cache, &info) == XRT_SUCCESS);
	CHECK(info.device_count == 1);
	CHECK(info.left == 0);
	CHECK(info.head == 0);
	CHECK(info.eyes == 0);
	CHECK(info.binding_profile_count == 1);
	CHECK(info.binding_profiles[0].name == XRT_DEVICE_SIMPLE_CONTROLLER);
	CHECK(info.binding_inputs[0].from == XRT_INPUT_SIMPLE_SELECT_CLICK);
	CHECK(info.binding_inputs[0].device == XRT_INPUT_PSSENSE_TRIGGER_VALUE);
	CHECK(info.binding_outputs[0].device == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
	CHECK(info.devices[0].id.val == 42);
	CHECK(info.devices[0].binding_profile_count == 1);
	CHECK(info.input_count == 2); // Compatibility metadata adds no per-frame aliases.
	std::vector<xrt_input> inputs(info.input_count);
	std::vector<xrt_output> outputs(info.output_count);
	bool stale = true;
	for (float sample : {0.25f, 0.75f}) {
		f.device.sample = sample;
		REQUIRE(xrt_io_cache_read(f.cache, info.generation_id, info.input_count, inputs.data(),
		                          info.output_count, outputs.data(), &stale) == XRT_SUCCESS);
		CHECK_FALSE(stale);
		CHECK(inputs[0].name == XRT_INPUT_PSSENSE_TRIGGER_VALUE);
		CHECK(inputs[0].value.vec1.x == sample);
		CHECK(inputs[0].active);
		CHECK(inputs[0].timestamp == 123456);
	}
	f.system.generation = 2;
	inputs[0].timestamp = 77;
	REQUIRE(xrt_io_cache_read(f.cache, info.generation_id, info.input_count, inputs.data(), info.output_count,
	                          outputs.data(), &stale) == XRT_SUCCESS);
	CHECK(stale);
	CHECK(inputs[0].timestamp == 77);
	// An invalid current-generation read returns an error, then the connection remains usable.
	REQUIRE(xrt_io_cache_read(f.cache, 2, 0, nullptr, 0, nullptr, &stale) == XRT_ERROR_INVALID_ARGUMENT);
	REQUIRE(xrt_io_cache_get_info(f.cache, &info) == XRT_SUCCESS);
	CHECK(info.generation_id == 2);
	CHECK(f.dispatch_result == XRT_SUCCESS);
}

TEST_CASE("IO cache IPC routes haptics and pose spaces by native device ID")
{
	Transport f;
	xrt_space *space = nullptr;
	REQUIRE(xrt_io_cache_create_space(f.cache, f.device.id, XRT_INPUT_PSSENSE_GRIP_POSE, &space) == XRT_SUCCESS);
	CHECK(f.overseer.last_input == XRT_INPUT_PSSENSE_GRIP_POSE);
	xrt_space_reference(&space, nullptr);
	xrt_output_value value = {};
	value.vibration.amplitude = 0.625f;
	REQUIRE(xrt_io_cache_set_output(f.cache, f.device.id, XRT_OUTPUT_NAME_PSSENSE_VIBRATION, &value) ==
	        XRT_SUCCESS);
	CHECK(f.device.last_output == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
	CHECK(f.device.amplitude == 0.625f);
	xrt_device_id missing = {1234};
	CHECK(xrt_io_cache_set_output(f.cache, missing, XRT_OUTPUT_NAME_PSSENSE_VIBRATION, &value) ==
	      XRT_ERROR_INVALID_ARGUMENT);
	CHECK(xrt_io_cache_create_space(f.cache, missing, XRT_INPUT_PSSENSE_GRIP_POSE, &space) ==
	      XRT_ERROR_INVALID_ARGUMENT);
	CHECK(f.dispatch_result == XRT_SUCCESS);
}

TEST_CASE("IO cache metadata limits reject oversized driver tables")
{
	TestDevice device;
	TestSystem system;
	TestOverseer overseer;
	system.static_xdev_count = 1;
	system.static_xdevs[0] = &device;
	xrt_io_cache *cache = nullptr;
	REQUIRE(b_io_cache_create(&system, &overseer, &cache) == XRT_SUCCESS);
	const int which = GENERATE(0, 1, 2);
	if (which == 0)
		device.binding_profile_count = XRT_IO_CACHE_MAX_BINDING_PROFILES + 1;
	if (which == 1)
		device.fallback.input_count = XRT_IO_CACHE_MAX_BINDING_INPUTS + 1;
	if (which == 2)
		device.fallback.output_count = XRT_IO_CACHE_MAX_BINDING_OUTPUTS + 1;
	xrt_io_cache_info info = {};
	CHECK(xrt_io_cache_get_info(cache, &info) == XRT_ERROR_INVALID_ARGUMENT);
	xrt_io_cache_destroy(&cache);
}

TEST_CASE("OpenXR action priority and overrides work with an IPC-backed cache")
{
	Transport f(true);
	struct ActionState
	{
		oxr_logger log = {};
		oxr_path_store store = {};
		oxr_binding_registry *registry = nullptr;
		oxr_instance instance = {};
		oxr_system system = {};
		oxr_session session = {};
		oxr_action_ref refs[2] = {};
		oxr_action_set_ref set_refs[2] = {};
		oxr_action_set handles[2] = {};
		oxr_action_set_attachment sets[2] = {};
		oxr_action_attachment actions[2] = {};
		ActionState(Transport &transport)
		{
			oxr_log_init(&log, "io-cache-priority");
			REQUIRE(oxr_path_store_init(&store) == XR_SUCCESS);
			REQUIRE(oxr_binding_registry_create(&log, &store, &registry) == XR_SUCCESS);
			instance.timekeeping = time_state_create(0);
			system.inst = &instance;
			system.xsysd = &transport.system;
			session.sys = &system;
			session.state = XR_SESSION_STATE_FOCUSED;
			auto &context = session.action_context;
			context.io_cache.xic = transport.cache;
			REQUIRE(oxr_io_cache_recreate(&log, &context.io_cache) == XR_SUCCESS);
			context.dynamic_roles_generation_id = 1;
			REQUIRE(os_mutex_init(&context.sync_actions_mutex) == 0);
			REQUIRE(u_hashmap_int_create(&context.act_sets_attachments_by_key) == 0);
			REQUIRE(oxr_priority_group_cache_init(&context.priority_group_cache) == XR_SUCCESS);
			REQUIRE(oxr_session_attached_actions_init(&session.attached_actions) == XR_SUCCESS);
			context.act_set_attachments = sets;
			context.action_set_attachment_count = 2;
			for (int i = 0; i < 2; i++) {
				refs[i].action_type = XR_ACTION_TYPE_FLOAT_INPUT;
				refs[i].subaction_paths.left = true;
				set_refs[i].priority = i == 0 ? 4 : 9;
				handles[i].act_set_key = i + 1;
				handles[i].data = &set_refs[i];
				sets[i].act_set_key = i + 1;
				sets[i].act_set_ref = &set_refs[i];
				sets[i].sess_context = &context;
				sets[i].action_attachment_count = 1;
				sets[i].act_attachments = &actions[i];
				actions[i].act_key = i + 1;
				actions[i].act_ref = &refs[i];
				actions[i].act_set_attached = &sets[i];
				REQUIRE(u_hashmap_int_insert(context.act_sets_attachments_by_key, i + 1, &sets[i]) >=
				        0);
				REQUIRE(oxr_session_attached_actions_add_action_attachment(&session.attached_actions,
				                                                           &actions[i]) == XR_SUCCESS);
			}
		}
		~ActionState()
		{
			for (auto &action : actions) {
				for (size_t i = 0; i < action.left.input_count; i++)
					oxr_input_transform_destroy(&action.left.inputs[i].transforms);
				free(action.left.inputs);
			}
			auto &context = session.action_context;
			context.io_cache.xic = nullptr; // Transport owns the backing IPC cache.
			oxr_io_cache_fini(&context.io_cache);
			u_hashmap_int_destroy(&context.act_sets_attachments_by_key);
			os_mutex_destroy(&context.sync_actions_mutex);
			oxr_priority_group_cache_fini(&context.priority_group_cache);
			oxr_session_attached_actions_fini(&session.attached_actions);
			time_state_destroy(&instance.timekeeping);
			oxr_binding_registry_unref(registry);
			oxr_path_store_fini(&store);
		}
		XrPath
		path(const char *str)
		{
			XrPath result = XR_NULL_PATH;
			REQUIRE(oxr_path_store_get_or_create(&store, str, strlen(str), &result) == XR_SUCCESS);
			return result;
		}
	} state(f);
	auto &context = state.session.action_context;
	const oxr_binding_profile *binding_profile = nullptr;
	REQUIRE(oxr_binding_registry_find_profile(
	    state.registry, state.path("/interaction_profiles/valve/index_controller"), &binding_profile));
	oxr_suggested_profile profile = {};
	profile.path = binding_profile->path;
	profile.templ = binding_profile->templ;
	oxr_suggested_profile *profiles[] = {&profile};
	oxr_suggested_profile_array array = {profiles, 1};
	oxr_profile_resolver_info info = {};
	oxr_sink_logger slog = {};
	oxr_resolver_populate_profile(&state.log, &slog, state.registry, &array, &context.io_cache,
	                              &context.io_cache.info.devices[0], state.path("/user/hand/left"),
	                              XRT_DEVICE_INVALID, &info);
	REQUIRE(info.compatibility_profile != nullptr);
	const oxr_binding_point *point = nullptr;
	REQUIRE(oxr_binding_profile_find_binding_point(binding_profile,
	                                               state.path("/user/hand/left/input/trigger/value"), &point));
	oxr_suggested_binding bindings[2] = {{1, point->path, point}, {2, point->path, point}};
	profile.bindings = bindings;
	profile.binding_count = 2;
	for (auto &action : state.actions) {
		oxr_resolver_populate_input(&state.log, &slog, &context.io_cache, &info,
		                            &context.io_cache.info.devices[0], state.path("/user/hand/left"), &action,
		                            &action.left);
		REQUIRE(action.left.input_count == 1);
	}
	oxr_slog_cancel(&slog);
	CHECK(state.actions[0].left.inputs[0].priority_group_state ==
	      state.actions[1].left.inputs[0].priority_group_state);
	XrActiveActionSet active[2] = {
	    {reinterpret_cast<XrActionSet>(&state.handles[0]), XR_NULL_PATH},
	    {reinterpret_cast<XrActionSet>(&state.handles[1]), XR_NULL_PATH},
	};
	auto sync = [&](const XrActiveActionSetPrioritiesEXT *overrides) {
		bool changed = false;
		REQUIRE(oxr_action_sync_data_with_context(&state.log, &state.session, &context, 2, active, overrides,
		                                          &changed) == XR_SUCCESS);
	};
	auto count = [&](uint32_t key) {
		uint32_t result = 0;
		REQUIRE(oxr_action_enumerate_bound_sources(&state.log, &state.session, key, 0, &result, nullptr) ==
		        XR_SUCCESS);
		return result;
	};
	sync(nullptr);
	CHECK_FALSE(state.actions[0].left.current.active);
	CHECK(state.actions[1].left.current.active);
	CHECK(state.actions[1].left.current.value.vec1.x == 0.25f);
	CHECK(count(1) == 0);
	CHECK(count(2) == 1);
	XrActiveActionSetPriorityEXT priority = {active[0].actionSet, 12};
	XrActiveActionSetPrioritiesEXT override_info = {};
	override_info.type = XR_TYPE_ACTIVE_ACTION_SET_PRIORITIES_EXT;
	override_info.actionSetPriorityCount = 1;
	override_info.actionSetPriorities = &priority;
	f.device.sample = 0.75f;
	sync(&override_info);
	CHECK(state.actions[0].left.current.active);
	CHECK(state.actions[0].left.current.value.vec1.x == 0.75f);
	CHECK_FALSE(state.actions[1].left.current.active);
	CHECK(count(1) == 1);
	CHECK(count(2) == 0);
	// Priority overrides expire on the next sync.
	sync(nullptr);
	CHECK_FALSE(state.actions[0].left.current.active);
	CHECK(state.actions[1].left.current.active);
	CHECK(count(1) == 0);
	CHECK(count(2) == 1);
	bool changed = false;
	REQUIRE(oxr_action_sync_data_with_context(&state.log, &state.session, &context, 0, nullptr, nullptr,
	                                          &changed) == XR_SUCCESS);
	CHECK(count(1) == 0);
	CHECK(count(2) == 0);
}
