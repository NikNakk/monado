// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Probe the macOS OpenXR state tracker by loading Monado directly.
 */

#include "openxr/openxr.h"
#include "openxr/openxr_loader_negotiation.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
fail_xr(const char *what, XrResult result)
{
	fprintf(stderr, "%s failed: %d\n", what, result);
	return 1;
}

static int
fail_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	return 1;
}

static int
get_proc(PFN_xrGetInstanceProcAddr get_instance_proc_addr,
         XrInstance instance,
         const char *name,
         PFN_xrVoidFunction *out_fn)
{
	XrResult result = get_instance_proc_addr(instance, name, out_fn);
	if (result != XR_SUCCESS) {
		return fail_xr(name, result);
	}

	if (*out_fn == NULL) {
		return fail_msg("Received NULL function pointer");
	}

	return 0;
}

int
main(void)
{
	const char *runtime_path = getenv("MONADO_OPENXR_RUNTIME_PATH");
	if (runtime_path == NULL || runtime_path[0] == '\0') {
		return fail_msg("Set MONADO_OPENXR_RUNTIME_PATH to libopenxr_monado.dylib");
	}

	void *runtime = dlopen(runtime_path, RTLD_NOW | RTLD_LOCAL);
	if (runtime == NULL) {
		fprintf(stderr, "dlopen(%s) failed: %s\n", runtime_path, dlerror());
		return 1;
	}

	PFN_xrNegotiateLoaderRuntimeInterface negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(
	    runtime, "xrNegotiateLoaderRuntimeInterface");
	if (negotiate == NULL) {
		fprintf(stderr, "dlsym(xrNegotiateLoaderRuntimeInterface) failed: %s\n", dlerror());
		dlclose(runtime);
		return 1;
	}

	XrNegotiateLoaderInfo loader_info = {
	    .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
	    .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
	    .structSize = sizeof(loader_info),
	    .minInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
	    .maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
	    .minApiVersion = XR_CURRENT_API_VERSION,
	    .maxApiVersion = XR_CURRENT_API_VERSION,
	};
	XrNegotiateRuntimeRequest runtime_request = {
	    .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
	    .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION,
	    .structSize = sizeof(runtime_request),
	};

	XrResult result = negotiate(&loader_info, &runtime_request);
	if (result != XR_SUCCESS) {
		dlclose(runtime);
		return fail_xr("xrNegotiateLoaderRuntimeInterface", result);
	}

	PFN_xrGetInstanceProcAddr get_instance_proc_addr = runtime_request.getInstanceProcAddr;
	if (get_instance_proc_addr == NULL) {
		dlclose(runtime);
		return fail_msg("Runtime negotiation returned NULL xrGetInstanceProcAddr");
	}

	PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = NULL;
	PFN_xrCreateInstance xrCreateInstance = NULL;
	PFN_xrDestroyInstance xrDestroyInstance = NULL;
	PFN_xrGetSystem xrGetSystem = NULL;
	PFN_xrCreateSession xrCreateSession = NULL;
	PFN_xrDestroySession xrDestroySession = NULL;

	if (get_proc(get_instance_proc_addr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
	             (PFN_xrVoidFunction *)&xrEnumerateInstanceExtensionProperties) != 0 ||
	    get_proc(get_instance_proc_addr, XR_NULL_HANDLE, "xrCreateInstance",
	             (PFN_xrVoidFunction *)&xrCreateInstance) != 0) {
		dlclose(runtime);
		return 1;
	}

	uint32_t extension_count = 0;
	result = xrEnumerateInstanceExtensionProperties(NULL, 0, &extension_count, NULL);
	if (result != XR_SUCCESS) {
		dlclose(runtime);
		return fail_xr("xrEnumerateInstanceExtensionProperties(count)", result);
	}

	XrExtensionProperties *extensions =
	    (XrExtensionProperties *)calloc(extension_count, sizeof(XrExtensionProperties));
	if (extensions == NULL) {
		dlclose(runtime);
		return fail_msg("calloc(extension properties) failed");
	}

	for (uint32_t i = 0; i < extension_count; ++i) {
		extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}

	result = xrEnumerateInstanceExtensionProperties(NULL, extension_count, &extension_count, extensions);
	if (result != XR_SUCCESS) {
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrEnumerateInstanceExtensionProperties(list)", result);
	}

	bool have_headless = false;
	for (uint32_t i = 0; i < extension_count; ++i) {
		if (strcmp(extensions[i].extensionName, XR_MND_HEADLESS_EXTENSION_NAME) == 0) {
			have_headless = true;
			break;
		}
	}

	if (!have_headless) {
		free(extensions);
		dlclose(runtime);
		return fail_msg("Runtime does not advertise XR_MND_headless");
	}

	const char *enabled_extensions[] = {
	    XR_MND_HEADLESS_EXTENSION_NAME,
	};

	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo instance_info = {
	    .type = XR_TYPE_INSTANCE_CREATE_INFO,
	    .enabledExtensionCount = 1,
	    .enabledExtensionNames = enabled_extensions,
	};
	snprintf(instance_info.applicationInfo.applicationName,
	         sizeof(instance_info.applicationInfo.applicationName), "%s",
	         "tests_macos_openxr_loaderless_probe");
	snprintf(instance_info.applicationInfo.engineName, sizeof(instance_info.applicationInfo.engineName), "%s",
	         "monado");
	instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	result = xrCreateInstance(&instance_info, &instance);
	if (result != XR_SUCCESS) {
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrCreateInstance", result);
	}

	if (get_proc(get_instance_proc_addr, instance, "xrDestroyInstance", (PFN_xrVoidFunction *)&xrDestroyInstance) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrGetSystem", (PFN_xrVoidFunction *)&xrGetSystem) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateSession", (PFN_xrVoidFunction *)&xrCreateSession) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrDestroySession", (PFN_xrVoidFunction *)&xrDestroySession) != 0) {
		xrDestroyInstance(instance);
		free(extensions);
		dlclose(runtime);
		return 1;
	}

	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XrSystemGetInfo system_info = {
	    .type = XR_TYPE_SYSTEM_GET_INFO,
	    .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
	};
	result = xrGetSystem(instance, &system_info, &system_id);
	if (result != XR_SUCCESS) {
		xrDestroyInstance(instance);
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrGetSystem", result);
	}

	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo session_info = {
	    .type = XR_TYPE_SESSION_CREATE_INFO,
	    .systemId = system_id,
	};
	result = xrCreateSession(instance, &session_info, &session);
	if (result != XR_SUCCESS) {
		xrDestroyInstance(instance);
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrCreateSession", result);
	}

	fprintf(stdout, "OpenXR loaderless probe created instance, system, and headless session.\n");

	result = xrDestroySession(session);
	if (result != XR_SUCCESS) {
		xrDestroyInstance(instance);
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrDestroySession", result);
	}

	result = xrDestroyInstance(instance);
	if (result != XR_SUCCESS) {
		free(extensions);
		dlclose(runtime);
		return fail_xr("xrDestroyInstance", result);
	}

	free(extensions);
	dlclose(runtime);
	return 0;
}
