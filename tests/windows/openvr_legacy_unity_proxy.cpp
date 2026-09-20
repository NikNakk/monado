// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
//
// Compatibility proxy for Unity 5.0-5.3-era SteamVR integrations.
//
// Old SteamVR Unity packages expected four UnityHooks_* exports from
// openvr_api.dll and issued render-thread events for WaitGetPoses/Submit.
// Modern OpenComposite intentionally implements only the standard OpenVR API,
// so this proxy forwards those exports to a sibling OpenComposite DLL while
// restoring the legacy Unity render hook ABI for D3D11.

#include "openvr.h"

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>

namespace
{
constexpr int kEventWaitGetPoses = 201510020;
constexpr int kEventSubmitL = 201510021;
constexpr int kEventSubmitR = 201510022;
constexpr int kEventFlush = 201510023;
constexpr int kEventPostPresentHandoff = 201510024;

constexpr int kUnityGfxRendererD3D11 = 2;
constexpr int kUnityGfxDeviceEventInitialize = 0;
constexpr int kUnityGfxDeviceEventShutdown = 1;

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
ID3D11Device *g_device = nullptr;
vr::IVRCompositor *g_compositor = nullptr;
vr::VRTextureBounds_t g_bounds[2] = {
    {0.0f, 0.0f, 1.0f, 1.0f},
    {0.0f, 0.0f, 1.0f, 1.0f},
};
vr::EVRSubmitFlags g_submit_flags = vr::Submit_Default;
vr::EColorSpace g_color_space = vr::ColorSpace_Auto;
std::mutex g_mutex;

void
log_line(const char *message)
{
    std::fprintf(stderr, "[legacy-unity-openvr] %s\n", message);
    std::fflush(stderr);
}

HMODULE
ensure_real()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_real != nullptr) {
        return g_real;
    }

    char path[MAX_PATH] = {};
    if (g_self == nullptr || GetModuleFileNameA(g_self, path, MAX_PATH) == 0) {
        log_line("Could not determine proxy DLL path");
        return nullptr;
    }

    char *slash = nullptr;
    for (char *p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    if (slash == nullptr) {
        log_line("Proxy DLL path has no directory");
        return nullptr;
    }
    slash[1] = '\0';

    const char real_name[] = "openvr_api_opencomposite.dll";
    if (std::strlen(path) + sizeof(real_name) >= MAX_PATH) {
        log_line("OpenComposite sibling path is too long");
        return nullptr;
    }
    std::strcat(path, real_name);

    g_real = LoadLibraryA(path);
    if (g_real == nullptr) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] LoadLibraryA(%s) failed: %lu\n",
                     path,
                     static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
    }
    return g_real;
}

template <typename T>
T
real_proc(const char *name)
{
    HMODULE module = ensure_real();
    if (module == nullptr) return nullptr;
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

vr::IVRCompositor *
get_compositor()
{
    if (g_compositor != nullptr) return g_compositor;

    using Fn = void *(__cdecl *)(const char *, vr::EVRInitError *);
    Fn get = real_proc<Fn>("VR_GetGenericInterface");
    if (get == nullptr) return nullptr;

    vr::EVRInitError error = vr::VRInitError_None;
    void *ptr = get(vr::IVRCompositor_Version, &error);
    if (ptr == nullptr || error != vr::VRInitError_None) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] Could not get %s error=%d\n",
                     vr::IVRCompositor_Version,
                     static_cast<int>(error));
        std::fflush(stderr);
        return nullptr;
    }

    g_compositor = reinterpret_cast<vr::IVRCompositor *>(ptr);
    return g_compositor;
}

ID3D11Texture2D *
current_render_target()
{
    if (g_device == nullptr) {
        log_line("Submit event arrived before Unity supplied a D3D11 device");
        return nullptr;
    }

    ID3D11DeviceContext *context = nullptr;
    g_device->GetImmediateContext(&context);
    if (context == nullptr) return nullptr;

    ID3D11RenderTargetView *rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    context->Release();
    if (rtv == nullptr) {
        log_line("No D3D11 render target is bound at Submit event");
        return nullptr;
    }

    ID3D11Resource *resource = nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if (resource == nullptr) return nullptr;

    ID3D11Texture2D *texture = nullptr;
    HRESULT hr = resource->QueryInterface(IID_ID3D11Texture2D, reinterpret_cast<void **>(&texture));
    resource->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] Bound resource is not ID3D11Texture2D hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        std::fflush(stderr);
        return nullptr;
    }

    return texture;
}

void __stdcall
legacy_render_event(int event_id)
{
    vr::IVRCompositor *compositor = get_compositor();
    if (compositor == nullptr) return;

    switch (event_id) {
    case kEventWaitGetPoses: {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        vr::EVRCompositorError e =
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        if (e != vr::VRCompositorError_None) {
            std::fprintf(stderr, "[legacy-unity-openvr] WaitGetPoses error=%d\n", static_cast<int>(e));
            std::fflush(stderr);
        }
        break;
    }
    case kEventSubmitL:
    case kEventSubmitR: {
        ID3D11Texture2D *texture = current_render_target();
        if (texture == nullptr) break;

        const vr::EVREye eye = event_id == kEventSubmitL ? vr::Eye_Left : vr::Eye_Right;
        const unsigned index = eye == vr::Eye_Left ? 0u : 1u;
        vr::Texture_t submitted = {
            texture,
            vr::TextureType_DirectX,
            g_color_space,
        };
        vr::EVRCompositorError e =
            compositor->Submit(eye, &submitted, &g_bounds[index], g_submit_flags);
        texture->Release();
        if (e != vr::VRCompositorError_None) {
            std::fprintf(stderr,
                         "[legacy-unity-openvr] Submit eye=%u error=%d\n",
                         index,
                         static_cast<int>(e));
            std::fflush(stderr);
        }
        break;
    }
    case kEventFlush:
        if (g_device != nullptr) {
            ID3D11DeviceContext *context = nullptr;
            g_device->GetImmediateContext(&context);
            if (context != nullptr) {
                context->Flush();
                context->Release();
            }
        }
        break;
    case kEventPostPresentHandoff:
        compositor->PostPresentHandoff();
        break;
    default:
        std::fprintf(stderr, "[legacy-unity-openvr] Unknown render event %d\n", event_id);
        std::fflush(stderr);
        break;
    }
}
} // namespace

extern "C" __declspec(dllexport) void __stdcall
UnitySetGraphicsDevice(void *device, int device_type, int event_type)
{
    if (device_type != kUnityGfxRendererD3D11) return;

    if (event_type == kUnityGfxDeviceEventInitialize) {
        auto *next = reinterpret_cast<ID3D11Device *>(device);
        if (next != nullptr) next->AddRef();
        if (g_device != nullptr) g_device->Release();
        g_device = next;
        log_line("Captured Unity D3D11 graphics device");
    } else if (event_type == kUnityGfxDeviceEventShutdown) {
        if (g_device != nullptr) {
            g_device->Release();
            g_device = nullptr;
        }
        log_line("Released Unity D3D11 graphics device");
    }
}

extern "C" __declspec(dllexport) void *
UnityHooks_GetRenderEventFunc()
{
    return reinterpret_cast<void *>(&legacy_render_event);
}

extern "C" __declspec(dllexport) void
UnityHooks_SetSubmitParams(vr::VRTextureBounds_t bounds_l,
                           vr::VRTextureBounds_t bounds_r,
                           vr::EVRSubmitFlags submit_flags)
{
    g_bounds[0] = bounds_l;
    g_bounds[1] = bounds_r;
    g_submit_flags = submit_flags;
}

extern "C" __declspec(dllexport) void
UnityHooks_SetColorSpace(vr::EColorSpace color_space)
{
    g_color_space = color_space;
}

extern "C" __declspec(dllexport) void
UnityHooks_EventWriteString(const wchar_t *event)
{
    if (event != nullptr) {
        OutputDebugStringW(event);
        OutputDebugStringW(L"\n");
    }
}

#define FORWARD_RET(name, ret, args, callargs) \
extern "C" __declspec(dllexport) ret name args { \
    using Fn = ret (__cdecl *) args; \
    Fn fn = real_proc<Fn>(#name); \
    if (fn == nullptr) return {}; \
    return fn callargs; \
}

#define FORWARD_VOID(name, args, callargs) \
extern "C" __declspec(dllexport) void name args { \
    using Fn = void (__cdecl *) args; \
    Fn fn = real_proc<Fn>(#name); \
    if (fn != nullptr) fn callargs; \
}

FORWARD_RET(VR_InitInternal2, uint32_t,
            (vr::EVRInitError *error, vr::EVRApplicationType type, const char *startup),
            (error, type, startup))
FORWARD_RET(VR_InitInternal, uint32_t,
            (vr::EVRInitError *error, vr::EVRApplicationType type),
            (error, type))
FORWARD_VOID(VR_ShutdownInternal, (), ())
FORWARD_RET(VR_GetGenericInterface, void *,
            (const char *version, vr::EVRInitError *error),
            (version, error))
FORWARD_RET(VR_IsInterfaceVersionValid, bool, (const char *version), (version))
FORWARD_RET(VR_IsHmdPresent, bool, (), ())
FORWARD_RET(VR_IsRuntimeInstalled, bool, (), ())
FORWARD_RET(VR_GetVRInitErrorAsSymbol, const char *, (vr::EVRInitError error), (error))
FORWARD_RET(VR_GetVRInitErrorAsEnglishDescription, const char *, (vr::EVRInitError error), (error))
FORWARD_RET(VR_GetStringForHmdError, const char *, (vr::EVRInitError error), (error))
FORWARD_RET(VR_GetInitToken, uint32_t, (), ())
FORWARD_RET(VR_RuntimePath, const char *, (), ())

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_device != nullptr) {
            g_device->Release();
            g_device = nullptr;
        }
        if (g_real != nullptr) {
            FreeLibrary(g_real);
            g_real = nullptr;
        }
    }
    return TRUE;
}
