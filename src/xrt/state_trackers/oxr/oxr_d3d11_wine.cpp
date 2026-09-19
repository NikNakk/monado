// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Raw-COM D3D11 requirements helpers for the Wine/macOS bridge.
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

static XrResult
get_first_adapter_luid(struct oxr_logger *log, LUID *out_luid)
{
	IDXGIFactory1 *factory = NULL;
	HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void **)&factory);
	if (FAILED(hr) || factory == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " CreateDXGIFactory1 failed: 0x%08lx", (unsigned long)hr);
	}

	IDXGIAdapter1 *adapter = NULL;
	hr = factory->EnumAdapters1(0, &adapter);
	factory->Release();
	if (FAILED(hr) || adapter == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " EnumAdapters1(0) failed: 0x%08lx", (unsigned long)hr);
	}

	DXGI_ADAPTER_DESC1 desc = {};
	hr = adapter->GetDesc1(&desc);
	adapter->Release();
	if (FAILED(hr)) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " IDXGIAdapter1::GetDesc1 failed: 0x%08lx", (unsigned long)hr);
	}

	*out_luid = desc.AdapterLuid;
	return XR_SUCCESS;
}

XrResult
oxr_d3d_get_requirements(struct oxr_logger *log,
                         struct oxr_system *sys,
                         LUID *adapter_luid,
                         D3D_FEATURE_LEVEL *min_feature_level)
{
	if (sys == NULL || adapter_luid == NULL || min_feature_level == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " invalid D3D requirements arguments");
	}

	if (!sys->suggested_d3d_luid_valid) {
		XrResult ret = get_first_adapter_luid(log, &sys->suggested_d3d_luid);
		if (ret != XR_SUCCESS) {
			return ret;
		}
		sys->suggested_d3d_luid_valid = true;
	}

	*adapter_luid = sys->suggested_d3d_luid;
	*min_feature_level = D3D_FEATURE_LEVEL_11_0;
	return XR_SUCCESS;
}

XrResult
oxr_d3d_check_luid(struct oxr_logger *log, struct oxr_system *sys, LUID *adapter_luid)
{
	if (sys == NULL || adapter_luid == NULL || !sys->suggested_d3d_luid_valid) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID, " no Wine D3D adapter LUID available");
	}

	if (adapter_luid->HighPart != sys->suggested_d3d_luid.HighPart ||
	    adapter_luid->LowPart != sys->suggested_d3d_luid.LowPart) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 " supplied Wine D3D11 device does not match the runtime adapter");
	}

	return XR_SUCCESS;
}

XrResult
oxr_d3d11_get_requirements(struct oxr_logger *log,
                           struct oxr_system *sys,
                           XrGraphicsRequirementsD3D11KHR *graphicsRequirements)
{
	return oxr_d3d_get_requirements(log, sys, &graphicsRequirements->adapterLuid,
	                                &graphicsRequirements->minFeatureLevel);
}

XrResult
oxr_d3d11_check_device(struct oxr_logger *log, struct oxr_system *sys, ID3D11Device *device)
{
	if (device == NULL) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID, " D3D11 device is NULL");
	}

	IDXGIDevice *dxgi_device = NULL;
	HRESULT hr = device->QueryInterface(IID_IDXGIDevice, (void **)&dxgi_device);
	if (FAILED(hr) || dxgi_device == NULL) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 " ID3D11Device does not expose IDXGIDevice: 0x%08lx", (unsigned long)hr);
	}

	IDXGIAdapter *adapter = NULL;
	hr = dxgi_device->GetAdapter(&adapter);
	dxgi_device->Release();
	if (FAILED(hr) || adapter == NULL) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 " IDXGIDevice::GetAdapter failed: 0x%08lx", (unsigned long)hr);
	}

	DXGI_ADAPTER_DESC desc = {};
	hr = adapter->GetDesc(&desc);
	adapter->Release();
	if (FAILED(hr)) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 " IDXGIAdapter::GetDesc failed: 0x%08lx", (unsigned long)hr);
	}

	return oxr_d3d_check_luid(log, sys, &desc.AdapterLuid);
}
