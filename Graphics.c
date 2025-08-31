#ifndef _GRAPHICS_
#define _GRAPHICS_


//#include "Extern/sokol/sokol_gfx.h"
#include "Extern/stb/stb_image_resize2.h"
#include "Common.h"
#include "Platform.h"
#include "Graphics.h"
#include "IO.h"

#include <stdint.h>
// #include "Math/Math.h"


unsigned char* g_TextureLoadBuffer = NULL;
uint64_t g_TextureLoadBufferSize = 0;

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


int GraphicsTypeToSize(GraphicType type)
{
    // BYTE, UNSIGNED_BYTE, SHORT, UNSIGNED_SHORT, INT, UNSIGNED_INT, FLOAT           
    const int TypeToSize[12] = { 1, 1, 2, 2, 4, 4, 4, 2, 4, 4, 8, 2 };
    return TypeToSize[type];
}

uint8_t rTextureTypeToBytesPerPixel(sg_pixel_format type)
{
    return TextureTypeToBytesPerPixelMap[type];
}

uint64_t rTextureSizeBytes(Texture texture)
{
    return texture.width * texture.height * TextureTypeToBytesPerPixelMap[texture.format];
}

void rInit()
{
    g_TextureLoadBuffer = rpmalloc(g_TextureLoadBufferSize);
    g_TextureLoadBufferSize = 1024 * 1024 * 4;
}

void rDestroy()
{
    rpfree(g_TextureLoadBuffer);
}

int rGetMipmapImageData(sg_image_data* img_data, void* data, int width, int height)
{
    int numMips = (int)Log2_32((unsigned)width) >> 1; 
    int size = width * height * 4;
    numMips = MMAX(numMips, 1) - 1;
    numMips = MMIN(numMips, SG_MAX_MIPMAPS);

    unsigned long bufferSize = 0;
    for (int i = 2; i < numMips; i++)
    {
        bufferSize += (width / i) * (height / i);
    }
    uint8_t* buffer = rpmalloc(bufferSize);

    for (int i = 0; i < numMips; i++)
    {
        img_data->subimage[0][i].ptr = data;
        img_data->subimage[0][i].size = size;
        if (i != numMips - 1)
            data = stbir_resize(data, width, height, width * 4, 
                                buffer, width >> 1, height >> 1, (width >> 1) * 4, 
                                STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL);
        width >>= 1;
        height >>= 1;
        size = width * height * 4;
        buffer += size;
    }
    return numMips;
}


static bool IsCompressed(const char* path, int pathLen)
{
    #ifndef __ANDROID__ 
    return path[pathLen-1] == 't' && path[pathLen-2] == 'x' && path[pathLen-3] == 'd';
    #else
    return path[pathLen-1] == 'c' && path[pathLen-2] == 't' && path[pathLen-3] == 's' && path[pathLen-4] == 'a';
    #endif
}


static void rResizeTextureLoadBufferIfNecessarry(unsigned long long size)
{
    if (g_TextureLoadBufferSize < size)
    {
        g_TextureLoadBuffer = rprealloc(g_TextureLoadBuffer, size);
    }
}

Texture rImportTexture(const char* path, TexFlags flags, const char* label)
{
    int width, height, channels;
    unsigned char* image = NULL;
    Texture defTexture;
    defTexture.width  = 32;
    defTexture.height = 32;
    defTexture.handle = (sg_image){ .id = 0};

    if (!FileExist(path)) {
        AX_ERROR("image is not exist, using default texture! %s", path);
        return defTexture;
    }

    AFile asset = AFileOpen(path, AOpenFlag_ReadBinary);
    uint64_t size = AFileSize(asset);

    rResizeTextureLoadBufferIfNecessarry(size);
    
    int compressed = IsCompressed(path, StringLength(path));
    if (compressed) {
        ASSERT(0 && "not implemented");
    }
    
    AFileRead(g_TextureLoadBuffer, size, asset, 1);
    image = stbi_load_from_memory(g_TextureLoadBuffer, (int)size, &width, &height, &channels, 4);
    
    AFileClose(asset);
    
    if (image == NULL) {
        AX_ERROR("image load failed! %s", path);
        return defTexture;
    }

    const sg_pixel_format numCompToFormat[5] = { 0, SG_PIXELFORMAT_R8, SG_PIXELFORMAT_RG8, SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_RGBA8 };
    
    Texture texture = rCreateTexture(width, height, image, numCompToFormat[channels], flags, label); 
    
    bool delBuff = (flags & TexFlags_DontDeleteCPUBuffer) == 0;
    if (!compressed && delBuff)
    {
        stbi_image_free(image);
        texture.buffer = NULL;
    }
    return texture;
}

void rDeleteMipData(sg_image_data* img_data, int numMips)
{
    rpfree((void*)img_data->subimage[0][1].ptr);
}

Texture rCreateTexture(int width, int height, void* data, sg_pixel_format format, TexFlags flags, const char* label)
{
    sg_image_desc imageDesc = {
        .pixel_format = format,
        .width = width,
        .height = height,
        .label = label
    };
    
    imageDesc.data.subimage[0][0] = (sg_range){ data, width * height * rTextureTypeToBytesPerPixel(format) };
    
    if (!!(flags & TexFlags_MipMap))
        imageDesc.num_mipmaps = rGetMipmapImageData(&imageDesc.data, data, imageDesc.width, imageDesc.height);

    if (!!(flags & (TexFlags_DynamicUpdate | TexFlags_StreamUpdate)))
        imageDesc.usage.immutable = false;

    imageDesc.usage.dynamic_update    = !!(flags & TexFlags_DynamicUpdate);
    imageDesc.usage.stream_update     = !!(flags & TexFlags_StreamUpdate);
    imageDesc.usage.render_attachment = !!(flags & TexFlags_RenderAttachment);

    Texture texture = {
        .width = width,
        .width = height,
        .format = format,
        .handle = sg_make_image(&imageDesc),
        .buffer = data
    };
    
    if ((flags & TexFlags_MipMap))
        rDeleteMipData(&imageDesc.data, imageDesc.num_mipmaps);

    return texture;
}

void rUpdateTexture(Texture texture, void* data)
{
    sg_image_data imageData = {0};
    imageData.subimage[0][0] = (sg_range){ data, rTextureSizeBytes(texture) };
    sg_update_image(texture.handle, &imageData);
}

void rDeleteTexture(Texture texture)
{
    sg_destroy_image(texture.handle);
}


#endif