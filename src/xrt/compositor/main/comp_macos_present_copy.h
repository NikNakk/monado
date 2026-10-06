// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Final copy of a presented image into a CAMetalLayer drawable.
 *
 * The macOS presenter ends every frame by copying a finished image into the
 * drawable. Its own target images match the drawable, so that is a blit. An
 * image composited elsewhere may differ in format (for example RGBA from a
 * D3D11 producer, where the drawable is BGRA) or size, and is then drawn.
 * Separate from the presenter so it can be tested without a display.
 * @ingroup comp_main
 */

#pragma once

#import <Metal/Metal.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_macos_present_copy;

//! NULL on allocation failure.
struct comp_macos_present_copy *
comp_macos_present_copy_create(id<MTLDevice> device);

void
comp_macos_present_copy_destroy(struct comp_macos_present_copy **pc_ptr);

/*!
 * Encode a copy of the whole of @p src into the whole of @p dst.
 *
 * Identical formats and sizes are blitted. Otherwise the image is drawn with
 * linear filtering, which converts channel order. An sRGB source written to a
 * non-sRGB destination keeps its encoded values, so either kind of source
 * reaches the display unchanged. @p dst needs MTLTextureUsageRenderTarget for
 * the draw, as drawables have.
 */
bool
comp_macos_present_copy_encode(struct comp_macos_present_copy *pc,
                               id<MTLCommandBuffer> command_buffer,
                               id<MTLTexture> src,
                               id<MTLTexture> dst);

//! True when encode would draw rather than blit; for tests and logging.
bool
comp_macos_present_copy_needs_draw(id<MTLTexture> src, id<MTLTexture> dst);

#ifdef __cplusplus
}
#endif
