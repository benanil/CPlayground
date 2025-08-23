#ifndef _GRAPHICS_
#define _GRAPHICS_

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBIR_MALLOC(size, user_data) malloc(size)
#define STBIR_FREE(ptr) free(ptr)

//#include "Extern/sokol/sokol_gfx.h"
#include "Extern/stb/stb_image_resize2.h"

#include <stdint.h>
// #include "Math/Math.h"

typedef sg_image Texture;

static const uint8_t TextureTypeToBytesPerPixelMap[_SG_PIXELFORMAT_NUM] =
{
    [_SG_PIXELFORMAT_DEFAULT]        = 0,
    [SG_PIXELFORMAT_NONE]            = 0,
    [SG_PIXELFORMAT_R8]              = 1,
    [SG_PIXELFORMAT_R8SN]            = 1,
    [SG_PIXELFORMAT_R8UI]            = 1,
    [SG_PIXELFORMAT_R8SI]            = 1,
    [SG_PIXELFORMAT_R16]             = 2,
    [SG_PIXELFORMAT_R16SN]           = 2,
    [SG_PIXELFORMAT_R16UI]           = 2,
    [SG_PIXELFORMAT_R16SI]           = 2,
    [SG_PIXELFORMAT_R16F]            = 2,
    [SG_PIXELFORMAT_RG8]             = 2,
    [SG_PIXELFORMAT_RG8SN]           = 2,
    [SG_PIXELFORMAT_RG8UI]           = 2,
    [SG_PIXELFORMAT_RG8SI]           = 2,
    [SG_PIXELFORMAT_R32UI]           = 4,
    [SG_PIXELFORMAT_R32SI]           = 4,
    [SG_PIXELFORMAT_R32F]            = 4,
    [SG_PIXELFORMAT_RG16]            = 4,
    [SG_PIXELFORMAT_RG16SN]          = 4,
    [SG_PIXELFORMAT_RG16UI]          = 4,
    [SG_PIXELFORMAT_RG16SI]          = 4,
    [SG_PIXELFORMAT_RG16F]           = 4,
    [SG_PIXELFORMAT_RGBA8]           = 4,
    [SG_PIXELFORMAT_SRGB8A8]         = 4,
    [SG_PIXELFORMAT_RGBA8SN]         = 4,
    [SG_PIXELFORMAT_RGBA8UI]         = 4,
    [SG_PIXELFORMAT_RGBA8SI]         = 4,
    [SG_PIXELFORMAT_BGRA8]           = 4,
    [SG_PIXELFORMAT_RGB10A2]         = 4,
    [SG_PIXELFORMAT_RG11B10F]        = 4,
    [SG_PIXELFORMAT_RGB9E5]          = 4,
    [SG_PIXELFORMAT_RG32UI]          = 8,
    [SG_PIXELFORMAT_RG32SI]          = 8,
    [SG_PIXELFORMAT_RG32F]           = 8,
    [SG_PIXELFORMAT_RGBA16]          = 8,
    [SG_PIXELFORMAT_RGBA16SN]        = 8,
    [SG_PIXELFORMAT_RGBA16UI]        = 8,
    [SG_PIXELFORMAT_RGBA16SI]        = 8,
    [SG_PIXELFORMAT_RGBA16F]         = 8,
    [SG_PIXELFORMAT_RGBA32UI]        = 16,
    [SG_PIXELFORMAT_RGBA32SI]        = 16,
    [SG_PIXELFORMAT_RGBA32F]         = 16,
    [SG_PIXELFORMAT_DEPTH]           = 1,
    [SG_PIXELFORMAT_DEPTH_STENCIL]   = 1,
    [SG_PIXELFORMAT_BC1_RGBA]        = 1,
    [SG_PIXELFORMAT_BC2_RGBA]        = 1,
    [SG_PIXELFORMAT_BC3_RGBA]        = 1,
    [SG_PIXELFORMAT_BC3_SRGBA]       = 1,
    [SG_PIXELFORMAT_BC4_R]           = 1,
    [SG_PIXELFORMAT_BC4_RSN]         = 1,
    [SG_PIXELFORMAT_BC5_RG]          = 1,
    [SG_PIXELFORMAT_BC5_RGSN]        = 1,
    [SG_PIXELFORMAT_BC6H_RGBF]       = 1,
    [SG_PIXELFORMAT_BC6H_RGBUF]      = 1,
    [SG_PIXELFORMAT_BC7_RGBA]        = 1,
    [SG_PIXELFORMAT_BC7_SRGBA]       = 1,
    [SG_PIXELFORMAT_ETC2_RGB8]       = 1,
    [SG_PIXELFORMAT_ETC2_SRGB8]      = 1,
    [SG_PIXELFORMAT_ETC2_RGB8A1]     = 1,
    [SG_PIXELFORMAT_ETC2_RGBA8]      = 1,
    [SG_PIXELFORMAT_ETC2_SRGB8A8]    = 1,
    [SG_PIXELFORMAT_EAC_R11]         = 1,
    [SG_PIXELFORMAT_EAC_R11SN]       = 1,
    [SG_PIXELFORMAT_EAC_RG11]        = 1,
    [SG_PIXELFORMAT_EAC_RG11SN]      = 1,
    [SG_PIXELFORMAT_ASTC_4x4_RGBA]   = 1,
    [SG_PIXELFORMAT_ASTC_4x4_SRGBA]  = 1,
};

uint8_t rTextureTypeToBytesPerPixel(sg_pixel_format type)
{
    return TextureTypeToBytesPerPixelMap[type];
}

void rInit()
{
}

int rGetMipmapImageData(sg_image_data* img_data, void* data, int width, int height)
{
    int numMips = (int)Log2_32((unsigned)width) >> 1; 
    int size = width * height * 4;
    numMips = MMAX(numMips, 1) - 1;
    numMips = MMIN(numMips, SG_MAX_MIPMAPS);

    for (int i = 0; i < numMips; i++)
    {
        img_data->subimage[0][i].ptr = data;
        img_data->subimage[0][i].size = size;
        if (i != numMips - 1)
            data = stbir_resize(data, width, height, width * 4, 
                                NULL, width >> 1, height >> 1, (width >> 1) * 4, 
                                STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL);
        width >>= 1;
        height >>= 1;
        size = width * height * 4;
    }
    return numMips;
}



#endif