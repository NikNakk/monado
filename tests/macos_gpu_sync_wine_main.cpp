// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
int
main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	auto module = LoadLibraryA("monado_gpu_sync.dll");
	auto run =
	    module ? (int(WINAPI *)(ID3D11Device *, const char *, const char *))GetProcAddress(module, "RunGPUProducer")
	           : nullptr;
	if (!run) {
		std::fprintf(stderr, "FAIL Windows producer DLL unavailable, error=%lu\n", GetLastError());
		return 1;
	}
	ID3D11Device *device = nullptr;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
	                               &device, nullptr, nullptr);
	if (FAILED(hr)) {
		std::fprintf(stderr, "FAIL D3D11CreateDevice: %#lx\n", hr);
		return 1;
	}
	int result = run(device, argv[2], argv[3]);
	device->Release();
	return result;
}
