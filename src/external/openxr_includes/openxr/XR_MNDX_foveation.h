// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental graphics-API-independent foveated-rendering contract.
 *
 * Applications obtain normalized per-axis foveation policy from the runtime,
 * implement it with their graphics API's native VRS mechanism, then attach the
 * actual logical-to-physical mapping when that mechanism uses non-uniform
 * raster coordinates. Mapping coordinates are normalized to the full
 * swapchain image, so the same mapping may be chained to multiple projection
 * views that reference subimages of a packed render target. Full-resolution
 * VRS backends may omit the mapping.
 */

#ifndef XR_MNDX_FOVEATION_H
#define XR_MNDX_FOVEATION_H 1

#include "openxr_extension_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MNDX_foveation 1
#define XR_MNDX_foveation_SPEC_VERSION 1
#define XR_MNDX_FOVEATION_EXTENSION_NAME "XR_MNDX_foveation"
#define XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT 129

XR_STRUCT_ENUM(XR_TYPE_FOVEATION_PROFILE_MNDX, 0x7fff5055);
XR_STRUCT_ENUM(XR_TYPE_COMPOSITION_LAYER_FOVEATION_MAP_MNDX, 0x7fff5056);

typedef enum XrFoveationLevelMNDX {
    XR_FOVEATION_LEVEL_REFERENCE_MNDX = 0,
    XR_FOVEATION_LEVEL_STRONG_MNDX = 1,
    XR_FOVEATION_LEVEL_AGGRESSIVE_MNDX = 2,
    XR_FOVEATION_LEVEL_AGGRESSIVE_PLUS_MNDX = 3,
    XR_FOVEATION_LEVEL_NEAR_EXTREME_MNDX = 4,
    XR_FOVEATION_LEVEL_EXTREME_MNDX = 5,
    XR_FOVEATION_LEVEL_MAX_ENUM_MNDX = 0x7fffffff
} XrFoveationLevelMNDX;

typedef struct XrFoveationProfileMNDX {
    XrStructureType       type;
    void* XR_MAY_ALIAS    next;
    XrFoveationLevelMNDX level;
    // Per-axis normalized rasterization rates. 1.0 means full rate.
    float                 centerRate;
    float                 middleRate;
    float                 peripheralRate;
    // Per-axis normalized half-extents measured from the foveation centre.
    float                 centerHalfExtent;
    float                 middleHalfExtent;
} XrFoveationProfileMNDX;

// XrCompositionLayerFoveationMapMNDX extends XrCompositionLayerProjectionView.
typedef struct XrCompositionLayerFoveationMapMNDX {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    uint32_t                    boundaryCount;
    float                       x[XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT];
    float                       y[XR_MNDX_FOVEATION_MAP_BOUNDARY_COUNT];
} XrCompositionLayerFoveationMapMNDX;

typedef XrResult (XRAPI_PTR *PFN_xrGetFoveationProfileMNDX)(
    XrInstance instance,
    XrSystemId systemId,
    XrFoveationLevelMNDX level,
    XrFoveationProfileMNDX* profile);

#ifdef __cplusplus
}
#endif

#endif
