// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
extern "C" {
#include "wine/unixlib.h"
}
#include <d3d11_4.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "dxmt_native_interop.h"
#include "macos_gpu_sync_wine.h"
BOOL WINAPI
DllMain(HINSTANCE module, DWORD reason, void *)
{
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(module);
	return !__wine_init_unix_call();
}
static void
require(bool ok, const char *what)
{
	if (!ok) {
		std::fprintf(stderr, "FAIL Wine/DXMT: %s\n", what);
		ExitProcess(1);
	}
}
static void
call(unsigned operation, WineGPUParams &p)
{
	require(WINE_UNIX_CALL(operation, &p) == 0, "native test operation");
}
extern "C" __declspec(dllexport) int WINAPI
RunGPUProducer(ID3D11Device *device, const char *service, const char *socket)
{
	IDXMTNativeDevice2 *native = nullptr;
	ID3D11DeviceContext *context = nullptr;
	ID3D11DeviceContext4 *context4 = nullptr;
	require(SUCCEEDED(device->QueryInterface(DXMT_IID_NATIVE_DEVICE2, (void **)&native)),
	        "actual DXMT native interface");
	WineGPUParams p{};
	require(strlen(service) < sizeof(p.service) && strlen(socket) < sizeof(p.socket), "path lengths");
	strcpy(p.service, service);
	strcpy(p.socket, socket);
	require(SUCCEEDED(native->GetMetalDevice(&p.device)), "DXMT Metal device");
	call(WINE_GPU_INIT, p);
	ID3D11Texture2D *textures[3]{};
	ID3D11Fence *event = nullptr, *gate = nullptr, *join = nullptr;
	D3D11_TEXTURE2D_DESC desc{p.width,
	                          p.width,
	                          1,
	                          1,
	                          DXGI_FORMAT_B8G8R8A8_UNORM,
	                          {1, 0},
	                          D3D11_USAGE_DEFAULT,
	                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
	                          0,
	                          0};
	for (unsigned i = 0; i < 3; ++i)
		require(SUCCEEDED(native->ImportMetalTexture(p.textures[i], &desc, &textures[i])),
		        "zero-copy D3D11 texture import");
	require(SUCCEEDED(native->ImportMetalSharedEvent(p.event, &event)) &&
	            SUCCEEDED(native->ImportMetalSharedEvent(p.gate, &gate)) &&
	            SUCCEEDED(native->ImportMetalSharedEvent(p.join, &join)),
	        "D3D11 shared event import");
	device->GetImmediateContext(&context);
	require(SUCCEEDED(context->QueryInterface(IID_ID3D11DeviceContext4, (void **)&context4)), "D3D11 context4");
	std::fprintf(
	    stderr,
	    "INFO genuine Windows x86-64 D3D11/DXMT producer: %ux%u, three zero-copy images, imported shared fence\n",
	    p.width, p.width);
	uint64_t previous = 0;
	std::vector<uint8_t> pixels(p.width * p.width * 4);
	for (;;) {
		call(WINE_GPU_NEXT, p);
		const Command &c = p.command;
		if (c.mode == 99)
			break;
		if (c.mode == 4)
			ExitProcess(0);
		require(c.image < 3, "image bound");
		if (c.mode == 6) {
			require(c.value <= previous, "non-monotonic fixture");
			call(WINE_GPU_SENT, p);
			continue;
		}
		if (c.delay_ms)
			Sleep(c.delay_ms);
		if (c.mode == 7) {
			call(WINE_GPU_BROKEN_SIGNAL, p);
			call(WINE_GPU_SENT, p);
			call(WINE_GPU_ACK, p);
		}
		if (c.mode == 9)
			require(SUCCEEDED(context4->Wait(gate, c.value)), "controlled in-flight GPU gate");
		for (unsigned y = 0; y < p.width; ++y)
			for (unsigned x = 0; x < p.width; ++x)
				gpu_sync_pattern(&c, x, y, pixels.data() + (y * p.width + x) * 4);
		context->UpdateSubresource(textures[c.image], 0, nullptr, pixels.data(), p.width * 4, 0);
		require(SUCCEEDED(context4->Signal(join, c.value)), "D3D11 internal completion signal");
		if (c.mode != 2 && c.mode != 7 && c.mode != 8)
			require(SUCCEEDED(context4->Signal(event, c.value)), "D3D11 exported producer signal");
		context->Flush();
		if (c.mode == 8)
			call(WINE_GPU_JOIN, p);
		if (c.mode != 7) {
			call(WINE_GPU_SENT, p);
			call(WINE_GPU_ACK, p);
		}
		call(WINE_GPU_COMPLETE, p);
		previous = c.value;
	}
	join->Release();
	gate->Release();
	event->Release();
	for (auto *texture : textures)
		texture->Release();
	context4->Release();
	context->Release();
	native->Release();
	return 0;
}
