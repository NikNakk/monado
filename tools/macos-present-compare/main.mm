// SPDX-License-Identifier: BSL-1.0
// Standalone experiment: no Monado, OpenXR, drivers, or private Apple APIs.
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static double
now_s()
{
	return CACurrentMediaTime();
}

static void
check(VkResult r, const char *what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string(what) + ": VkResult " + std::to_string(r));
}
#define VKCHECK(call) check(call, #call)

struct Options
{
	std::string mode = "native", policy = "minimum", display = "PS VR2", trace = "capture.csv", shaders = ".build";
	uint32_t frames = 3600, burn = 0, delay_every = 0;
	double delay_ms = 0, offset_ms = 0, minimum_ms = 8;
	bool windowed = false, list = false, validation = false;
};

static const char *usage =
    "present-compare [--mode native|wsi-copy|wsi-direct] [--policy immediate|timed|minimum]\n"
    "  --display-name NAME   exact NSScreen name; default PS VR2; no fallback\n"
    "  --windowed            960x540 test window (default covers selected display)\n"
    "  --frames N           submitted frames, default 3600, includes warm-up\n"
    "  --burn N             fragment shader iterations per pixel, default 0\n"
    "  --delay-ms N --delay-every N   delay every Nth frame after pacing, before acquire\n"
    "  --offset-ms N        timed presentation offset from CVDisplayLink output\n"
    "  --minimum-ms N       native minimum duration, default 8\n"
    "  --trace PATH         CSV (metadata written to PATH.meta.txt)\n"
    "  --shader-dir PATH    directory with vert.spv and frag.spv, default .build\n"
    "  --validation         require VK_LAYER_KHRONOS_validation\n"
    "  --list-displays      list displays without presenting\n"
    "  --help\n"
    "WSI always uses FIFO. Set --policy immediate or timed explicitly for WSI.\n";

static Options
parse(int argc, char **argv)
{
	Options o;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--help") {
			std::printf("%s", usage);
			std::exit(0);
		}
		if (a == "--windowed") {
			o.windowed = true;
			continue;
		}
		if (a == "--list-displays") {
			o.list = true;
			continue;
		}
		if (a == "--validation") {
			o.validation = true;
			continue;
		}
		if (++i == argc)
			throw std::runtime_error("Missing value for " + a);
		std::string v = argv[i];
		auto num = [&]() {
			size_t end = 0;
			double n = std::stod(v, &end);
			if (end != v.size() || !std::isfinite(n))
				throw std::runtime_error("Invalid number: " + v);
			return n;
		};
		auto integer = [&](uint32_t limit) {
			double n = num();
			if (n < 0 || n > limit || n != std::floor(n))
				throw std::runtime_error("Invalid integer: " + v);
			return uint32_t(n);
		};
		if (a == "--mode")
			o.mode = v;
		else if (a == "--policy")
			o.policy = v;
		else if (a == "--display-name")
			o.display = v;
		else if (a == "--trace")
			o.trace = v;
		else if (a == "--shader-dir")
			o.shaders = v;
		else if (a == "--frames")
			o.frames = integer(1000000);
		else if (a == "--burn")
			o.burn = integer(100000);
		else if (a == "--delay-every")
			o.delay_every = integer(1000000);
		else if (a == "--delay-ms")
			o.delay_ms = num();
		else if (a == "--offset-ms")
			o.offset_ms = num();
		else if (a == "--minimum-ms")
			o.minimum_ms = num();
		else
			throw std::runtime_error("Unknown option: " + a);
	}
	if (o.mode != "native" && o.mode != "wsi-copy" && o.mode != "wsi-direct")
		throw std::runtime_error("Invalid mode");
	if (o.policy != "immediate" && o.policy != "timed" && o.policy != "minimum")
		throw std::runtime_error("Invalid policy");
	if (o.mode != "native" && o.policy == "minimum")
		throw std::runtime_error("WSI has no minimum-duration policy; select immediate or timed");
	if (!o.frames || o.delay_ms < 0 || o.delay_ms > 1000 || o.minimum_ms < 0 || o.minimum_ms > 1000 ||
	    std::abs(o.offset_ms) > 1000)
		throw std::runtime_error("Numeric option out of range");
	if ((o.delay_every != 0) != (o.delay_ms != 0))
		throw std::runtime_error("Specify both delay options");
	return o;
}

struct Row
{
	uint32_t id = 0;
	uint64_t tick = 0;
	double generated = 0, target = 0, acquire_begin = 0, acquire_end = 0, reuse_ms = 0;
	double submit_begin = 0, submit_end = 0, present_begin = 0, present_end = 0;
	double presented = 0, feedback_received = 0, gpu_ms = 0, copy_ms = 0, metal_ms = 0;
	uint32_t depth = 0;
	std::string status = "unsubmitted";
};

struct Slot
{
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore acquired = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFramebuffer fb = VK_NULL_HANDLE;
	IOSurfaceRef surface = nullptr;
	id<MTLTexture> texture = nil;
	bool busy = false;
	int row = -1;
};

struct Probe
{
	Options o;
	CAMetalLayer *layer = nil;
	CVDisplayLinkRef link = nullptr;
	std::mutex mutex;
	std::condition_variable cv;
	uint64_t tick = 0;
	double target = 0;
	bool stopping = false;
	std::atomic<bool> presenter_failed{false};
	std::string presenter_error;
	int pending = -1;
	std::thread presenter;
	std::vector<Row> rows;
	Slot slots[3];
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkRenderPass pass = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;
	VkSemaphore timeline = VK_NULL_HANDLE;
	id<MTLSharedEvent> event = nil;
	id<MTLCommandQueue> metal_queue = nil;
	dispatch_group_t metal_group = dispatch_group_create();
	std::vector<VkImage> images;
	std::vector<VkImageView> views;
	std::vector<VkFramebuffer> framebuffers;
	std::vector<VkSemaphore> finished;
	uint32_t family = 0, timestamp_bits = 0;
	VkExtent2D extent{};
	VkPhysicalDeviceProperties properties{};
	PFN_vkGetPastPresentationTimingGOOGLE past = nullptr;
	PFN_vkGetRefreshCycleDurationGOOGLE refresh = nullptr;
	PFN_vkExportMetalObjectsEXT export_objects = nullptr;
	std::ofstream meta;

	bool
	native() const
	{
		return o.mode == "native";
	}

	static CVReturn
	callback(CVDisplayLinkRef,
	         const CVTimeStamp *,
	         const CVTimeStamp *output,
	         CVOptionFlags,
	         CVOptionFlags *,
	         void *context)
	{
		auto *p = static_cast<Probe *>(context);
		std::lock_guard<std::mutex> lock(p->mutex);
		p->tick++;
		p->target = double(output->hostTime) / CVGetHostClockFrequency();
		p->cv.notify_all();
		return kCVReturnSuccess;
	}

	void
	barrier(VkCommandBuffer cmd,
	        VkImage image,
	        VkImageLayout before,
	        VkImageLayout after,
	        VkAccessFlags src,
	        VkAccessFlags dst)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = src;
		b.dstAccessMask = dst;
		b.oldLayout = before;
		b.newLayout = after;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &b);
	}

	VkImageView
	make_view(VkImage image)
	{
		VkImageViewCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		c.image = image;
		c.viewType = VK_IMAGE_VIEW_TYPE_2D;
		c.format = VK_FORMAT_B8G8R8A8_UNORM;
		c.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView v;
		VKCHECK(vkCreateImageView(device, &c, nullptr, &v));
		return v;
	}

	VkFramebuffer
	make_fb(VkImageView view)
	{
		VkFramebufferCreateInfo c{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		c.renderPass = pass;
		c.attachmentCount = 1;
		c.pAttachments = &view;
		c.width = extent.width;
		c.height = extent.height;
		c.layers = 1;
		VkFramebuffer f;
		VKCHECK(vkCreateFramebuffer(device, &c, nullptr, &f));
		return f;
	}

	void
	init_vulkan()
	{
		meta.open(o.trace + ".meta.txt");
		if (!meta)
			throw std::runtime_error("Cannot open metadata output");
		uint32_t count = 0;
		VKCHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
		std::vector<VkExtensionProperties> ie(count);
		VKCHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, ie.data()));
		for (auto &e : ie)
			meta << "instance_extension=" << e.extensionName << '\n';
		std::vector<const char *> extensions{VK_KHR_SURFACE_EXTENSION_NAME,
		                                     VK_EXT_METAL_SURFACE_EXTENSION_NAME};
		bool portability = std::any_of(ie.begin(), ie.end(), [](auto &e) {
			return std::string(e.extensionName) == VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
		});
		if (portability)
			extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		const char *validation = "VK_LAYER_KHRONOS_validation";
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "Monado presentation experiment";
		app.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ic.pApplicationInfo = &app;
		ic.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
		ic.enabledExtensionCount = uint32_t(extensions.size());
		ic.ppEnabledExtensionNames = extensions.data();
		ic.enabledLayerCount = o.validation ? 1 : 0;
		ic.ppEnabledLayerNames = &validation;
		VKCHECK(vkCreateInstance(&ic, nullptr, &instance));
		VkMetalSurfaceCreateInfoEXT sc{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
		sc.pLayer = layer;
		auto create_surface =
		    (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
		if (!create_surface)
			throw std::runtime_error("Missing vkCreateMetalSurfaceEXT");
		VKCHECK(create_surface(instance, &sc, nullptr, &surface));
		VKCHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
		std::vector<VkPhysicalDevice> devices(count);
		VKCHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
		for (auto candidate : devices) {
			uint32_t n = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
			std::vector<VkQueueFamilyProperties> qp(n);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, qp.data());
			for (uint32_t i = 0; i < n; ++i) {
				VkBool32 supported = false;
				VKCHECK(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported));
				if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported) {
					physical = candidate;
					family = i;
					timestamp_bits = qp[i].timestampValidBits;
					break;
				}
			}
			if (physical)
				break;
		}
		if (!physical || !timestamp_bits)
			throw std::runtime_error("No present-capable timestamp graphics queue");
		VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
		vkGetPhysicalDeviceProperties2(physical, &props);
		properties = props.properties;
		meta << "driver_name=" << driver.driverName << "\ndriver_info=" << driver.driverInfo << "\n";
		meta << "gpu=" << properties.deviceName << "\ndriver_version=" << properties.driverVersion
		     << "\napi_version=" << properties.apiVersion << "\ntimestamp_bits=" << timestamp_bits << '\n';
		VKCHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
		std::vector<VkExtensionProperties> de(count);
		VKCHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, de.data()));
		auto has = [&](const char *name) {
			return std::any_of(de.begin(), de.end(),
			                   [&](auto &e) { return std::string(e.extensionName) == name; });
		};
		for (auto &e : de)
			meta << "device_extension=" << e.extensionName << '\n';
		std::vector<const char *> enabled{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_METAL_OBJECTS_EXTENSION_NAME};
		if (has(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
			enabled.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
		if (has(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME))
			enabled.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
		else if (o.policy == "timed" && !native())
			throw std::runtime_error("Timed WSI needs VK_GOOGLE_display_timing");
		VkPhysicalDeviceTimelineSemaphoreFeatures tf{
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
		VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		features.pNext = &tf;
		vkGetPhysicalDeviceFeatures2(physical, &features);
		if (!tf.timelineSemaphore)
			throw std::runtime_error("Timeline semaphore support required");
		float priority = 1;
		VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qc.queueFamilyIndex = family;
		qc.queueCount = 1;
		qc.pQueuePriorities = &priority;
		VkExportMetalObjectCreateInfoEXT export_device{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
		export_device.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT;
		export_device.pNext = &tf;
		VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dc.pNext = &export_device;
		dc.queueCreateInfoCount = 1;
		dc.pQueueCreateInfos = &qc;
		dc.enabledExtensionCount = uint32_t(enabled.size());
		dc.ppEnabledExtensionNames = enabled.data();
		VKCHECK(vkCreateDevice(physical, &dc, nullptr, &device));
		vkGetDeviceQueue(device, family, 0, &queue);
		export_objects = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT");
		VkExportMetalDeviceInfoEXT md{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
		VkExportMetalObjectsInfoEXT mo{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &md};
		export_objects(device, &mo);
		layer.device = md.mtlDevice;
		metal_queue = [layer.device newCommandQueue];
		if (!metal_queue)
			throw std::runtime_error("Cannot create Metal command queue");
		if (has(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
			past = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(
			    device, "vkGetPastPresentationTimingGOOGLE");
			refresh = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(
			    device, "vkGetRefreshCycleDurationGOOGLE");
		}
		VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cp.queueFamilyIndex = family;
		VKCHECK(vkCreateCommandPool(device, &cp, nullptr, &pool));
		VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qp.queryCount = 9;
		VKCHECK(vkCreateQueryPool(device, &qp, nullptr, &queries));
		VkExportMetalObjectCreateInfoEXT export_event{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
		export_event.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT;
		VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &export_event};
		st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st};
		VKCHECK(vkCreateSemaphore(device, &sem, nullptr, &timeline));
		VkExportMetalSharedEventInfoEXT me{VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT};
		me.semaphore = timeline;
		mo.pNext = &me;
		export_objects(device, &mo);
		event = me.mtlSharedEvent;
		if (!event)
			throw std::runtime_error("Cannot export shared event");
	}

	VkShaderModule
	shader(const char *name)
	{
		std::ifstream in(o.shaders + "/" + name, std::ios::binary | std::ios::ate);
		if (!in || in.tellg() <= 0 || size_t(in.tellg()) % 4)
			throw std::runtime_error("Cannot read SPIR-V shader");
		std::vector<uint32_t> code(size_t(in.tellg()) / 4);
		in.seekg(0);
		in.read((char *)code.data(), code.size() * 4);
		VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		c.codeSize = code.size() * 4;
		c.pCode = code.data();
		VkShaderModule m;
		VKCHECK(vkCreateShaderModule(device, &c, nullptr, &m));
		return m;
	}

	void
	init_rendering()
	{
		VkAttachmentDescription a{};
		a.format = VK_FORMAT_B8G8R8A8_UNORM;
		a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &ref;
		VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rp.attachmentCount = 1;
		rp.pAttachments = &a;
		rp.subpassCount = 1;
		rp.pSubpasses = &sub;
		VKCHECK(vkCreateRenderPass(device, &rp, nullptr, &pass));
		VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
		VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &range;
		VKCHECK(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
		VkShaderModule vert = shader("vert.spv"), frag = shader("frag.spv");
		VkPipelineShaderStageCreateInfo stages[2]{};
		for (int i = 0; i < 2; ++i) {
			stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
			stages[i].module = i ? frag : vert;
			stages[i].pName = "main";
		}
		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
		VkRect2D scissor{{0, 0}, extent};
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.pViewports = &viewport;
		vp.scissorCount = 1;
		vp.pScissors = &scissor;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState ba{};
		ba.colorWriteMask = 15;
		VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		bs.attachmentCount = 1;
		bs.pAttachments = &ba;
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.stageCount = 2;
		gp.pStages = stages;
		gp.pVertexInputState = &vi;
		gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &rs;
		gp.pMultisampleState = &ms;
		gp.pColorBlendState = &bs;
		gp.layout = layout;
		gp.renderPass = pass;
		VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
		vkDestroyShaderModule(device, vert, nullptr);
		vkDestroyShaderModule(device, frag, nullptr);
		check(result, "vkCreateGraphicsPipelines");
	}

	void
	init_images()
	{
		if (!native()) {
			VkSurfaceCapabilitiesKHR caps;
			VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
			uint32_t count = 0;
			VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
			std::vector<VkSurfaceFormatKHR> formats(count);
			VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
			bool found = false;
			for (auto f : formats) {
				meta << "surface_format=" << f.format << "," << f.colorSpace << '\n';
				if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
				    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					found = true;
			}
			if (!found)
				throw std::runtime_error("Required BGRA8 UNORM / SRGB nonlinear surface unavailable");
			VKCHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr));
			std::vector<VkPresentModeKHR> modes(count);
			VKCHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data()));
			for (auto m : modes)
				meta << "available_present_mode=" << m << '\n';
			meta << "surface_extent=" << caps.currentExtent.width << "," << caps.currentExtent.height
			     << "\nrequested_extent=" << extent.width << "," << extent.height << std::endl;
			if (caps.currentExtent.width != UINT32_MAX &&
			    (caps.currentExtent.width != extent.width || caps.currentExtent.height != extent.height))
				throw std::runtime_error("Surface extent differs from requested extent");
			VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if ((caps.supportedUsageFlags & use) != use)
				throw std::runtime_error("Required swapchain usage unsupported");
			if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
				throw std::runtime_error("Opaque alpha unavailable");
			VkSwapchainCreateInfoKHR c{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
			c.surface = surface;
			c.minImageCount = std::max(3u, caps.minImageCount);
			if (caps.maxImageCount)
				c.minImageCount = std::min(c.minImageCount, caps.maxImageCount);
			c.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
			c.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			c.imageExtent = extent;
			c.imageArrayLayers = 1;
			c.imageUsage = use;
			c.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			c.preTransform = caps.currentTransform;
			c.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			c.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			c.clipped = VK_TRUE;
			VKCHECK(vkCreateSwapchainKHR(device, &c, nullptr, &swapchain));
			VKCHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
			images.resize(count);
			VKCHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()));
			meta << "requested_image_count=" << c.minImageCount << "\nactual_image_count=" << count
			     << "\npresent_mode=FIFO\n";
			for (auto image : images) {
				views.push_back(make_view(image));
				framebuffers.push_back(make_fb(views.back()));
				VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
				VkSemaphore sem;
				VKCHECK(vkCreateSemaphore(device, &s, nullptr, &sem));
				finished.push_back(sem);
			}
			if (refresh) {
				VkRefreshCycleDurationGOOGLE cycle;
				VKCHECK(refresh(device, swapchain, &cycle));
				meta << "wsi_refresh_ns=" << cycle.refreshDuration << '\n';
			}
		} else
			meta << "requested_image_count=3\nactual_image_count=3\npresent_mode=native\n";
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(physical, &mp);
		for (auto &s : slots) {
			VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			ca.commandPool = pool;
			ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			ca.commandBufferCount = 1;
			VKCHECK(vkAllocateCommandBuffers(device, &ca, &s.cmd));
			VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VKCHECK(vkCreateFence(device, &f, nullptr, &s.fence));
			VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			VKCHECK(vkCreateSemaphore(device, &se, nullptr, &s.acquired));
			if (o.mode == "wsi-direct")
				continue;
			VkImportMetalIOSurfaceInfoEXT io{VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT};
			if (o.mode != "wsi-direct") {
				NSDictionary *props = @{
					(id)kIOSurfaceWidth : @(extent.width),
					(id)kIOSurfaceHeight : @(extent.height),
					(id)kIOSurfaceBytesPerElement : @4,
					(id)kIOSurfacePixelFormat : @(0x42475241),
				};
				s.surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
				if (!s.surface)
					throw std::runtime_error("IOSurfaceCreate failed");
				io.ioSurface = s.surface;
			}
			VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			c.pNext = &io;
			c.imageType = VK_IMAGE_TYPE_2D;
			c.format = VK_FORMAT_B8G8R8A8_UNORM;
			c.extent = {extent.width, extent.height, 1};
			c.mipLevels = c.arrayLayers = 1;
			c.samples = VK_SAMPLE_COUNT_1_BIT;
			c.tiling = VK_IMAGE_TILING_OPTIMAL;
			c.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			VKCHECK(vkCreateImage(device, &c, nullptr, &s.image));
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(device, s.image, &req);
			uint32_t index = UINT32_MAX;
			for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
				if ((req.memoryTypeBits & (1u << i)) &&
				    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
					index = i;
					break;
				}
			if (index == UINT32_MAX)
				throw std::runtime_error("No device local memory");
			VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
			dedicated.image = s.image;
			VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = index;
			VKCHECK(vkAllocateMemory(device, &alloc, nullptr, &s.memory));
			VKCHECK(vkBindImageMemory(device, s.image, s.memory, 0));
			s.view = make_view(s.image);
			s.fb = make_fb(s.view);
			if (native()) {
				MTLTextureDescriptor *td =
				    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				                                                       width:extent.width
				                                                      height:extent.height
				                                                   mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				td.storageMode = MTLStorageModeShared;
				s.texture = [layer.device newTextureWithDescriptor:td iosurface:s.surface plane:0];
				if (!s.texture)
					throw std::runtime_error("IOSurface Metal texture creation failed");
			}
		}
	}

	void
	harvest(Slot &s, uint32_t index)
	{
		VKCHECK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, 5000000000ull));
		if (s.row >= 0) {
			uint64_t q[3];
			VKCHECK(vkGetQueryPoolResults(device, queries, index * 3, 3, sizeof(q), q, sizeof(uint64_t),
			                              VK_QUERY_RESULT_64_BIT));
			uint64_t mask = timestamp_bits == 64 ? UINT64_MAX : ((1ull << timestamp_bits) - 1);
			rows[s.row].gpu_ms = double((q[1] - q[0]) & mask) * properties.limits.timestampPeriod / 1e6;
			rows[s.row].copy_ms = double((q[2] - q[1]) & mask) * properties.limits.timestampPeriod / 1e6;
		}
	}

	void
	record(uint32_t index, uint32_t image_index, uint32_t id)
	{
		Slot &s = slots[index];
		VKCHECK(vkResetCommandBuffer(s.cmd, 0));
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VKCHECK(vkBeginCommandBuffer(s.cmd, &begin));
		vkCmdResetQueryPool(s.cmd, queries, index * 3, 3);
		vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, index * 3);
		VkImage target_image = o.mode == "wsi-direct" ? images[image_index] : s.image;
		VkFramebuffer fb = o.mode == "wsi-direct" ? framebuffers[image_index] : s.fb;
		barrier(s.cmd, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
		        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rp.renderPass = pass;
		rp.framebuffer = fb;
		rp.renderArea = {{0, 0}, extent};
		vkCmdBeginRenderPass(s.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		uint32_t params[] = {id, o.burn, extent.width, extent.height};
		vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
		vkCmdDraw(s.cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(s.cmd);
		vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, index * 3 + 1);
		if (o.mode == "wsi-copy") {
			barrier(s.cmd, s.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			        VK_ACCESS_TRANSFER_READ_BIT);
			barrier(s.cmd, images[image_index], VK_IMAGE_LAYOUT_UNDEFINED,
			        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
			VkImageCopy copy{};
			copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.extent = {extent.width, extent.height, 1};
			vkCmdCopyImage(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[image_index],
			               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			barrier(s.cmd, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
		} else {
			barrier(s.cmd, target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			        native() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, native() ? VK_ACCESS_MEMORY_READ_BIT : 0);
		}
		vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, index * 3 + 2);
		VKCHECK(vkEndCommandBuffer(s.cmd));
	}

	void
	feedback()
	{
		if (!past || !swapchain || native())
			return;
		uint32_t n = 0;
		VKCHECK(past(device, swapchain, &n, nullptr));
		if (!n)
			return;
		std::vector<VkPastPresentationTimingGOOGLE> timings(n);
		VkResult result = past(device, swapchain, &n, timings.data());
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
			check(result, "presentation feedback");
		for (uint32_t i = 0; i < n; ++i) {
			auto &t = timings[i];
			if (t.presentID && t.presentID <= rows.size()) {
				Row &r = rows[t.presentID - 1];
				r.presented = double(t.actualPresentTime) / 1e9;
				r.feedback_received = now_s();
				r.status = "presented";
			}
		}
	}

	void
	native_presenter()
	{
		try {
			for (;;) {
				@autoreleasepool {
					{
						std::unique_lock<std::mutex> lock(mutex);
						cv.wait(lock, [&] { return pending >= 0 || stopping; });
						if (pending < 0 && stopping)
							break;
					}
					double ab = now_s();
					id<CAMetalDrawable> drawable = [layer nextDrawable];
					double ae = now_s();
					if (!drawable)
						throw std::runtime_error("nextDrawable returned nil");
					int index;
					{
						std::lock_guard<std::mutex> lock(mutex);
						index = pending;
						pending = -1;
					}
					Slot &s = slots[index];
					Row *r = &rows[s.row];
					r->acquire_begin = ab;
					r->acquire_end = ae;
					id<MTLCommandBuffer> cb = [metal_queue commandBuffer];
					if (!cb)
						throw std::runtime_error("Metal command buffer unavailable");
					[cb encodeWaitForEvent:event value:r->id];
					id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
					if (!blit)
						throw std::runtime_error("Metal blit encoder unavailable");
					[blit copyFromTexture:s.texture
					          sourceSlice:0
					          sourceLevel:0
					         sourceOrigin:MTLOriginMake(0, 0, 0)
					           sourceSize:MTLSizeMake(extent.width, extent.height, 1)
					            toTexture:drawable.texture
					     destinationSlice:0
					     destinationLevel:0
					    destinationOrigin:MTLOriginMake(0, 0, 0)];
					[blit endEncoding];
					[drawable addPresentedHandler:^(id<MTLDrawable> d) {
					  std::lock_guard<std::mutex> lock(mutex);
					  r->presented = d.presentedTime;
					  r->feedback_received = now_s();
					  r->status = d.presentedTime > 0 ? "presented" : "no_timestamp";
					}];
					dispatch_group_enter(metal_group);
					[cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
					  std::lock_guard<std::mutex> lock(mutex);
					  r->metal_ms = (completed.GPUEndTime - completed.GPUStartTime) * 1000;
					  if (completed.status == MTLCommandBufferStatusError) {
						  presenter_failed = true;
						  presenter_error = "Metal command buffer failed";
					  }
					  slots[index].busy = false;
					  cv.notify_all();
					  dispatch_group_leave(metal_group);
					}];
					r->present_begin = now_s();
					if (o.policy == "minimum")
						[cb presentDrawable:drawable afterMinimumDuration:o.minimum_ms / 1000];
					else if (o.policy == "timed")
						[cb presentDrawable:drawable atTime:r->target + o.offset_ms / 1000];
					else
						[cb presentDrawable:drawable];
					[cb commit];
					r->present_end = now_s();
				}
			}
		} catch (const std::exception &e) {
			std::lock_guard<std::mutex> lock(mutex);
			presenter_error = e.what();
			presenter_failed = true;
			cv.notify_all();
		}
	}

	void
	run_frames()
	{
		rows.resize(o.frames);
		if (native())
			presenter = std::thread([&] { native_presenter(); });
		uint64_t previous_tick = 0;
		for (uint32_t f = 0; f < o.frames; ++f) {
			@autoreleasepool {
				Row &r = rows[f];
				r.id = f + 1;
				uint32_t index = f % 3;
				{
					std::unique_lock<std::mutex> lock(mutex);
					if (!cv.wait_for(lock, std::chrono::seconds(5),
					                 [&] { return tick > previous_tick || presenter_failed; }))
						throw std::runtime_error("Display link timeout");
					previous_tick = tick;
					r.tick = tick;
					r.target = target;
					r.generated = now_s();
					if (native()) {
						if (!cv.wait_for(lock, std::chrono::seconds(5), [&] {
							    return presenter_failed ||
							           std::any_of(std::begin(slots), std::end(slots),
							                       [](auto &s) { return !s.busy; });
						    }))
							throw std::runtime_error("Native slot timeout");
						for (index = 0; index < 3 && slots[index].busy; ++index) {
						}
					}
					if (presenter_failed)
						throw std::runtime_error(presenter_error);
					slots[index].busy = true;
				}
				Slot &s = slots[index];
				double reuse_begin = now_s();
				harvest(s, index);
				r.reuse_ms = (now_s() - reuse_begin) * 1000;
				s.row = int(f);
				uint32_t gpu_inflight = 1;
				for (uint32_t i = 0; i < 3; ++i)
					if (i != index && vkGetFenceStatus(device, slots[i].fence) == VK_NOT_READY)
						++gpu_inflight;
				r.depth = gpu_inflight;
				if (o.delay_every && r.id % o.delay_every == 0)
					std::this_thread::sleep_for(
					    std::chrono::duration<double, std::milli>(o.delay_ms));
				uint32_t image_index = 0;
				if (!native()) {
					r.acquire_begin = now_s();
					VKCHECK(vkAcquireNextImageKHR(device, swapchain, 5000000000ull, s.acquired,
					                              VK_NULL_HANDLE, &image_index));
					r.acquire_end = now_s();
				}
				record(index, image_index, r.id);
				VKCHECK(vkResetFences(device, 1, &s.fence));
				VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				uint64_t value = r.id;
				VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
				ti.signalSemaphoreValueCount = 1;
				ti.pSignalSemaphoreValues = &value;
				VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
				submit.commandBufferCount = 1;
				submit.pCommandBuffers = &s.cmd;
				submit.signalSemaphoreCount = 1;
				submit.pSignalSemaphores = native() ? &timeline : &finished[image_index];
				if (native())
					submit.pNext = &ti;
				else {
					submit.waitSemaphoreCount = 1;
					submit.pWaitSemaphores = &s.acquired;
					submit.pWaitDstStageMask = &wait_stage;
				}
				r.submit_begin = now_s();
				VKCHECK(vkQueueSubmit(queue, 1, &submit, s.fence));
				r.submit_end = now_s();
				{
					std::lock_guard<std::mutex> lock(mutex);
					r.status = "submitted";
					if (native()) {
						if (pending >= 0) {
							rows[slots[pending].row].status = "superseded";
							slots[pending].busy = false;
						}
						pending = int(index);
						cv.notify_all();
					}
				}
				if (!native()) {
					VkPresentTimeGOOGLE time{
					    r.id,
					    o.policy == "timed" ? uint64_t((r.target + o.offset_ms / 1000) * 1e9) : 0};
					VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
					times.swapchainCount = 1;
					times.pTimes = &time;
					VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
					present.pNext = past ? &times : nullptr;
					present.waitSemaphoreCount = 1;
					present.pWaitSemaphores = &finished[image_index];
					present.swapchainCount = 1;
					present.pSwapchains = &swapchain;
					present.pImageIndices = &image_index;
					r.present_begin = now_s();
					VKCHECK(vkQueuePresentKHR(queue, &present));
					r.present_end = now_s();
					feedback();
				}
			}
		}
	}

	void
	drain()
	{
		if (link) {
			CVDisplayLinkStop(link);
			CVDisplayLinkRelease(link);
			link = nullptr;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			cv.notify_all();
		}
		if (presenter.joinable())
			presenter.join();
		dispatch_group_wait(metal_group, DISPATCH_TIME_FOREVER);
		if (device) {
			VKCHECK(vkDeviceWaitIdle(device));
			for (uint32_t i = 0; i < 3; ++i)
				if (slots[i].fence && slots[i].row >= 0)
					harvest(slots[i], i);
		}
		// Completion is not presentation. Give late feedback a bounded grace period.
		for (int i = 0; i < 40; ++i) {
			feedback();
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
	}

	void
	write_csv()
	{
		std::lock_guard<std::mutex> lock(mutex);
		FILE *file = std::fopen(o.trace.c_str(), "w");
		if (!file)
			throw std::runtime_error("Cannot open CSV");
		std::fprintf(file,
		             "frame_id,tick_id,status,generated_s,target_s,acquire_begin_s,acquire_end_s,reuse_wait_ms,"
		             "submit_begin_s,submit_end_s,present_begin_s,present_end_s,presented_s,feedback_received_"
		             "s,render_gpu_ms,post_render_gpu_ms,metal_command_gpu_ms,gpu_inflight_at_record\n");
		for (auto &r : rows) {
			if (!r.id)
				continue;
			std::fprintf(
			    file,
			    "%u,%llu,%s,%.9f,%.9f,%.9f,%.9f,%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%u\n",
			    r.id, (unsigned long long)r.tick, r.status.c_str(), r.generated, r.target, r.acquire_begin,
			    r.acquire_end, r.reuse_ms, r.submit_begin, r.submit_end, r.present_begin, r.present_end,
			    r.presented, r.feedback_received, r.gpu_ms, r.copy_ms, r.metal_ms, r.depth);
		}
		bool failed = std::ferror(file);
		failed |= std::fclose(file) != 0;
		if (failed)
			throw std::runtime_error("CSV write failed");
	}

	void
	release_graphics()
	{
		// Normal and failed runs drain before destruction; callbacks retain no Vulkan handles.
		if (device) {
			for (auto fb : framebuffers)
				vkDestroyFramebuffer(device, fb, nullptr);
			for (auto view : views)
				vkDestroyImageView(device, view, nullptr);
			for (auto sem : finished)
				vkDestroySemaphore(device, sem, nullptr);
			for (auto &s : slots) {
				if (s.fb)
					vkDestroyFramebuffer(device, s.fb, nullptr);
				if (s.view)
					vkDestroyImageView(device, s.view, nullptr);
				if (s.image)
					vkDestroyImage(device, s.image, nullptr);
				if (s.memory)
					vkFreeMemory(device, s.memory, nullptr);
				if (s.fence)
					vkDestroyFence(device, s.fence, nullptr);
				if (s.acquired)
					vkDestroySemaphore(device, s.acquired, nullptr);
				s.texture = nil;
				if (s.surface)
					CFRelease(s.surface);
			}
			if (swapchain)
				vkDestroySwapchainKHR(device, swapchain, nullptr);
			if (pipeline)
				vkDestroyPipeline(device, pipeline, nullptr);
			if (layout)
				vkDestroyPipelineLayout(device, layout, nullptr);
			if (pass)
				vkDestroyRenderPass(device, pass, nullptr);
			if (timeline)
				vkDestroySemaphore(device, timeline, nullptr);
			if (queries)
				vkDestroyQueryPool(device, queries, nullptr);
			if (pool)
				vkDestroyCommandPool(device, pool, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (surface)
			vkDestroySurfaceKHR(instance, surface, nullptr);
		if (instance)
			vkDestroyInstance(instance, nullptr);
	}
};

int
main(int argc, char **argv)
{
	@autoreleasepool {
		try {
			Options o = parse(argc, argv);
			[NSApplication sharedApplication];
			if (o.list) {
				for (NSScreen *screen in NSScreen.screens)
					std::printf("%s\n", screen.localizedName.UTF8String);
				return 0;
			}
			NSScreen *selected = nil;
			for (NSScreen *screen in NSScreen.screens)
				if (o.display == screen.localizedName.UTF8String)
					selected = screen;
			if (!selected)
				throw std::runtime_error("Selected display not found; use --list-displays");
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
			NSRect rect = selected.frame;
			if (o.windowed) {
				rect.size = NSMakeSize(960, 540);
				rect.origin.x += 40;
				rect.origin.y += 40;
			}
			NSWindow *window = [[NSWindow alloc] initWithContentRect:rect
			                                               styleMask:NSWindowStyleMaskBorderless
			                                                 backing:NSBackingStoreBuffered
			                                                   defer:NO
			                                                  screen:selected];
			window.releasedWhenClosed = NO;
			CAMetalLayer *layer = [CAMetalLayer layer];
			layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			layer.framebufferOnly = NO;
			layer.maximumDrawableCount = 3;
			layer.displaySyncEnabled = YES;
			layer.allowsNextDrawableTimeout = YES;
			layer.opaque = YES;
			CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
			layer.colorspace = colorspace;
			CGColorSpaceRelease(colorspace);
			window.contentView.wantsLayer = YES;
			window.contentView.layer = layer;
			NSRect pixels = [window.contentView convertRectToBacking:window.contentView.bounds];
			layer.contentsScale = selected.backingScaleFactor;
			layer.drawableSize = pixels.size;
			[window orderFront:nil];
			[NSApp finishLaunching];
			// Keep the experiment alive until all presentation callbacks are finished.
			// Deliberately process-lifetime allocation: Apple may deliver callbacks after the drain grace
			// period.
			Probe *p = new Probe;
			p->o = o;
			p->layer = layer;
			p->extent = {uint32_t(pixels.size.width), uint32_t(pixels.size.height)};
			std::atomic<bool> done{false};
			int result = 0;
			std::thread worker([&] {
				@autoreleasepool {
					try {
						p->init_vulkan();
						p->init_rendering();
						p->init_images();
						p->meta
						    << "mode=" << o.mode << "\npolicy=" << o.policy
						    << "\nwidth=" << p->extent.width << "\nheight=" << p->extent.height
						    << "\nburn=" << o.burn << "\ndisplay=" << o.display
						    << "\nminimum_ms=" << o.minimum_ms << "\noffset_ms=" << o.offset_ms
						    << "\ndelay_ms=" << o.delay_ms << "\ndelay_every=" << o.delay_every
						    << "\nvalidation=" << o.validation << "\nos="
						    << NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String
						    << "\npresentation_clock=CACurrentMediaTime (MoltenVK GOOGLE "
						       "timing assumption; verify for installed version)\n";
						CGDirectDisplayID display =
						    [selected.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
						CVReturn ret = CVDisplayLinkCreateWithCGDisplay(display, &p->link);
						if (ret != kCVReturnSuccess)
							throw std::runtime_error("CVDisplayLinkCreate failed");
						CVTime period =
						    CVDisplayLinkGetNominalOutputVideoRefreshPeriod(p->link);
						p->meta
						    << "nominal_refresh_hz="
						    << (period.timeValue ? double(period.timeScale) / period.timeValue
						                         : 0)
						    << '\n';
						if (CVDisplayLinkSetOutputCallback(p->link, Probe::callback, p) !=
						        kCVReturnSuccess ||
						    CVDisplayLinkStart(p->link) != kCVReturnSuccess)
							throw std::runtime_error("Display link start failed");
						p->run_frames();
						p->drain();
						if (p->presenter_failed)
							throw std::runtime_error(p->presenter_error);
						p->meta << "result=complete\n";
					} catch (const std::exception &e) {
						std::fprintf(stderr, "Experiment failed: %s\n", e.what());
						result = 1;
						p->meta << "result=failed\nerror=" << e.what() << '\n';
						try {
							p->drain();
						} catch (const std::exception &d) {
							std::fprintf(stderr, "Drain: %s\n", d.what());
						}
					}
					try {
						p->write_csv();
					} catch (const std::exception &e) {
						std::fprintf(stderr, "%s\n", e.what());
						result = 1;
					}
					p->release_graphics();
					p->meta.flush();
					done = true;
				}
			});
			while (!done) {
				NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
				                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
				                                   inMode:NSDefaultRunLoopMode
				                                  dequeue:YES];
				if (e)
					[NSApp sendEvent:e];
			}
			worker.join();
			[window close];
			std::printf("%s: %s\n", result ? "Failed capture" : "Capture written", o.trace.c_str());
			return result;
		} catch (const std::exception &e) {
			std::fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}
}
