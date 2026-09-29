// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental Metal transport companion for XR_FB_foveation.
 *
 * XR_FB_foveation owns policy and swapchain state. This extension only exposes
 * the current Metal rasterization-rate map required by a Metal application to
 * encode rendering that actually benefits from that policy.
 */

#ifndef XR_MNDX_FOVEATION_METAL_H
#define XR_MNDX_FOVEATION_METAL_H 1

#include "openxr_extension_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MNDX_foveation_metal 1
#define XR_MNDX_foveation_metal_SPEC_VERSION 1
#define XR_MNDX_FOVEATION_METAL_EXTENSION_NAME "XR_MNDX_foveation_metal"

XR_STRUCT_ENUM(XR_TYPE_FOVEATION_METAL_STATE_MNDX, 0x7fff5057);

/*!
 * Renderer-facing state for the currently applied XR_FB_foveation profile.
 *
 * rasterizationRateMap is an id<MTLRasterizationRateMap> represented as an
 * opaque pointer, consistent with XR_KHR_metal_enable's opaque Metal handles.
 * The object is owned by the runtime and remains valid until the XrSwapchain
 * is destroyed. Applications must not release it.
 *
 * physicalWidth/physicalHeight describe the rasterized extent for this array
 * layer. They are the dimensions appropriate for the physical-coordinate
 * render region when foveationEnabled is XR_TRUE.
 */
typedef struct XrFoveationMetalStateMNDX {
    XrStructureType       type;
    void* XR_MAY_ALIAS    next;
    XrBool32              foveationEnabled;
    void* XR_MAY_ALIAS    rasterizationRateMap;
    uint32_t              physicalWidth;
    uint32_t              physicalHeight;
    uint32_t              revision;
} XrFoveationMetalStateMNDX;

/*!
 * Return the current Metal render state for one view and swapchain array layer.
 *
 * viewIndex identifies the XrView whose foveation centre is required. It is
 * independent of arrayLayer: applications commonly use separate per-eye
 * swapchains where both eyes render to array layer zero.
 *
 * Call after xrUpdateSwapchainFB and before encoding the Metal render pass
 * which targets this swapchain. A revision change means the returned map may
 * differ from the previous call. Previously returned map objects remain valid
 * for the lifetime of the swapchain so in-flight GPU work is safe.
 */
typedef XrResult (XRAPI_PTR *PFN_xrGetFoveationMetalStateMNDX)(
    XrSwapchain swapchain,
    uint32_t viewIndex,
    uint32_t arrayLayer,
    XrFoveationMetalStateMNDX* state);

#ifdef __cplusplus
}
#endif

#endif
