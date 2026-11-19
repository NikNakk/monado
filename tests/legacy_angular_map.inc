// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Frozen graphics_native.m gaze_build/gaze_rate from macOS-wine-xr 5c52ef88.
// Keep independent of the implementation under test.
static struct
{
	float outer;
} gaze;
static u_foveation_angular_recipe legacy_recipe;
struct mw_gaze_foveation_params
{
	uint32_t width, height;
	XrRect2Di rects[2];
	XrFovf fovs[2];
};
static float
gaze_rate(float delta, float core, float edge, float outer)
{
	float t = (fabsf(delta) - core) / fmaxf(edge - core, .01f);
	t = fminf(1.f, fmaxf(0.f, t));
	t = t * t * (3.f - 2.f * t);
	return 1.f + (outer - 1.f) * t;
}
static id<MTLRasterizationRateMap>
legacy_build(
    id<MTLDevice> device, const struct mw_gaze_foveation_params *p, float yaw, float pitch, float core, float edge)
{
	const unsigned columns = 64, rows = 32;
	const float deg = 57.29577951308232f;
	MTLRasterizationRateLayerDescriptor *layer =
	    [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(columns, rows, 1)];
	for (unsigned i = 0; i < columns; ++i) {
		float x = (i + .5f) / columns * p->width, rate = gaze.outer;
		for (unsigned v = 0; v < 2; ++v) {
			const XrRect2Di *r = &p->rects[v];
			if (x < r->offset.x || x >= r->offset.x + r->extent.width)
				continue;
			float l = tanf(p->fovs[v].angleLeft), rt = tanf(p->fovs[v].angleRight);
			float angle = atanf(l + (x - r->offset.x) / r->extent.width * (rt - l)) * deg;
			rate = fmaxf(rate, gaze_rate(angle - yaw, core, edge, gaze.outer));
		}
		layer.horizontal[i] = @(rate);
		legacy_recipe.horizontal[i] = rate;
	}
	for (unsigned j = 0; j < rows; ++j) {
		float y = (j + .5f) / rows * p->height, rate = gaze.outer;
		for (unsigned v = 0; v < 2; ++v) {
			const XrRect2Di *r = &p->rects[v];
			if (y < r->offset.y || y >= r->offset.y + r->extent.height)
				continue;
			float u = tanf(p->fovs[v].angleUp), d = tanf(p->fovs[v].angleDown);
			float angle = atanf(u - (y - r->offset.y) / r->extent.height * (u - d)) * deg;
			rate = fmaxf(rate, gaze_rate(angle - pitch, core, edge, gaze.outer));
		}
		layer.vertical[j] = @(rate);
		legacy_recipe.vertical[j] = rate;
	}
	MTLRasterizationRateMapDescriptor *desc = [MTLRasterizationRateMapDescriptor
	    rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(p->width, p->height, 1)
	                                           layer:layer];
	id<MTLRasterizationRateMap> map = [device newRasterizationRateMapWithDescriptor:desc];
	[layer release];
	return map;
}
