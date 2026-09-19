// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Windows D3D11 producer for the macOS Wine/DXMT IOSurface import probe.
 *
 * Creates three BGRA8 D3D11_RESOURCE_MISC_SHARED_NTHANDLE textures under the
 * Basalt-patched DXMT path, writes a distinct colour into each, waits for GPU
 * completion with an ID3D11Fence, republishes the Basalt IOSurface IDs, and
 * keeps the D3D11 resources alive until MONADO_IOSURFACE_DONE_FILE appears.
 */

#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr uint32_t kImageCount = 3;
constexpr DWORD kFenceWaitMilliseconds = 10000;
constexpr DWORD kDoneFileWaitMilliseconds = 120000;

// Must match BASALT_GUID_IOSURFACE_ID in Basalt's DXMT fork.
// {DD807311-529E-4856-A5C0-48BED2048129}
constexpr GUID kBasaltIOSurfaceIdGuid = {
    0xdd807311, 0x529e, 0x4856, {0xa5, 0xc0, 0x48, 0xbe, 0xd2, 0x04, 0x81, 0x29}};

template <typename Interface>
void
release(Interface *&object)
{
	if (object != nullptr) {
		object->Release();
		object = nullptr;
	}
}

int
fail(const char *operation, HRESULT result)
{
	std::fprintf(stderr,
	             "FAIL: %s returned HRESULT 0x%08lx\n",
	             operation,
	             static_cast<unsigned long>(result));
	return 1;
}

HRESULT
create_shared_texture(ID3D11Device *device, ID3D11Texture2D **out_texture)
{
	D3D11_TEXTURE2D_DESC description{};
	description.Width = kWidth;
	description.Height = kHeight;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	return device->CreateTexture2D(&description, nullptr, out_texture);
}

bool
query_iosurface_id(ID3D11Texture2D *texture, uint32_t *out_id)
{
	uint32_t id = 0;
	UINT size = sizeof(id);
	HRESULT result = texture->GetPrivateData(kBasaltIOSurfaceIdGuid, &size, &id);
	if (FAILED(result) || size != sizeof(id) || id == 0) {
		std::fprintf(stderr,
		             "FAIL: GetPrivateData(BASALT_GUID_IOSURFACE_ID) HRESULT=0x%08lx size=%u id=%u\n",
		             static_cast<unsigned long>(result),
		             size,
		             id);
		return false;
	}

	*out_id = id;
	return true;
}

} // namespace

int
main()
{
	const char *done_file = std::getenv("MONADO_IOSURFACE_DONE_FILE");
	if (done_file == nullptr || done_file[0] == '\0') {
		std::fprintf(stderr, "FAIL: MONADO_IOSURFACE_DONE_FILE is not set\n");
		return 1;
	}

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	D3D_FEATURE_LEVEL selected_feature_level{};
	const D3D_FEATURE_LEVEL requested_feature_levels[] = {
	    D3D_FEATURE_LEVEL_11_0,
	};

	HRESULT result = D3D11CreateDevice(nullptr,
	                                   D3D_DRIVER_TYPE_HARDWARE,
	                                   nullptr,
	                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	                                   requested_feature_levels,
	                                   static_cast<UINT>(sizeof(requested_feature_levels) / sizeof(requested_feature_levels[0])),
	                                   D3D11_SDK_VERSION,
	                                   &device,
	                                   &selected_feature_level,
	                                   &context);
	if (FAILED(result)) {
		return fail("D3D11CreateDevice", result);
	}

	int exit_code = 1;
	std::array<ID3D11Texture2D *, kImageCount> textures{};
	ID3D11Device5 *device5 = nullptr;
	ID3D11DeviceContext4 *context4 = nullptr;
	ID3D11Fence *fence = nullptr;
	HANDLE fence_event = nullptr;

	do {
		const std::array<uint32_t, kImageCount> colors = {
		    0xfff02020u, // BGRA bytes: 20 20 f0 ff
		    0xff20f020u, // BGRA bytes: 20 f0 20 ff
		    0xff2020f0u, // BGRA bytes: f0 20 20 ff
		};

		for (uint32_t i = 0; i < kImageCount; ++i) {
			result = create_shared_texture(device, &textures[i]);
			if (FAILED(result)) {
				exit_code = fail("CreateTexture2D(shared NTHANDLE)", result);
				goto cleanup;
			}

			std::vector<uint32_t> pixels(kWidth * kHeight, colors[i]);
			context->UpdateSubresource(textures[i],
			                           0,
			                           nullptr,
			                           pixels.data(),
			                           kWidth * sizeof(uint32_t),
			                           0);
		}

		result = device->QueryInterface(IID_ID3D11Device5, reinterpret_cast<void **>(&device5));
		if (FAILED(result)) {
			exit_code = fail("QueryInterface(ID3D11Device5)", result);
			break;
		}
		result = context->QueryInterface(IID_ID3D11DeviceContext4, reinterpret_cast<void **>(&context4));
		if (FAILED(result)) {
			exit_code = fail("QueryInterface(ID3D11DeviceContext4)", result);
			break;
		}
		result = device5->CreateFence(0,
		                             D3D11_FENCE_FLAG_NONE,
		                             IID_ID3D11Fence,
		                             reinterpret_cast<void **>(&fence));
		if (FAILED(result)) {
			exit_code = fail("ID3D11Device5::CreateFence", result);
			break;
		}

		fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (fence_event == nullptr) {
			exit_code = fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
			break;
		}

		result = fence->SetEventOnCompletion(1, fence_event);
		if (FAILED(result)) {
			exit_code = fail("ID3D11Fence::SetEventOnCompletion", result);
			break;
		}
		result = context4->Signal(fence, 1);
		if (FAILED(result)) {
			exit_code = fail("ID3D11DeviceContext4::Signal", result);
			break;
		}
		context->Flush();

		const DWORD wait_status = WaitForSingleObject(fence_event, kFenceWaitMilliseconds);
		if (wait_status != WAIT_OBJECT_0) {
			std::fprintf(stderr,
			             "FAIL: D3D11 fence wait returned %lu\n",
			             static_cast<unsigned long>(wait_status));
			break;
		}

		std::array<uint32_t, kImageCount> ids{};
		for (uint32_t i = 0; i < kImageCount; ++i) {
			if (!query_iosurface_id(textures[i], &ids[i])) {
				break;
			}
		}
		if (ids[0] == 0 || ids[1] == 0 || ids[2] == 0 ||
		    ids[0] == ids[1] || ids[0] == ids[2] || ids[1] == ids[2]) {
			std::fprintf(stderr,
			             "FAIL: IOSurface IDs are not nonzero and distinct: %u %u %u\n",
			             ids[0],
			             ids[1],
			             ids[2]);
			break;
		}

		// stdout is intentionally machine-readable for the host harness.
		std::printf("%u %u %u\n", ids[0], ids[1], ids[2]);
		std::fflush(stdout);
		std::fprintf(stderr,
		             "D3D11 producer is holding three BGRA8 IOSurface-backed textures alive.\n");

		bool done = false;
		for (DWORD waited = 0; waited < kDoneFileWaitMilliseconds; waited += 100) {
			if (GetFileAttributesA(done_file) != INVALID_FILE_ATTRIBUTES) {
				done = true;
				break;
			}
			Sleep(100);
		}
		if (!done) {
			std::fprintf(stderr, "FAIL: timed out waiting for %s\n", done_file);
			break;
		}

		std::fprintf(stderr, "PASS: host completed external IOSurface import while D3D11 resources remained alive.\n");
		exit_code = 0;
	} while (false);

cleanup:
	if (fence_event != nullptr) {
		CloseHandle(fence_event);
	}
	release(fence);
	release(context4);
	release(device5);
	for (auto &texture : textures) {
		release(texture);
	}
	release(context);
	release(device);
	return exit_code;
}
