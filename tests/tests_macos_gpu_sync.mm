// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Native Vulkan consumer with opt-in Windows D3D11/DXMT producer.
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include "macos_gpu_sync_common.h"
static void
vkcheck_at(VkResult result, int line)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error("Vulkan result " + std::to_string(result) + " at line " +
		                         std::to_string(line));
}
#define vkcheck(result) vkcheck_at(result, __LINE__)

static int
producer(const char *service_name, const char *socket_path)

{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	check(device != nil, "no Metal device");
	id<MTLCommandQueue> queues[2] = {[device newCommandQueue], [device newCommandQueue]};
	id<MTLSharedEvent> event = [device newSharedEvent];
	id<MTLSharedEvent> join = [device newSharedEvent];
	GPUSyncEventService *service = [GPUSyncEventService new];
	service.event = event;
	service.gate = [device newSharedEvent];
	NSXPCListener *listener =
	    [[NSXPCListener alloc] initWithMachServiceName:[NSString stringWithUTF8String:service_name]];
	listener.delegate = service;
	[listener resume];
	int channel = socket(AF_UNIX, SOCK_STREAM, 0);
	check(channel >= 0, "producer socket");
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
	check(connect(channel, (sockaddr *)&addr, sizeof(addr)) == 0, "producer connect");
	dup2(channel, 0);
	dup2(channel, 1);
	close(channel);
	pid_t ownpid = getpid();
	io(1, &ownpid, sizeof(ownpid), true);
	IOSurfaceRef surfaces[N];
	id<MTLTexture> textures[N];
	id<MTLBuffer> uploads[N]{};
	const bool upload_pattern =
	    getenv("GPU_SYNC_PRODUCER_UPLOAD") != nullptr && strcmp(getenv("GPU_SYNC_PRODUCER_UPLOAD"), "1") == 0;
	NSMutableArray *ids = [NSMutableArray array];
	for (unsigned i = 0; i < N; ++i) {
		surfaces[i] = surface();
		check(surfaces[i] != nullptr, "IOSurface creation");
		MTLTextureDescriptor *desc =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                       width:W
		                                                      height:W
		                                                   mipmapped:NO];
		desc.storageMode = MTLStorageModeShared;
		desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
		textures[i] = [device newTextureWithDescriptor:desc iosurface:surfaces[i] plane:0];
		check(textures[i] != nil, "Metal IOSurface texture");
		if (upload_pattern) {
			uploads[i] = [device newBufferWithLength:BYTES options:MTLResourceStorageModeShared];
			check(uploads[i] != nil, "native pattern upload buffer");
		}
		[ids addObject:@(IOSurfaceGetID(surfaces[i]))];
	}
	NSError *error = nil;
	NSString *shader =
	    @"#include <metal_stdlib>\nusing namespace metal;\n"
	     "kernel void fill(texture2d<float,access::write> image [[texture(0)]], constant uint4 &ids [[buffer(0)]], "
	     "uint2 p [[thread_position_in_grid]]) {"
	     "uint word=ids[(p.x/4)%4]; uint byte=(word>>((p.x%4)*8))&255;"
	     "image.write(float4(float(byte)/255.0,float((p.x+p.y)%251)/255.0,float(p.y%251)/255.0,1.0),p);}";
	id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
	check(library != nil, "producer compute shader");
	id<MTLFunction> function = [library newFunctionWithName:@"fill"];
	id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
	check(pipeline != nil, "producer compute pipeline");
	[function release];
	[library release];
	NSData *archive = [NSKeyedArchiver archivedDataWithRootObject:@{
		@"surfaces" : ids,
		@"registry" : @(device.registryID)
	}
	                                        requiringSecureCoding:YES
	                                                        error:&error];
	if (!archive)
		fprintf(stderr, "archive: %s\n", error.description.UTF8String);
	check(archive != nil, "shared-event archive");
	uint32_t len = (uint32_t)archive.length;
	io(1, &len, sizeof(len), true);
	io(1, (void *)archive.bytes, len, true);
	uint64_t previous = 0;
	for (;;) {
		Command c{};
		io(0, &c, sizeof(c), false);
		if (c.mode == 99)
			break;
		check(c.image < N, "producer image index");
		if (c.mode == 4)
			_exit(0); // exit with an outstanding, unsignalled obligation
		if (c.mode == 6) {
			check(c.value <= previous, "invalid-sequence fixture");
			Receipt r{};
			io(1, &r, sizeof(r), true);
			continue;
		}
		auto write_texture = [&](id<MTLCommandQueue> q) {
			id<MTLCommandBuffer> cb = [q commandBuffer];
			if (c.mode == 9)
				[cb encodeWaitForEvent:service.gate value:c.value];
			if (upload_pattern) {
				for (unsigned y = 0; y < W; ++y)
					for (unsigned x = 0; x < W; ++x)
						pattern(c, x, y,
						        (uint8_t *)uploads[c.image].contents + (y * W + x) * 4);
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit copyFromBuffer:uploads[c.image]
				           sourceOffset:0
				      sourceBytesPerRow:W * 4
				    sourceBytesPerImage:BYTES
				             sourceSize:MTLSizeMake(W, W, 1)
				              toTexture:textures[c.image]
				       destinationSlice:0
				       destinationLevel:0
				      destinationOrigin:MTLOriginMake(0, 0, 0)];
				[blit endEncoding];
				return cb;
			}
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			uint32_t words[4] = {(uint32_t)c.frame, (uint32_t)(c.frame >> 32), c.producer, c.image};
			[enc setComputePipelineState:pipeline];
			[enc setTexture:textures[c.image] atIndex:0];
			[enc setBytes:words length:sizeof(words) atIndex:0];
			[enc dispatchThreads:MTLSizeMake(W, W, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
			[enc endEncoding];
			return cb;
		};
		if (c.mode ==
		    7) { // broken multi-queue: signal now, deliberately write only after consumer acknowledgement
			id<MTLCommandBuffer> cb = [queues[1] commandBuffer];
			[cb encodeSignalEvent:event value:c.value];
			Receipt r{ns(), 0};
			[cb commit];
			r.gpu_status = (uint64_t)cb.status;
			io(1, &r, sizeof(r), true);
			char ack;
			io(0, &ack, 1, false);
			cb = write_texture(queues[0]);
			[cb commit];
			metal_complete(cb);
		} else {
			if (c.delay_ms)
				std::this_thread::sleep_for(std::chrono::milliseconds(c.delay_ms));
			id<MTLCommandBuffer> cb = write_texture(queues[0]);
			if (c.mode == 8) {
				[cb encodeSignalEvent:join value:c.value];
				[cb commit];
				cb = [queues[1] commandBuffer];
				[cb encodeWaitForEvent:join value:c.value];
			}
			if (c.mode != 2)
				[cb encodeSignalEvent:event value:c.value];
			Receipt r{ns(), 0};
			[cb commit];
			r.gpu_status = (uint64_t)cb.status;
			io(1, &r, sizeof(r), true);
			char ack;
			io(0, &ack, 1, false);
			// Finite normal producer work only. Never encode an unresolved GPU dependency.
			metal_complete(cb);
			check(cb.status == MTLCommandBufferStatusCompleted, "Metal command buffer failed");
		}
		previous = c.value;
	}
	for (unsigned i = 0; i < N; ++i) {
		[textures[i] release];
		[uploads[i] release];
		CFRelease(surfaces[i]);
	}
	[pipeline release];
	[event release];
	[join release];
	[queues[0] release];
	[queues[1] release];
	[device release];
	return 0;
}
struct Child
{
	pid_t pid = -1;
	int input = -1, output = -1, server = -1;
	NSXPCConnection *connection = nil;
	std::string directory, plist, domain, name;
	Child(const char *path)
	{
		try {
			char dir[] = "/tmp/monado-gpu-sync-XXXXXX";
			check(mkdtemp(dir) != nullptr, "temporary directory");
			directory = dir;
			std::string sock = directory + "/socket";
			plist = directory + "/agent.plist";
			domain = "gui/" + std::to_string(getuid());
			static unsigned sequence = 0;
			name = "org.freedesktop.monado.gpu-sync." + std::to_string(getpid()) + "." +
			       std::to_string(sequence++);
			server = socket(AF_UNIX, SOCK_STREAM, 0);
			check(server >= 0, "consumer socket");
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			strlcpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path));
			check(bind(server, (sockaddr *)&addr, sizeof(addr)) == 0 && listen(server, 1) == 0,
			      "consumer listen");
			char absolute[4096];
			const char *wine_launcher = getenv("GPU_SYNC_WINE_LAUNCHER");
			check(realpath(wine_launcher ? wine_launcher : path, absolute) != nullptr, "executable path");
			NSString *label = [NSString stringWithUTF8String:name.c_str()];
			NSDictionary *agent = @{
				@"Label" : label,
				@"ProgramArguments" : @[
					[NSString stringWithUTF8String:absolute], @"--producer", label,
					[NSString stringWithUTF8String:sock.c_str()]
				],
				@"MachServices" : @{label : @YES},
				@"EnvironmentVariables" : @{
					@"GPU_SYNC_TEXTURE_SIZE" : [NSString stringWithFormat:@"%u", W],
					@"GPU_SYNC_PRODUCER_UPLOAD" :
					    (getenv("GPU_SYNC_PRODUCER_UPLOAD")
			                         ? [NSString stringWithUTF8String:getenv("GPU_SYNC_PRODUCER_UPLOAD")]
					         : @"")
				}
			};
			NSData *data = [NSPropertyListSerialization dataWithPropertyList:agent
			                                                          format:NSPropertyListXMLFormat_v1_0
			                                                         options:0
			                                                           error:nil];
			check([data writeToFile:[NSString stringWithUTF8String:plist.c_str()] atomically:YES],
			      "launchd plist");
			check(launchctl("bootstrap", domain.c_str(), plist.c_str()) == 0, "launchd bootstrap");
			connection = [[NSXPCConnection alloc] initWithMachServiceName:label options:0];
			connection.remoteObjectInterface =
			    [NSXPCInterface interfaceWithProtocol:@protocol(GPUSyncEventProtocol)];
			[connection resume];
			// Trigger service activation; handle retrieval below performs the actual XPC transfer.
			[[connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			  (void)error;
			}] eventHandle:^(MTLSharedEventHandle *handle) {
			  (void)handle;
			}];
			pollfd p{server, POLLIN, 0};
			check(poll(&p, 1, wine_launcher ? 60000 : 5000) > 0, "producer activation timeout");
			input = accept(server, nullptr, nullptr);
			output = input;
			close(server);
			server = -1;
			check(input >= 0, "accept producer");
			io(output, &pid, sizeof(pid), false);
		} catch (...) {
			cleanup();
			throw;
		}
	}
	MTLSharedEventHandle *
	handle(bool gate = false)
	{
		__block MTLSharedEventHandle *result = nil;
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		id<GPUSyncEventProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		  (void)error;
		  dispatch_semaphore_signal(done);
		}];
		void (^reply)(MTLSharedEventHandle *) = ^(MTLSharedEventHandle *h) {
		  result = [h retain];
		  dispatch_semaphore_signal(done);
		};
		if (gate)
			[proxy gateHandle:reply];
		else
			[proxy eventHandle:reply];
		check(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0 &&
		          result != nil,
		      "XPC event timeout");
		return [result autorelease];
	}
	~Child()
	{
		cleanup();
	}
	void
	cleanup()
	{
		if (server >= 0)
			close(server);
		[connection invalidate];
		[connection release];
		if (input >= 0)
			close(input);
		if (!domain.empty() && !plist.empty())
			launchctl("bootout", domain.c_str(), plist.c_str());
		unlink((directory + "/socket").c_str());
		unlink(plist.c_str());
		rmdir(directory.c_str());
	}
	void
	ack()
	{
		char a = 1;
		io(input, &a, 1, true);
	}
};
struct Consumer
{
	VkInstance instance{};
	VkPhysicalDevice physical{};
	VkDevice device{};
	VkQueue queue{};
	uint32_t family = 0;
	VkImage images[N]{};
	VkDeviceMemory memory[N]{};
	IOSurfaceRef surfaces[N]{};
	VkBuffer buffer{};
	VkDeviceMemory readback{};
	void *mapped{};
	VkCommandPool pool{};
	VkCommandBuffer cmd{};
	VkFence fence{};
	VkSemaphore semaphore{};
	id<MTLSharedEvent> event = nil;
	id<MTLSharedEvent> producer_gate = nil;
	VkSemaphore consumer_gate{};
	std::thread release_gate;
	bool unresolved_producer = false;
	uint64_t registry = 0;
	uint32_t
	memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
	{
		VkPhysicalDeviceMemoryProperties props{};
		vkGetPhysicalDeviceMemoryProperties(physical, &props);
		for (unsigned i = 0; i < props.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
				return i;
		throw std::runtime_error("no suitable Vulkan memory");
	}
	Consumer(Child &child)
	{
		VkApplicationInfo app{};
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.pApplicationName = "Monado GPU sync probe";
		app.apiVersion = VK_API_VERSION_1_2;
		const char *ie[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
		VkInstanceCreateInfo ic{};
		ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		ic.pApplicationInfo = &app;
		ic.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		ic.enabledExtensionCount = 1;
		ic.ppEnabledExtensionNames = ie;
		vkcheck(vkCreateInstance(&ic, nullptr, &instance));
		uint32_t count = 1;
		vkcheck(vkEnumeratePhysicalDevices(instance, &count, &physical));
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> props(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props.data());
		while (family < count && !(props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			++family;
		check(family < count, "graphics queue");
		float priority = 1;
		VkDeviceQueueCreateInfo qc{};
		qc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qc.queueFamilyIndex = family;
		qc.queueCount = 1;
		qc.pQueuePriorities = &priority;
		const char *de[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME, "VK_KHR_portability_subset"};
		VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
		timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
		timeline.timelineSemaphore = VK_TRUE;
		VkExportMetalObjectCreateInfoEXT exportDevice{};
		exportDevice.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT;
		exportDevice.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT;
		exportDevice.pNext = &timeline;
		VkDeviceCreateInfo dc{};
		dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dc.pNext = &exportDevice;
		dc.queueCreateInfoCount = 1;
		dc.pQueueCreateInfos = &qc;
		dc.enabledExtensionCount = 2;
		dc.ppEnabledExtensionNames = de;
		vkcheck(vkCreateDevice(physical, &dc, nullptr, &device));
		vkGetDeviceQueue(device, family, 0, &queue);
		VkExportMetalDeviceInfoEXT md{};
		md.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
		VkExportMetalObjectsInfoEXT ex{};
		ex.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT;
		ex.pNext = &md;
		auto exporter = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT");
		check(exporter != nullptr, "Metal export entry point");
		exporter(device, &ex);
		id<MTLDevice> metal = md.mtlDevice;
		check(metal != nil, "Vulkan Metal device");
		registry = metal.registryID;
		uint32_t length = 0;
		io(child.output, &length, 4, false);
		check(length < 1048576, "archive bound");
		NSMutableData *data = [NSMutableData dataWithLength:length];
		io(child.output, data.mutableBytes, length, false);
		NSError *error = nil;
		NSSet *classes = [NSSet setWithObjects:[NSDictionary class], [NSArray class], [NSNumber class],
		                                       [NSString class], [MTLSharedEventHandle class], nil];
		NSDictionary *d = [NSKeyedUnarchiver unarchivedObjectOfClasses:classes fromData:data error:&error];
		check(d != nil, "shared-event decode");
		check([d[@"registry"] unsignedLongLongValue] == registry, "Metal device mismatch");
		event = [metal newSharedEventWithHandle:child.handle()];
		producer_gate = [metal newSharedEventWithHandle:child.handle(true)];
		check(event != nil, "shared-event import");
		VkSemaphoreTypeCreateInfo st{};
		st.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		st.initialValue = 0;
		VkImportMetalSharedEventInfoEXT si{};
		si.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT;
		si.mtlSharedEvent = event;
		si.pNext = &st;
		VkSemaphoreCreateInfo sc{};
		sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		sc.pNext = &si;
		vkcheck(vkCreateSemaphore(device, &sc, nullptr, &semaphore));
		sc.pNext = &st;
		vkcheck(vkCreateSemaphore(device, &sc, nullptr, &consumer_gate));
		for (unsigned i = 0; i < N; ++i) {
			surfaces[i] = IOSurfaceLookup([d[@"surfaces"][i] unsignedIntValue]);
			check(surfaces[i] != nullptr, "IOSurface lookup");
			VkImportMetalIOSurfaceInfoEXT surf{};
			surf.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT;
			surf.ioSurface = surfaces[i];
			VkImageCreateInfo im{};
			im.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			im.pNext = &surf;
			im.imageType = VK_IMAGE_TYPE_2D;
			im.format = VK_FORMAT_B8G8R8A8_UNORM;
			im.extent = {W, W, 1};
			im.mipLevels = 1;
			im.arrayLayers = 1;
			im.samples = VK_SAMPLE_COUNT_1_BIT;
			im.tiling = VK_IMAGE_TILING_OPTIMAL;
			im.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			vkcheck(vkCreateImage(device, &im, nullptr, &images[i]));
			VkMemoryRequirements req{};
			vkGetImageMemoryRequirements(device, images[i], &req);
			VkMemoryDedicatedAllocateInfo dedicated{};
			dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
			dedicated.image = images[i];
			VkMemoryAllocateInfo alloc{};
			alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc.pNext = &dedicated;
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
			vkcheck(vkAllocateMemory(device, &alloc, nullptr, &memory[i]));
			vkcheck(vkBindImageMemory(device, images[i], memory[i], 0));
		}
		VkBufferCreateInfo bc{};
		bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bc.size = BYTES;
		bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		vkcheck(vkCreateBuffer(device, &bc, nullptr, &buffer));
		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(device, buffer, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		vkcheck(vkAllocateMemory(device, &alloc, nullptr, &readback));
		vkcheck(vkBindBufferMemory(device, buffer, readback, 0));
		vkcheck(vkMapMemory(device, readback, 0, BYTES, 0, &mapped));
		VkCommandPoolCreateInfo pc{};
		pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pc.queueFamilyIndex = family;
		vkcheck(vkCreateCommandPool(device, &pc, nullptr, &pool));
		VkCommandBufferAllocateInfo ca{};
		ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		ca.commandPool = pool;
		ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ca.commandBufferCount = 1;
		vkcheck(vkAllocateCommandBuffers(device, &ca, &cmd));
		VkFenceCreateInfo fc{};
		fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkcheck(vkCreateFence(device, &fc, nullptr, &fence));
		VkCommandBufferBeginInfo init{};
		init.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		vkcheck(vkBeginCommandBuffer(cmd, &init));
		for (unsigned i = 0; i < N; ++i) {
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.image = images[i];
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		vkcheck(vkEndCommandBuffer(cmd));
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		vkcheck(vkQueueSubmit(queue, 1, &submit, fence));
		vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000));
	}
	bool pending = false;
	uint64_t
	submit(const Command &c, bool wait = true)
	{
		check(!pending, "premature consumer reuse");
		check(event.signaledValue >= c.value, "unsafe future dependency");
		vkcheck(vkResetCommandBuffer(cmd, 0));
		vkcheck(vkResetFences(device, 1, &fence));
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		vkcheck(vkBeginCommandBuffer(cmd, &begin));
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		// Imported external content is preserved by the first transition from GENERAL.
		barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = images[c.image];
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {W, W, 1};
		unsigned copies = getenv("GPU_SYNC_COPY_REPEATS") ? std::stoul(getenv("GPU_SYNC_COPY_REPEATS")) : 1;
		check(copies >= 1 && copies <= 1024, "copy repeat bound");
		for (unsigned i = 0; i < copies; i++) {
			vkCmdCopyImageToBuffer(cmd, images[c.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
			                       &copy);
			VkMemoryBarrier order{};
			order.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			order.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			order.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			if (i + 1 < copies)
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &order, 0, nullptr, 0,
				                     nullptr);
		}
		std::swap(barrier.oldLayout, barrier.newLayout);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &barrier);
		VkMemoryBarrier host{};
		host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
		                     nullptr, 0, nullptr);
		vkcheck(vkEndCommandBuffer(cmd));
		VkTimelineSemaphoreSubmitInfo ts{};
		ts.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		uint64_t wait_values[] = {c.value, c.value};
		VkSemaphore wait_sems[] = {semaphore, consumer_gate};
		VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
		ts.waitSemaphoreValueCount = c.mode == 10 ? 2 : 1;
		ts.pWaitSemaphoreValues = wait_values;
		VkSubmitInfo s{};
		s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		s.pNext = &ts;
		s.waitSemaphoreCount = c.mode == 10 ? 2 : 1;
		s.pWaitSemaphores = wait_sems;
		s.pWaitDstStageMask = stages;
		s.commandBufferCount = 1;
		s.pCommandBuffers = &cmd;
		uint64_t start = ns();
		if (c.mode == 10) {
			if (release_gate.joinable())
				release_gate.join();
			release_gate = std::thread([this, value = c.value] {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				VkSemaphoreSignalInfo signal{};
				signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
				signal.semaphore = consumer_gate;
				signal.value = value;
				vkSignalSemaphore(device, &signal);
			});
		}
		vkcheck(vkQueueSubmit(queue, 1, &s, fence));
		pending = true;
		return ns() - start;
	}
	uint64_t
	overhead(const Command &c, bool wait)
	{
		// Identical empty submissions isolate the satisfied-wait submission cost.
		// Neither benchmark control accesses any shared texture.
		check(!pending && event.signaledValue >= c.value, "unsafe benchmark dependency");
		vkcheck(vkResetFences(device, 1, &fence));
		VkTimelineSemaphoreSubmitInfo ts{};
		ts.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		ts.waitSemaphoreValueCount = wait ? 1 : 0;
		ts.pWaitSemaphoreValues = &c.value;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		info.pNext = &ts;
		info.waitSemaphoreCount = wait ? 1 : 0;
		info.pWaitSemaphores = &semaphore;
		info.pWaitDstStageMask = &stage;
		uint64_t start = ns();
		vkcheck(vkQueueSubmit(queue, 1, &info, fence));
		pending = true;
		return ns() - start;
	}
	bool
	complete(uint64_t timeout_ns = 1000000000)
	{
		VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns);
		if (r == VK_TIMEOUT)
			return false;
		vkcheck(r);
		pending = false;
		return true;
	}
	bool
	pixels(const Command &c)
	{
		uint8_t p[4];
		for (unsigned y = 0; y < W; y++) {
			for (unsigned x = 0; x < W; x++) {
				pattern(c, x, y, p);
				if (memcmp((char *)mapped + (y * W + x) * 4, p, 4)) {
					return false;
				}
			}
		}
		return true;
	}
	~Consumer()
	{
		if (release_gate.joinable())
			release_gate.join();
		producer_gate.signaledValue = UINT64_MAX; // Release only the trusted test gate.
		if (unresolved_producer)
			return;
		// On an unresolved consumer fence retain the entire Vulkan device/resources until process exit.
		if (pending && !complete(0))
			return;
		if (device) {
			if (mapped)
				vkUnmapMemory(device, readback);
			vkDestroyFence(device, fence, nullptr);
			vkDestroyCommandPool(device, pool, nullptr);
			vkDestroyBuffer(device, buffer, nullptr);
			vkFreeMemory(device, readback, nullptr);
			for (unsigned i = 0; i < N; ++i) {
				vkDestroyImage(device, images[i], nullptr);
				vkFreeMemory(device, memory[i], nullptr);
				if (surfaces[i])
					CFRelease(surfaces[i]);
			}
			vkDestroySemaphore(device, semaphore, nullptr);
			[event release];
			[producer_gate release];
			vkDestroySemaphore(device, consumer_gate, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (instance)
			vkDestroyInstance(instance, nullptr);
	}
};
static int
concurrent_producers(const char *path, unsigned frames)
{
	uint64_t start = ns();
	Child first(path);
	Consumer a(first);
	Child second(path);
	Consumer b(second);
	check(a.registry == b.registry, "concurrent producers must share the same physical device");
	printf(
	    "{\"event\":\"config\",\"scenario\":\"concurrent-producers\",\"producer_pids\":[%d,%d],\"consumer_pid\":%d,"
	    "\"texture_size\":%u,\"thermal\":%ld}\n",
	    first.pid, second.pid, getpid(), W, (long)[NSProcessInfo processInfo].thermalState);
	for (unsigned frame = 1; frame <= frames; frame++) {
		Command ca{frame, frame, (frame - 1) % N, (uint32_t)first.pid, 0, 0};
		Command cb = ca;
		cb.producer = (uint32_t)second.pid;
		io(first.input, &ca, sizeof(ca), true);
		io(second.input, &cb, sizeof(cb), true);
		Receipt ra{}, rb{};
		io(first.output, &ra, sizeof(ra), false);
		io(second.output, &rb, sizeof(rb), false);
		uint64_t until = ns() + 500000000;
		while ((a.event.signaledValue < frame || b.event.signaledValue < frame) && ns() < until)
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		check(a.event.signaledValue >= frame && b.event.signaledValue >= frame, "concurrent readiness");
		a.submit(ca);
		b.submit(cb);
		check(a.complete() && b.complete(), "concurrent GPU completion");
		check(a.pixels(ca) && b.pixels(cb), "cross-producer attribution or overwrite");
		first.ack();
		second.ack();
		printf(
		    "{\"event\":\"concurrent_frame\",\"frame\":%u,\"both_gpu_patterns_valid\":true,\"elapsed_ns\":%llu,"
		    "\"thermal\":%ld}\n",
		    frame, (unsigned long long)(ns() - start), (long)[NSProcessInfo processInfo].thermalState);
	}
	Command stop{};
	stop.mode = 99;
	io(first.input, &stop, sizeof(stop), true);
	io(second.input, &stop, sizeof(stop), true);
	printf(
	    "{\"event\":\"result\",\"scenario\":\"concurrent-producers\",\"status\":\"PASS\",\"consumed\":%u,\"elapsed_"
	    "ns\":%llu,\"thermal_end\":%ld}\n",
	    frames * 2, (unsigned long long)(ns() - start), (long)[NSProcessInfo processInfo].thermalState);
	return 0;
}

static double
percentile(std::vector<uint64_t> v, double p)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1)))] / 1000.;
}
int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	@autoreleasepool {
		try {
			if (argc == 4 && !strcmp(argv[1], "--producer"))
				return producer(argv[2], argv[3]);
			check(W != 0, "texture size must be 16–2048");
			std::string scenario = argc > 1 ? argv[1] : "normal";
			unsigned frames = argc > 2 ? std::stoul(argv[2]) : 12;
			check(frames > 0 && frames <= 1000000, "frame count bound");
			if (scenario == "concurrent-producers")
				return concurrent_producers(argv[0], frames);
			const unsigned hung = getenv("GPU_SYNC_HUNG_MS") ? std::stoul(getenv("GPU_SYNC_HUNG_MS")) : 500;
			const unsigned margin =
			    getenv("GPU_SYNC_MARGIN_US") ? std::stoul(getenv("GPU_SYNC_MARGIN_US")) : 6000;
			const unsigned interval =
			    getenv("GPU_SYNC_INTERVAL_US") ? std::stoul(getenv("GPU_SYNC_INTERVAL_US")) : 8333;
			check(hung > 0 && hung <= 10000 && interval > margin, "timing parameters");
			const std::vector<std::string> scenarios = {
			    "normal",      "delayed",    "missing",         "future",        "nonmonotonic",
			    "exit-before", "exit-after", "consumer-delay",  "multi-queue",   "broken-queue",
			    "reuse",       "idle",       "cancel-callback", "exit-inflight", "consumer-gpu-delay"};
			check(std::find(scenarios.begin(), scenarios.end(), scenario) != scenarios.end(),
			      "unknown scenario");
			Child child(argv[0]);
			Consumer gpu(child);
			struct CallbackState
			{
				std::atomic<bool> canceled{false};
				std::atomic<unsigned> callbacks{0}, activations{0};
			};
			auto callback_state = std::make_shared<CallbackState>();
			MTLSharedEventListener *notification = nil;
			if (scenario == "cancel-callback") {
				notification = [[MTLSharedEventListener alloc]
				    initWithDispatchQueue:dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0)];
				[gpu.event notifyListener:notification
				                  atValue:1
				                    block:^(id<MTLSharedEvent> event, uint64_t value) {
					              (void)event;
					              (void)value;
					              if (!callback_state->canceled.load())
						              callback_state->activations++;
					              callback_state->callbacks++;
				                    }];
				callback_state->canceled = true;
			}
			std::vector<uint64_t> submits, controls, empty_waits, readiness, e2e;
			unsigned consumed = 0, expired = 0, blocked = 0;
			uint64_t start = ns();
			printf(
			    "{\"event\":\"config\",\"scenario\":\"%s\",\"producer_pid\":%d,\"consumer_pid\":%d,\"hung_"
			    "ms\":%u,\"margin_us\":%u,\"interval_us\":%u,\"thermal\":%ld,\"low_power\":%s,\"seed\":0,"
			    "\"registry\":%llu,\"texture_size\":%u}\n",
			    scenario.c_str(), child.pid, getpid(), hung, margin, interval,
			    (long)[NSProcessInfo processInfo].thermalState,
			    [NSProcessInfo processInfo].lowPowerModeEnabled ? "true" : "false",
			    (unsigned long long)gpu.registry, W);
			if (scenario == "idle") {
				std::this_thread::sleep_for(std::chrono::milliseconds(hung + 10));
				check(!gpu.pending, "idle pending");
				frames = 0;
			}
			for (unsigned f = 1; f <= frames; ++f) {
				std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
				    std::chrono::nanoseconds(start + (uint64_t)(f - 1) * interval * 1000)));
				Command c{f, f, (f - 1) % N, (uint32_t)child.pid, 0, 0};
				if (scenario == "delayed" || scenario == "cancel-callback")
					c.delay_ms = 20;
				if (scenario == "missing")
					c.mode = 2;
				if (scenario == "exit-before")
					c.mode = 4;
				if (scenario == "exit-inflight")
					c.mode = 9;
				if (scenario == "consumer-gpu-delay")
					c.mode = 10;
				if (scenario == "broken-queue")
					c.mode = 7;
				if (scenario == "multi-queue")
					c.mode = 8;
				uint64_t obligation = ns(), deadline = obligation + (interval - margin) * 1000;
				io(child.input, &c, sizeof(c), true);
				Receipt receipt{};
				if (c.mode != 4)
					io(child.output, &receipt, sizeof(receipt), false);
				if (scenario == "exit-inflight") {
					check(
					    (getenv("GPU_SYNC_WINE_LAUNCHER")
					         ? receipt.gpu_status == UINT64_MAX
						 : receipt.gpu_status < MTLCommandBufferStatusCompleted) &&
					        gpu.event.signaledValue < c.value,
					    "producer must have actual submitted, incomplete GPU work at termination");
					gpu.unresolved_producer = true;
					check(kill(child.pid, SIGKILL) == 0, "kill in-flight producer");
					child.pid = -1;
					gpu.producer_gate.signaledValue = c.value;
					printf(
					    "{\"event\":\"producer_killed_inflight\",\"gpu_status\":%llu,\"storage_"
					    "retained\":true}\n",
					    (unsigned long long)receipt.gpu_status);
				}
				uint64_t expected = c.value + (scenario == "future" ? 1 : 0);
				while (gpu.event.signaledValue < expected &&
				       ns() - obligation < (uint64_t)hung * 1000000)
					std::this_thread::sleep_for(std::chrono::microseconds(100));
				uint64_t observed = ns();
				bool ready = gpu.event.signaledValue >= expected;
				if (observed > deadline)
					expired++;
				uint64_t selected_presentation = obligation + interval * 1000;
				while (selected_presentation < observed + margin * 1000)
					selected_presentation += interval * 1000;
				printf(
				    "{\"event\":\"presentation_selection\",\"frame\":%u,\"target_ns\":%llu,"
				    "\"deferred\":%s}\n",
				    f, (unsigned long long)selected_presentation,
				    observed > deadline ? "true" : "false");
				printf(
				    "{\"event\":\"gate\",\"frame\":%u,\"image\":%u,\"expected\":%llu,\"observed\":%llu,"
				    "\"obligation_ns\":%llu,\"commit_ns\":%llu,\"ready_ns\":%llu,\"ready\":%s}\n",
				    f, c.image, (unsigned long long)expected,
				    (unsigned long long)gpu.event.signaledValue, (unsigned long long)obligation,
				    (unsigned long long)receipt.commit_ns, (unsigned long long)observed,
				    ready ? "true" : "false");
				if (!ready) {
					gpu.unresolved_producer = true;
					check(scenario == "missing" || scenario == "future" ||
					          scenario == "exit-before" || scenario == "exit-inflight",
					      "unexpected producer stall");
					blocked++;
					check(!gpu.pending, "unsignalled submission");
					if (c.mode != 4 && c.mode != 9)
						child.ack();
					break;
				}
				if (scenario == "cancel-callback") {
					uint64_t callback_deadline = ns() + 1000000000;
					while (!callback_state->callbacks && ns() < callback_deadline)
						std::this_thread::sleep_for(std::chrono::microseconds(100));
					check(callback_state->callbacks == 1 && callback_state->activations == 0 &&
					          !gpu.pending,
					      "late callback reactivated canceled ownership");
					child.ack();
					[notification release];
					break;
				}
				check(scenario != "missing" && scenario != "future" && scenario != "exit-before",
				      "fault unexpectedly ready");
				if (scenario == "exit-inflight")
					gpu.unresolved_producer = false;
				readiness.push_back(observed - obligation);
				if (scenario == "exit-after") {
					kill(child.pid, SIGKILL);
					child.pid = -1;
				}
				uint64_t vk_observed = 0;
				vkcheck(vkGetSemaphoreCounterValue(gpu.device, gpu.semaphore, &vk_observed));
				check(vk_observed >= c.value, "imported Vulkan payload does not match Metal event");
				uint64_t duration = gpu.submit(c);
				submits.push_back(duration);
				printf(
				    "{\"event\":\"vulkan_wait_submit\",\"frame\":%u,\"value\":%llu,\"stage\":"
				    "\"TRANSFER\",\"duration_ns\":%llu}\n",
				    f, (unsigned long long)c.value, (unsigned long long)duration);
				if (scenario == "consumer-gpu-delay") {
					check(!gpu.complete(0) && gpu.pending,
					      "actual consumer GPU work must still own the image");
					printf("{\"event\":\"consumer_gpu_pending\",\"reuse_permitted\":false}\n");
				}
				if (scenario == "consumer-delay") {
					check(gpu.pending, "reuse must remain excluded");
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				check(gpu.complete(), "consumer fence timeout; quarantined until process exit");
				e2e.push_back(ns() - obligation);
				bool valid = gpu.pixels(c);
				check(scenario == "broken-queue" ? !valid : valid, "pixel correctness");
				consumed++;
				// Paired no-wait timing is diagnostic only, and occurs after completed GPU ownership.
				if (scenario == "normal" || scenario == "reuse") {
					for (unsigned k = 0; k < 2; k++) {
						bool wait = (f + k) % 2 == 0;
						uint64_t cost = gpu.overhead(c, wait);
						(wait ? empty_waits : controls).push_back(cost);
						printf("{\"event\":\"%s\",\"frame\":%u,\"duration_ns\":%llu}\n",
						       wait ? "empty_wait_submit" : "empty_control_submit", f,
						       (unsigned long long)cost);
						check(gpu.complete(), "empty submission completion");
					}
				}
				if (scenario == "exit-after" || scenario == "exit-inflight")
					break;
				child.ack();
				if (scenario == "nonmonotonic") {
					Command invalid = c;
					invalid.mode = 6;
					io(child.input, &invalid, sizeof(invalid), true);
					Receipt r{};
					io(child.output, &r, sizeof(r), false);
					check(r.commit_ns == 0, "nonmonotonic accepted");
				}
			}
			if (scenario == "missing" || scenario == "future" || scenario == "exit-before")
				check(blocked == 1 && consumed == 0, "fault must refuse GPU work");
			if (child.pid > 0 && scenario != "exit-before") {
				Command stop{};
				stop.mode = 99;
				io(child.input, &stop, sizeof(stop), true);
			}
			printf(
			    "{\"event\":\"result\",\"scenario\":\"%s\",\"status\":\"PASS\",\"consumed\":%u,\"refused\":"
			    "%u,\"missed_deadlines\":%u,\"runtime_ms\":%.3f,\"submit_us\":{\"p50\":%.3f,\"p95\":%.3f,"
			    "\"p99\":%.3f,\"max\":%.3f},\"no_wait_p50_us\":%.3f,\"empty_wait_p50_us\":%.3f,\"ready_p50_"
			    "us\":%.3f,\"end_to_end_p50_"
			    "us\":%.3f,\"thermal_end\":%ld}\n",
			    scenario.c_str(), consumed, blocked, expired, (ns() - start) / 1e6, percentile(submits, .5),
			    percentile(submits, .95), percentile(submits, .99), percentile(submits, 1),
			    percentile(controls, .5), percentile(empty_waits, .5), percentile(readiness, .5),
			    percentile(e2e, .5), (long)[NSProcessInfo processInfo].thermalState);
			return 0;
		} catch (const std::exception &e) {
			fprintf(stderr, "{\"status\":\"FAIL\",\"error\":\"%s\"}\n", e.what());
			return 1;
		}
	}
}
