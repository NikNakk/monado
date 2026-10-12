// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Wine unixlib: native object transport only. All pixel writes use Windows D3D11.
#include "macos_gpu_sync_common.h"
#include "macos_gpu_sync_wine.h"
static int channel = -1;
static id<MTLDevice> device;
static id<MTLSharedEvent> event, join_event;
static id<MTLCommandQueue> queue;
static GPUSyncEventService *service;
static NSXPCListener *listener;
static IOSurfaceRef surfaces[N];
static id<MTLTexture> textures[N];

static int32_t
initialise(void *args)
{
	auto *p = static_cast<WineGPUParams *>(args);
	@autoreleasepool {
		device = [(id<MTLDevice>)p->device retain];
		check(device != nil && W != 0, "Wine native device/size");
		queue = [device newCommandQueue];
		event = [device newSharedEvent];
		join_event = [device newSharedEvent];
		service = [GPUSyncEventService new];
		service.event = event;
		service.gate = [device newSharedEvent];
		listener = [[NSXPCListener alloc] initWithMachServiceName:[NSString stringWithUTF8String:p->service]];
		listener.delegate = service;
		[listener resume];
		channel = socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		strlcpy(address.sun_path, p->socket, sizeof(address.sun_path));
		check(channel >= 0 && connect(channel, (sockaddr *)&address, sizeof(address)) == 0,
		      "Wine native connect");
		pid_t pid = getpid();
		io(channel, &pid, sizeof(pid), true);
		NSMutableArray *ids = [NSMutableArray array];
		for (unsigned i = 0; i < N; ++i) {
			surfaces[i] = surface();
			auto *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                                width:W
			                                                               height:W
			                                                            mipmapped:NO];
			desc.storageMode = MTLStorageModeShared;
			desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
			textures[i] = [device newTextureWithDescriptor:desc iosurface:surfaces[i] plane:0];
			check(textures[i] != nil, "Wine native texture");
			p->textures[i] = (uint64_t)textures[i];
			[ids addObject:@(IOSurfaceGetID(surfaces[i]))];
		}
		NSData *archive = [NSKeyedArchiver archivedDataWithRootObject:@{
			@"surfaces" : ids,
			@"registry" : @(device.registryID)
		}
		                                        requiringSecureCoding:YES
		                                                        error:nil];
		check(archive != nil, "Wine metadata archive");
		uint32_t length = (uint32_t)archive.length;
		io(channel, &length, sizeof(length), true);
		io(channel, (void *)archive.bytes, length, true);
		p->event = (uint64_t)event;
		p->gate = (uint64_t)service.gate;
		p->join = (uint64_t)join_event;
		p->width = W;
	}
	return 0;
}
static int32_t
next(void *args)
{
	io(channel, &static_cast<WineGPUParams *>(args)->command, sizeof(Command), false, 65000);
	return 0;
}
static int32_t
sent(void *args)
{
	// DXMT does not expose its command buffer status. The consumer instead
	// proves that the exported D3D11 fence is pending at in-flight termination.
	Receipt receipt{};
	if (static_cast<WineGPUParams *>(args)->command.mode != 6)
		receipt = {ns(), UINT64_MAX};
	io(channel, &receipt, sizeof(receipt), true);
	return 0;
}
static int32_t
ack(void *)
{
	char a;
	io(channel, &a, 1, false);
	return 0;
}
static int32_t
signal_join(void *args)
{
	uint64_t value = static_cast<WineGPUParams *>(args)->command.value;
	@autoreleasepool {
		auto command = [queue commandBuffer];
		[command encodeWaitForEvent:join_event value:value];
		[command encodeSignalEvent:event value:value];
		[command commit];
	}
	return 0;
}
static int32_t
broken_signal(void *args)
{
	@autoreleasepool {
		auto command = [queue commandBuffer];
		[command encodeSignalEvent:event value:static_cast<WineGPUParams *>(args)->command.value];
		[command commit];
	}
	return 0;
}
static int32_t
complete(void *args)
{
	uint64_t value = static_cast<WineGPUParams *>(args)->command.value;
	uint64_t deadline = ns() + 1000000000;
	while (join_event.signaledValue < value && ns() < deadline)
		usleep(100);
	return join_event.signaledValue >= value ? 0 : 1;
}
extern "C" {
typedef int32_t (*unixlib_entry_t)(void *);
}
template <unixlib_entry_t operation>
static int32_t
guarded(void *args)
{
	try {
		return operation(args);
	} catch (const std::exception &error) {
		fprintf(stderr, "FAIL Wine native transport: %s\n", error.what());
		return 1;
	}
}
extern "C" {
__attribute__((visibility("default"))) unixlib_entry_t __wine_unix_call_funcs[] = {
    guarded<initialise>,  guarded<next>,          guarded<sent>,    guarded<ack>,
    guarded<signal_join>, guarded<broken_signal>, guarded<complete>};
}
