// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
//
// Visible OpenVR -> OpenComposite -> OpenXR D3D11 smoke test.
// Dynamically loads OpenComposite so SteamVR cannot be selected accidentally.

#include "openvr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>

using VR_InitInternal2_t = uint32_t(__cdecl *)(int32_t *, int32_t, const char *);
using VR_ShutdownInternal_t = void(__cdecl *)();
using VR_GetGenericInterface_t = void *(__cdecl *)(const char *, int32_t *);

static void
release_texture(ID3D11Texture2D *&texture, ID3D11RenderTargetView *&rtv)
{
	if (rtv != nullptr) {
		rtv->Release();
		rtv = nullptr;
	}
	if (texture != nullptr) {
		texture->Release();
		texture = nullptr;
	}
}

int
main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		std::fprintf(stderr, "Usage: %s <OpenComposite openvr_api.dll> [frames]\n", argv[0]);
		return 2;
	}
	int frames = argc == 3 ? std::atoi(argv[2]) : 360;
	if (frames < 1 || frames > 36000) {
		std::fprintf(stderr, "frames must be 1..36000\n");
		return 2;
	}

	HMODULE module = LoadLibraryA(argv[1]);
	if (module == nullptr) {
		std::fprintf(stderr, "LoadLibraryA failed: %lu\n", GetLastError());
		return 3;
	}

	auto vr_init = reinterpret_cast<VR_InitInternal2_t>(GetProcAddress(module, "VR_InitInternal2"));
	auto vr_shutdown = reinterpret_cast<VR_ShutdownInternal_t>(GetProcAddress(module, "VR_ShutdownInternal"));
	auto vr_get = reinterpret_cast<VR_GetGenericInterface_t>(GetProcAddress(module, "VR_GetGenericInterface"));
	if (vr_init == nullptr || vr_shutdown == nullptr || vr_get == nullptr) {
		std::fprintf(stderr, "OpenComposite is missing required OpenVR exports\n");
		FreeLibrary(module);
		return 4;
	}

	int32_t init_error = 0;
	uint32_t token = vr_init(&init_error, 1 /* VRApplication_Scene */, nullptr);
	if (init_error != 0 || token == 0) {
		std::fprintf(stderr, "VR_InitInternal2 failed error=%d token=%u\n", init_error, token);
		FreeLibrary(module);
		return 5;
	}

	int32_t error = 0;
	auto *system = reinterpret_cast<vr::IVRSystem *>(vr_get(vr::IVRSystem_Version, &error));
	if (system == nullptr || error != 0) {
		std::fprintf(stderr, "Could not obtain %s error=%d\n", vr::IVRSystem_Version, error);
		vr_shutdown();
		FreeLibrary(module);
		return 6;
	}

	error = 0;
	auto *compositor = reinterpret_cast<vr::IVRCompositor *>(vr_get(vr::IVRCompositor_Version, &error));
	if (compositor == nullptr || error != 0) {
		std::fprintf(stderr, "Could not obtain %s error=%d\n", vr::IVRCompositor_Version, error);
		vr_shutdown();
		FreeLibrary(module);
		return 7;
	}

	uint32_t width = 0, height = 0;
	system->GetRecommendedRenderTargetSize(&width, &height);
	if (width == 0 || height == 0) {
		std::fprintf(stderr, "OpenVR returned invalid recommended render size %ux%u\n", width, height);
		vr_shutdown();
		FreeLibrary(module);
		return 8;
	}

	int32_t adapter_index = -1;
	system->GetDXGIOutputInfo(&adapter_index);
	std::printf("OpenVR recommended per-eye size: %ux%u adapter=%d\n", width, height, adapter_index);

	IDXGIFactory1 *factory = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) &&
	    factory != nullptr && adapter_index >= 0) {
		factory->EnumAdapters1(static_cast<UINT>(adapter_index), &adapter);
	}
	if (factory != nullptr) factory->Release();

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
	D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(adapter,
	                               adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr,
	                               0,
	                               requested,
	                               2,
	                               D3D11_SDK_VERSION,
	                               &device,
	                               &obtained,
	                               &context);
	if (adapter != nullptr) adapter->Release();
	if (FAILED(hr) || device == nullptr || context == nullptr) {
		std::fprintf(stderr, "D3D11CreateDevice failed hr=0x%08lx\n", (unsigned long)hr);
		vr_shutdown();
		FreeLibrary(module);
		return 9;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	ID3D11Texture2D *left = nullptr, *right = nullptr;
	ID3D11RenderTargetView *left_rtv = nullptr, *right_rtv = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &left)) || left == nullptr ||
	    FAILED(device->CreateRenderTargetView(left, nullptr, &left_rtv)) || left_rtv == nullptr ||
	    FAILED(device->CreateTexture2D(&desc, nullptr, &right)) || right == nullptr ||
	    FAILED(device->CreateRenderTargetView(right, nullptr, &right_rtv)) || right_rtv == nullptr) {
		std::fprintf(stderr, "Failed to create D3D11 eye render targets\n");
		release_texture(left, left_rtv);
		release_texture(right, right_rtv);
		context->Release();
		device->Release();
		vr_shutdown();
		FreeLibrary(module);
		return 10;
	}

	vr::Texture_t left_texture = {left, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
	vr::Texture_t right_texture = {right, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};

	compositor->SetTrackingSpace(vr::TrackingUniverseStanding);
	for (int frame = 0; frame < frames; ++frame) {
		vr::EVRCompositorError e =
		    compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
		if (e != vr::VRCompositorError_None) {
			std::fprintf(stderr, "WaitGetPoses failed frame=%d error=%d\n", frame, (int)e);
			break;
		}

		float phase = (float)(frame % 120) / 119.0f;
		const float left_color[4] = {0.08f + 0.45f * phase, 0.12f, 0.55f - 0.35f * phase, 1.0f};
		const float right_color[4] = {0.55f - 0.35f * phase, 0.18f + 0.35f * phase, 0.08f, 1.0f};
		context->ClearRenderTargetView(left_rtv, left_color);
		context->ClearRenderTargetView(right_rtv, right_color);

		e = compositor->Submit(vr::Eye_Left, &left_texture);
		if (e != vr::VRCompositorError_None) {
			std::fprintf(stderr, "Submit left failed frame=%d error=%d\n", frame, (int)e);
			break;
		}
		e = compositor->Submit(vr::Eye_Right, &right_texture);
		if (e != vr::VRCompositorError_None) {
			std::fprintf(stderr, "Submit right failed frame=%d error=%d\n", frame, (int)e);
			break;
		}
		compositor->PostPresentHandoff();

		if (frame == 0 || ((frame + 1) % 120) == 0) {
			std::printf("submitted frame %d/%d hmd_pose_valid=%s\n",
			            frame + 1,
			            frames,
			            poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid ? "true" : "false");
			std::fflush(stdout);
		}
	}

	context->Flush();
	release_texture(left, left_rtv);
	release_texture(right, right_rtv);
	context->Release();
	device->Release();

	vr_shutdown();
	FreeLibrary(module);
	std::printf("PASS: rendered OpenVR D3D11 frames through OpenComposite -> Monado OpenXR\n");
	return 0;
}
