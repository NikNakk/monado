// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental access to the headset's display layout and lens
 *        distortion.
 *
 * For applications that produce the final, panel-ready image themselves, for
 * example a SteamVR driver whose compositor distorts with the driver's
 * distortion function. Applications that submit ordinary composition layers
 * do not need this: the runtime distorts for them.
 */

#ifndef XR_MNDX_DISPLAY_DISTORTION_H
#define XR_MNDX_DISPLAY_DISTORTION_H 1

#include "openxr_extension_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MNDX_display_distortion 1
#define XR_MNDX_display_distortion_SPEC_VERSION 1
#define XR_MNDX_DISPLAY_DISTORTION_EXTENSION_NAME "XR_MNDX_display_distortion"
#define XR_MNDX_DISPLAY_DISTORTION_MAX_VIEWS 2

XR_STRUCT_ENUM(XR_TYPE_DISPLAY_DISTORTION_PROPERTIES_MNDX, 0x7fff5060);
XR_STRUCT_ENUM(XR_TYPE_COMPOSITION_LAYER_DISPLAY_IMAGE_MNDX, 0x7fff5061);

/*!
 * One view of the display.
 *
 * viewport is the view's region of the display, in display pixels with the
 * origin at the top left. fov is the field of view of the image the
 * distortion samples from: an image rendered with this fov covers source UVs
 * 0..1.
 */
typedef struct XrDisplayDistortionViewMNDX {
    XrRect2Di    viewport;
    XrFovf       fov;
} XrDisplayDistortionViewMNDX;

typedef struct XrDisplayDistortionPropertiesMNDX {
    XrStructureType                type;
    void* XR_MAY_ALIAS             next;
    XrExtent2Di                    displaySize;
    float                          nominalRefreshRate;
    uint32_t                       viewCount;
    XrDisplayDistortionViewMNDX    views[XR_MNDX_DISPLAY_DISTORTION_MAX_VIEWS];
} XrDisplayDistortionPropertiesMNDX;

/*!
 * Chained to an XrCompositionLayerProjection whose image is already the final
 * display image, distorted with xrComputeDisplayDistortionMNDX. The runtime
 * presents it as it is, without distortion or reprojection, so the views'
 * poses and FOVs only describe it.
 *
 * - The layer must be the frame's only layer.
 * - All views must use the same image of one swapchain, created with
 *   XR_SWAPCHAIN_USAGE_SAMPLED_BIT and XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT,
 *   at the display size. Each view's imageRect should be its viewport.
 * - The values are sent to the display as stored. Use a UNORM format holding
 *   display-encoded values; an sRGB format may be decoded on the way.
 *
 * Runtimes that cannot present the image directly copy it to the display, and
 * render it as an ordinary projection layer when they cannot do that either
 * (for example when other layers are present).
 */
typedef struct XrCompositionLayerDisplayImageMNDX {
    XrStructureType       type;
    const void* XR_MAY_ALIAS    next;
} XrCompositionLayerDisplayImageMNDX;

/*!
 * Properties of the system's head-mounted display. Returns
 * XR_ERROR_FEATURE_UNSUPPORTED when the system has no display distortion
 * that this extension can describe, for example rotated views.
 */
typedef XrResult(XRAPI_PTR *PFN_xrGetDisplayDistortionPropertiesMNDX)(
    XrInstance instance, XrSystemId systemId, XrDisplayDistortionPropertiesMNDX *properties);

/*!
 * Evaluates the lens distortion of one view at pointCount points.
 *
 * Each points[i] is a position in the view's viewport, 0..1 with the origin
 * at the top left. red/green/blue[i] receive the source UV (0..1, origin at
 * the top left, in an image rendered with the view's fov) whose colour channel
 * should be shown there. This is the same mapping as SteamVR's
 * IVRDisplayComponent::ComputeDistortion.
 */
typedef XrResult(XRAPI_PTR *PFN_xrComputeDisplayDistortionMNDX)(XrInstance instance,
                                                               XrSystemId systemId,
                                                               uint32_t viewIndex,
                                                               uint32_t pointCount,
                                                               const XrVector2f *points,
                                                               XrVector2f *red,
                                                               XrVector2f *green,
                                                               XrVector2f *blue);

#ifndef XR_NO_PROTOTYPES
#ifdef XR_EXTENSION_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL
xrGetDisplayDistortionPropertiesMNDX(XrInstance instance,
                                     XrSystemId systemId,
                                     XrDisplayDistortionPropertiesMNDX *properties);

XRAPI_ATTR XrResult XRAPI_CALL
xrComputeDisplayDistortionMNDX(XrInstance instance,
                               XrSystemId systemId,
                               uint32_t viewIndex,
                               uint32_t pointCount,
                               const XrVector2f *points,
                               XrVector2f *red,
                               XrVector2f *green,
                               XrVector2f *blue);
#endif /* XR_EXTENSION_PROTOTYPES */
#endif /* !XR_NO_PROTOTYPES */

#ifdef __cplusplus
}
#endif

#endif
