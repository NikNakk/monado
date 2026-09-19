#pragma once

/*
 * MinGW's D3D11 headers expose the interfaces used by hello_xr but omit two
 * convenience constructors supplied by Microsoft's Windows SDK helper header.
 * Provide only those value-initialising helpers; no runtime behaviour changes.
 */
#include_next <d3d11_4.h>

#ifdef __cplusplus

struct CD3D11_DEPTH_STENCIL_VIEW_DESC : public D3D11_DEPTH_STENCIL_VIEW_DESC
{
	CD3D11_DEPTH_STENCIL_VIEW_DESC(D3D11_DSV_DIMENSION dimension,
	                               DXGI_FORMAT view_format,
	                               UINT mip_slice,
	                               UINT first_array_slice,
	                               UINT array_size)
	{
		Format = view_format;
		ViewDimension = dimension;
		Flags = 0;
		Texture2DArray.MipSlice = mip_slice;
		Texture2DArray.FirstArraySlice = first_array_slice;
		Texture2DArray.ArraySize = array_size;
	}
};

struct CD3D11_VIEWPORT : public D3D11_VIEWPORT
{
	CD3D11_VIEWPORT(float top_left_x,
	                float top_left_y,
	                float width,
	                float height,
	                float min_depth = 0.0f,
	                float max_depth = 1.0f)
	{
		TopLeftX = top_left_x;
		TopLeftY = top_left_y;
		Width = width;
		Height = height;
		MinDepth = min_depth;
		MaxDepth = max_depth;
	}
};

#endif
