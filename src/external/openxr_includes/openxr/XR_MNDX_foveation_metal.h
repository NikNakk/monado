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
#define XR_MNDX_foveation_metal_SPEC_VERSION 2
#define XR_MNDX_FOVEATION_METAL_EXTENSION_NAME "XR_MNDX_foveation_metal"
#define XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT 16
#define XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT 129

XR_STRUCT_ENUM(XR_TYPE_FOVEATION_METAL_STATE_MNDX, 0x7fff5057);
XR_STRUCT_ENUM(XR_TYPE_FOVEATION_METAL_PACKED_STATE_MNDX, 0x7fff5058);

/*!
 * Renderer-facing state for the currently applied XR_FB_foveation profile.
 *
 * rasterizationRateMap is an id<MTLRasterizationRateMap> represented as an
 * opaque pointer, consistent with XR_KHR_metal_enable's opaque Metal handles.
 * The object is owned by the runtime. Applications must not release it.
 * The returned pointer remains valid until a later successful
 * xrUpdateSwapchainFB changes the revision, or until the XrSwapchain is
 * destroyed. Applications should query again after updating foveation state.
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
 * One OpenXR view packed into a rectangle of the Metal render target.
 */
typedef struct XrFoveationMetalViewMNDX {
    uint32_t    viewIndex;
    XrRect2Di  imageRect;
} XrFoveationMetalViewMNDX;

/*!
 * Optional input/output structure chained to XrFoveationMetalStateMNDX::next.
 *
 * On input, viewCount/views describe all OpenXR views rendered into one
 * swapchain array layer. On output, the sample-rate arrays are the exact
 * descriptor recipe used to construct rasterizationRateMap, and x/y are the
 * matching logical-to-physical mapping. This supports multi-process clients
 * which cannot share a process-local MTLRasterizationRateMap object.
 */
typedef struct XrFoveationMetalPackedStateMNDX {
    XrStructureType                    type;
    void* XR_MAY_ALIAS                 next;
    uint32_t                           viewCount;
    const XrFoveationMetalViewMNDX*    views;
    uint32_t                           horizontalSampleCount;
    uint32_t                           verticalSampleCount;
    float                              horizontalSampleRates[XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT];
    float                              verticalSampleRates[XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT];
    uint32_t                           boundaryCount;
    float                              x[XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT];
    float                              y[XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT];
} XrFoveationMetalPackedStateMNDX;

/*!
 * Return the current Metal render state for one view and swapchain array layer.
 *
 * viewIndex identifies the XrView whose foveation centre is required. It is
 * independent of arrayLayer: applications commonly use separate per-eye
 * swapchains where both eyes render to array layer zero.
 *
 * If XrFoveationMetalPackedStateMNDX is chained to state->next, viewIndex is
 * ignored and the runtime returns one map covering the supplied packed view
 * rectangles. That map becomes the active map for compositor reconstruction.
 *
 * Call after xrUpdateSwapchainFB and before encoding the Metal render pass
 * which targets this swapchain. A revision change means the returned map may
 * differ from the previous call. A revision change invalidates previously
 * returned borrowed pointers for future encoding; query again before encoding
 * subsequent render passes. Metal retains resources referenced by already
 * encoded command buffers, so submitted work remains safe.
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
