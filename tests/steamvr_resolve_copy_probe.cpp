// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Uses the external production SteamVR backend, with counted copy calls.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "xr_backend.h"

extern unsigned long long mwxr_test_copies;
namespace mwxr {
void Log(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
}
}
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int writer(int argc, char **argv) {
    REQUIRE(argc == 4);
    ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11Texture2D *texture = nullptr;
        HANDLE handle = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[eye+2], nullptr, 16)));
        REQUIRE(SUCCEEDED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&texture)));
        ID3D11RenderTargetView *view = nullptr;
        REQUIRE(SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view)));
        const float colour[2][4] = {{1, 0, 0, 1}, {0, 0, 1, 1}};
        context->ClearRenderTargetView(view, colour[eye]);
        D3D11_TEXTURE2D_DESC desc; texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *staging = nullptr;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
        context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        REQUIRE(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
        // Map establishes real completion of the child writer before parent release.
        REQUIRE(*static_cast<unsigned *>(mapped.pData) == (eye ? 0xffff0000u : 0xff0000ffu));
        context->Unmap(staging, 0);
        staging->Release(); view->Release(); texture->Release();
    }
    context->Release(); device->Release();
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "writer") return writer(argc, argv);
    REQUIRE(argc == 2);
    REQUIRE(std::string(argv[1]) == "three" || std::string(argv[1]) == "four");
    const bool expect_shared = std::string(argv[1]) == "three";
    mwxr::XrBackend backend; std::string error;
    if (!backend.Init(error)) { fprintf(stderr, "Init: %s\n", error.c_str()); return 1; }
    ID3D11Texture2D *textures[2][3]{}; HANDLE handles[2][3]{};
    XrSwapchain chains[2]{};
    for (int eye = 0; eye < 2; ++eye) {
        bool shared = backend.CreateSharedSwapchain(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                  chains[eye], textures[eye], handles[eye]);
        REQUIRE(shared == expect_shared);
        if (!shared) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 128; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            for (int image = 0; image < 3; ++image) {
                REQUIRE(SUCCEEDED(backend.device->CreateTexture2D(&desc, nullptr, &textures[eye][image])));
                IDXGIResource *resource = nullptr;
                REQUIRE(SUCCEEDED(textures[eye][image]->QueryInterface(__uuidof(IDXGIResource), (void **)&resource)));
                REQUIRE(SUCCEEDED(resource->GetSharedHandle(&handles[eye][image]))); resource->Release();
            }
        }
    }
    unsigned masks[2]{};
    // The first frame can legitimately have shouldRender=false while session
    // visibility settles. Warm up before counting the 18 rendering frames.
    for (int frame = 0; frame < 3; ++frame) {
        backend.PollEvents(); double offset, period;
        REQUIRE(backend.WaitAndBeginFrame(offset, period));
        backend.Present({});
    }
    for (int frame = 0; frame < 18; ++frame) {
        backend.PollEvents(); double offset, period;
        REQUIRE(backend.WaitAndBeginFrame(offset, period));
        int indices[2];
        mwxr::LayerSubmit layer{};
        for (int eye = 0; eye < 2; ++eye) {
            indices[eye] = expect_shared ? backend.AcquireImage(chains[eye]) : frame % 3;
            REQUIRE(indices[eye] >= 0 && indices[eye] < 3); masks[eye] |= 1u << indices[eye];
            layer.eye[eye] = {textures[eye][indices[eye]], chains[eye], {0,0,0,128,128,1},
                             backend.eyeInHead[eye], backend.fov[eye]};
        }
        char executable[MAX_PATH]; REQUIRE(GetModuleFileNameA(nullptr, executable, MAX_PATH));
        char command[2048];
        snprintf(command, sizeof(command), "\"%s\" writer %llx %llx", executable,
                 (unsigned long long)uintptr_t(handles[0][indices[0]]),
                 (unsigned long long)uintptr_t(handles[1][indices[1]]));
        STARTUPINFOA startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        REQUIRE(CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
        DWORD waited = WaitForSingleObject(process.hProcess, 15000);
        if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
        DWORD status = 1; GetExitCodeProcess(process.hProcess, &status);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        REQUIRE(waited == WAIT_OBJECT_0 && status == 0);
        backend.Present({layer});
        printf("{\"frame\":%d,\"left_image\":%d,\"right_image\":%d,\"driver_copies\":%llu}\n",
               frame+1, indices[0], indices[1], mwxr_test_copies); fflush(stdout);
    }
    REQUIRE(masks[0] == 7 && masks[1] == 7);
    REQUIRE(mwxr_test_copies == (expect_shared ? 0u : 36u));
    // Renderer diagnostics sample every 120 ticks after startup. Retain the
    // final delivered image for two further sampled compositor readbacks.
    Sleep(4500);
    for (int eye = 0; eye < 2; ++eye) {
        if (chains[eye]) backend.DestroySwapchain(chains[eye]);
        else for (auto texture : textures[eye]) texture->Release();
    }
    backend.Shutdown();
    printf("{\"status\":\"PASS\",\"runtime_owned_resolve\":%s,\"frames\":18,\"driver_copies\":%llu}\n",
           expect_shared ? "true" : "false", mwxr_test_copies);
    return 0;
}
