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
bool g_submitted_left = false;
bool g_submitted_right = false;
uint64_t g_event_counts[5] = {};
double g_event_total_ms[5] = {};
double g_event_max_ms[5] = {};
LARGE_INTEGER g_qpc_frequency = {};

using LegacyGetFrameTimingFn = bool (__stdcall *)(vr::Compositor_FrameTiming *, uint32_t);
LegacyGetFrameTimingFn g_real_get_frame_timing = nullptr;
bool g_compositor_fn_table_patched = false;
uint64_t g_get_frame_timing_calls = 0;

double
elapsed_ms(LARGE_INTEGER begin, LARGE_INTEGER end)
{
    if (g_qpc_frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_frequency);
    }
    return 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) /
           static_cast<double>(g_qpc_frequency.QuadPart);
}

void
record_event_timing(unsigned slot, const char *name, LARGE_INTEGER begin)
{
    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&end);
    const double ms = elapsed_ms(begin, end);
    ++g_event_counts[slot];
    g_event_total_ms[slot] += ms;
    if (ms > g_event_max_ms[slot]) g_event_max_ms[slot] = ms;

    if (ms >= 20.0 || (g_event_counts[slot] % 120) == 0) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] timing %s count=%llu last=%.3fms avg=%.3fms max=%.3fms\n",
                     name,
                     static_cast<unsigned long long>(g_event_counts[slot]),
                     ms,
                     g_event_total_ms[slot] / static_cast<double>(g_event_counts[slot]),
                     g_event_max_ms[slot]);
        std::fflush(stderr);
    }
}

void
log_line(const char *message)
{
    std::fprintf(stderr, "[legacy-unity-openvr] %s\n", message);
    std::fflush(stderr);
}

HMODULE
ensure_real()
{
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

bool __stdcall
legacy_get_frame_timing(vr::Compositor_FrameTiming *timing, uint32_t frames_ago)
{
    if (g_real_get_frame_timing == nullptr) {
        return false;
    }

    const bool ok = g_real_get_frame_timing(timing, frames_ago);
    ++g_get_frame_timing_calls;

    const uint32_t raw_presents =
        (timing != nullptr) ? timing->m_nNumFramePresents : 0xffffffffu;
    if (g_get_frame_timing_calls <= 20 || (g_get_frame_timing_calls % 120) == 0) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] GetFrameTiming count=%llu ok=%d framesAgo=%u size=%u rawPresents=%u\n",
                     static_cast<unsigned long long>(g_get_frame_timing_calls),
                     ok ? 1 : 0,
                     frames_ago,
                     timing != nullptr ? timing->m_nSize : 0u,
                     raw_presents);
        std::fflush(stderr);
    }

    if (ok && timing != nullptr && timing->m_nNumFramePresents == 0) {
        timing->m_nNumFramePresents = 1;
    }
    return ok;
}

void
patch_legacy_compositor_fn_table(void *table_ptr, const char *version)
{
    if (g_compositor_fn_table_patched || table_ptr == nullptr) {
        return;
    }

    // In the legacy OpenVR compositor function tables used by Unity 5.0-5.3,
    // GetFrameTiming is entry 8:
    // SetTrackingSpace, GetTrackingSpace, WaitGetPoses, GetLastPoses,
    // GetLastPoseForTrackedDeviceIndex, Submit, ClearLastSubmittedFrame,
    // PostPresentHandoff, GetFrameTiming.
    auto **table = reinterpret_cast<void **>(table_ptr);
    void **slot = &table[8];

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        std::fprintf(stderr,
                     "[legacy-unity-openvr] Could not patch %s GetFrameTiming table slot: %lu\n",
                     version ? version : "<unknown>",
                     static_cast<unsigned long>(GetLastError()));
        std::fflush(stderr);
        return;
    }

    g_real_get_frame_timing = reinterpret_cast<LegacyGetFrameTimingFn>(*slot);
    *slot = reinterpret_cast<void *>(&legacy_get_frame_timing);

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));

    g_compositor_fn_table_patched = true;
    std::fprintf(stderr,
                 "[legacy-unity-openvr] Patched legacy compositor GetFrameTiming (%s)\n",
                 version ? version : "<unknown>");
    std::fflush(stderr);
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

    LARGE_INTEGER begin = {};
    QueryPerformanceCounter(&begin);

    switch (event_id) {
    case kEventWaitGetPoses: {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        vr::EVRCompositorError e =
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        if (e != vr::VRCompositorError_None) {
            std::fprintf(stderr, "[legacy-unity-openvr] WaitGetPoses error=%d\n", static_cast<int>(e));
            std::fflush(stderr);
        }
        record_event_timing(0, "WaitGetPoses", begin);
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
        if (e == vr::VRCompositorError_None) {
            if (eye == vr::Eye_Left) {
                g_submitted_left = true;
            } else {
                g_submitted_right = true;
            }
        } else {
            std::fprintf(stderr,
                         "[legacy-unity-openvr] Submit eye=%u error=%d\n",
                         index,
                         static_cast<int>(e));
            std::fflush(stderr);
        }
        record_event_timing(eye == vr::Eye_Left ? 1u : 2u,
                            eye == vr::Eye_Left ? "SubmitL" : "SubmitR",
                            begin);
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
        record_event_timing(3, "Flush", begin);
        break;
    case kEventPostPresentHandoff:
        // Old Unity 5.x SteamVR integrations can emit this plugin event far
        // more often than actual rendered frames. Forwarding every event to
        // OpenComposite causes a pathological handoff storm. A handoff is
        // meaningful only after a complete stereo frame has been submitted.
        if (g_submitted_left && g_submitted_right) {
            compositor->PostPresentHandoff();
            g_submitted_left = false;
            g_submitted_right = false;
        }
        record_event_timing(4, "PostPresentHandoff", begin);
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
extern "C" __declspec(dllexport) void *
VR_GetGenericInterface(const char *version, vr::EVRInitError *error)
{
    using Fn = void *(__cdecl *)(const char *, vr::EVRInitError *);
    Fn fn = real_proc<Fn>("VR_GetGenericInterface");
    if (fn == nullptr) {
        return nullptr;
    }

    void *result = fn(version, error);

    if (result != nullptr && version != nullptr &&
        std::strncmp(version, "FnTable:IVRCompositor_", 22) == 0) {
        patch_legacy_compositor_fn_table(result, version);
    }

    return result;
}
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
