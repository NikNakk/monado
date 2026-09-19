// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
//
// Minimal OpenVR/OpenComposite loader smoke test. It intentionally does not
// link Valve's openvr_api import library: the OpenComposite DLL path is supplied
// explicitly so the test cannot accidentally start SteamVR.

#include <windows.h>

#include <cstdio>
#include <cstdint>

using VR_InitInternal2_t = uint32_t(__cdecl *)(int32_t *peError, int32_t eApplicationType, const char *pStartupInfo);
using VR_ShutdownInternal_t = void(__cdecl *)();
using VR_GetGenericInterface_t = void *(__cdecl *)(const char *pchInterfaceVersion, int32_t *peError);
using VR_IsHmdPresent_t = bool(__cdecl *)();

static FARPROC
required_proc(HMODULE module, const char *name)
{
	FARPROC proc = GetProcAddress(module, name);
	if (proc == nullptr) {
		std::fprintf(stderr, "Missing OpenVR export: %s\n", name);
	}
	return proc;
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "Usage: %s <OpenComposite openvr_api.dll>\n", argv[0]);
		return 2;
	}

	HMODULE module = LoadLibraryA(argv[1]);
	if (module == nullptr) {
		std::fprintf(stderr, "LoadLibraryA failed for %s: %lu\n", argv[1], GetLastError());
		return 3;
	}

	auto vr_init = reinterpret_cast<VR_InitInternal2_t>(required_proc(module, "VR_InitInternal2"));
	auto vr_shutdown = reinterpret_cast<VR_ShutdownInternal_t>(required_proc(module, "VR_ShutdownInternal"));
	auto vr_get_interface =
	    reinterpret_cast<VR_GetGenericInterface_t>(required_proc(module, "VR_GetGenericInterface"));
	auto vr_is_hmd_present = reinterpret_cast<VR_IsHmdPresent_t>(required_proc(module, "VR_IsHmdPresent"));
	if (vr_init == nullptr || vr_shutdown == nullptr || vr_get_interface == nullptr || vr_is_hmd_present == nullptr) {
		FreeLibrary(module);
		return 4;
	}

	std::printf("OpenComposite DLL loaded: %s\n", argv[1]);
	std::printf("VR_IsHmdPresent before init: %s\n", vr_is_hmd_present() ? "true" : "false");

	// vr::VRApplication_Scene == 1.
	int32_t init_error = 0;
	uint32_t token = vr_init(&init_error, 1, nullptr);
	if (init_error != 0 || token == 0) {
		std::fprintf(stderr, "VR_InitInternal2 failed: error=%d token=%u\n", init_error, token);
		FreeLibrary(module);
		return 5;
	}

	std::printf("VR_InitInternal2 succeeded: token=%u\n", token);

	// OpenComposite supports multiple historical OpenVR interface revisions.
	const char *system_versions[] = {"IVRSystem_022", "IVRSystem_021", "IVRSystem_020", "IVRSystem_019"};
	void *system = nullptr;
	const char *selected_system = nullptr;
	for (const char *version : system_versions) {
		int32_t error = 0;
		system = vr_get_interface(version, &error);
		if (system != nullptr && error == 0) {
			selected_system = version;
			break;
		}
	}

	if (system == nullptr) {
		std::fprintf(stderr, "Could not obtain a supported IVRSystem interface\n");
		vr_shutdown();
		FreeLibrary(module);
		return 6;
	}
	std::printf("OpenVR system interface: %s\n", selected_system);

	const char *compositor_versions[] = {
	    "IVRCompositor_028", "IVRCompositor_027", "IVRCompositor_026", "IVRCompositor_025", "IVRCompositor_024"};
	void *compositor = nullptr;
	const char *selected_compositor = nullptr;
	for (const char *version : compositor_versions) {
		int32_t error = 0;
		compositor = vr_get_interface(version, &error);
		if (compositor != nullptr && error == 0) {
			selected_compositor = version;
			break;
		}
	}

	if (compositor == nullptr) {
		std::fprintf(stderr, "Could not obtain a supported IVRCompositor interface\n");
		vr_shutdown();
		FreeLibrary(module);
		return 7;
	}

	std::printf("OpenVR compositor interface: %s\n", selected_compositor);
	std::printf("PASS: OpenVR -> OpenComposite -> OpenXR runtime initialization succeeded\n");

	vr_shutdown();
	FreeLibrary(module);
	return 0;
}
