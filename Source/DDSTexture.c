#include "Include/DDSTexture.h"

#include "Include/ParallelFor.h"
#include "Include/Platform.h"
#include "Include/Memory.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#ifndef BASISU_NOTE_UNUSED
    #define BASISU_NOTE_UNUSED(x) (void)(x)
#endif

#define TinyDDS_ArraySlices AXTinyDDS_ArraySlices
#define TinyDDS_CreateContext AXTinyDDS_CreateContext
#define TinyDDS_Depth AXTinyDDS_Depth
#define TinyDDS_DestroyContext AXTinyDDS_DestroyContext
#define TinyDDS_Dimensions AXTinyDDS_Dimensions
#define TinyDDS_FaceSize AXTinyDDS_FaceSize
#define TinyDDS_GetFormat AXTinyDDS_GetFormat
#define TinyDDS_Height AXTinyDDS_Height
#define TinyDDS_ImageRawData AXTinyDDS_ImageRawData
#define TinyDDS_ImageSize AXTinyDDS_ImageSize
#define TinyDDS_Is1D AXTinyDDS_Is1D
#define TinyDDS_Is2D AXTinyDDS_Is2D
#define TinyDDS_Is3D AXTinyDDS_Is3D
#define TinyDDS_IsArray AXTinyDDS_IsArray
#define TinyDDS_IsCubemap AXTinyDDS_IsCubemap
#define TinyDDS_NeedsEndianCorrecting AXTinyDDS_NeedsEndianCorrecting
#define TinyDDS_NeedsGenerationOfMipmaps AXTinyDDS_NeedsGenerationOfMipmaps
#define TinyDDS_NumberOfMipmaps AXTinyDDS_NumberOfMipmaps
#define TinyDDS_ReadHeader AXTinyDDS_ReadHeader
#define TinyDDS_Reset AXTinyDDS_Reset
#define TinyDDS_Width AXTinyDDS_Width
#define TinyDDS_WriteImage AXTinyDDS_WriteImage

#define TINYDDS_IMPLEMENTATION
#include "Extern/basis_universal/encoder/3rdparty/tinydds.h"

typedef enum DDSBlockDecodeFormat_
{
    DDS_BLOCK_DECODE_BC1,
    DDS_BLOCK_DECODE_BC3,
    DDS_BLOCK_DECODE_BC4,
    DDS_BLOCK_DECODE_BC5,
} DDSBlockDecodeFormat;

typedef struct DDSColor_
{
    u8 r, g, b, a;
} DDSColor;

typedef struct DDSUncompressedDecodeTask_
{
    TinyDDS_Format format;
    const u8* src;
    u8* dst;
    u32 width;
    u32 bytesPerPixel;
} DDSUncompressedDecodeTask;

typedef struct DDSCompressedDecodeTask_
{
    DDSBlockDecodeFormat decodeFormat;
    const u8* src;
    u8* dst;
    u32 width;
    u32 height;
    u32 blocksX;
    u32 blockBytes;
    u32 rowPitch;
} DDSCompressedDecodeTask;

static u16 DDSReadLE16(const u8* ptr) {
    return (u16)(ptr[0] | ((u16)ptr[1] << 8));
}

static u32 DDSReadLE32(const u8* ptr) {
    return (u32)ptr[0] | ((u32)ptr[1] << 8) | ((u32)ptr[2] << 16) | ((u32)ptr[3] << 24);
}

static u8 DDSExpand5(u32 value) {
    return (u8)((value << 3) | (value >> 2));
}

static u8 DDSExpand6(u32 value) {
    return (u8)((value << 2) | (value >> 4));
}

static bool DDSIsBC1(TinyDDS_Format format) {
    return format == TDDS_BC1_RGBA_UNORM_BLOCK || format == TDDS_BC1_RGBA_SRGB_BLOCK ||
           (int)format == TIF_DXGI_FORMAT_BC1_TYPELESS;
}

static bool DDSIsBC3(TinyDDS_Format format)
{
    return format == TDDS_BC3_UNORM_BLOCK || format == TDDS_BC3_SRGB_BLOCK ||
           (int)format == TIF_DXGI_FORMAT_BC3_TYPELESS;
}

static bool DDSIsBC4(TinyDDS_Format format)
{
    return format == TDDS_BC4_UNORM_BLOCK || format == TDDS_BC4_SNORM_BLOCK ||
           (int)format == TIF_DXGI_FORMAT_BC4_TYPELESS;
}

static bool DDSIsBC5(TinyDDS_Format format) {
    return format == TDDS_BC5_UNORM_BLOCK || format == TDDS_BC5_SNORM_BLOCK ||
           (int)format == TIF_DXGI_FORMAT_BC5_TYPELESS;
}

static bool DDSIsCompressed(TinyDDS_Format format) {
    return DDSIsBC1(format) || DDSIsBC3(format) || DDSIsBC4(format) || DDSIsBC5(format);
}

static void DDSDecodeBC1Block(const u8* block, DDSColor* pixels)
{
    const u16 c0 = DDSReadLE16(block + 0);
    const u16 c1 = DDSReadLE16(block + 2);
    DDSColor colors[4];

    colors[0].r = DDSExpand5((c0 >> 11) & 31);
    colors[0].g = DDSExpand6((c0 >>  5) & 63);
    colors[0].b = DDSExpand5( c0        & 31);
    colors[0].a = 255;

    colors[1].r = DDSExpand5((c1 >> 11) & 31);
    colors[1].g = DDSExpand6((c1 >>  5) & 63);
    colors[1].b = DDSExpand5( c1        & 31);
    colors[1].a = 255;

    if (c0 > c1) {
        colors[2].r = (u8)((2u * colors[0].r + colors[1].r) / 3u);
        colors[2].g = (u8)((2u * colors[0].g + colors[1].g) / 3u);
        colors[2].b = (u8)((2u * colors[0].b + colors[1].b) / 3u);
        colors[2].a = 255;

        colors[3].r = (u8)((colors[0].r + 2u * colors[1].r) / 3u);
        colors[3].g = (u8)((colors[0].g + 2u * colors[1].g) / 3u);
        colors[3].b = (u8)((colors[0].b + 2u * colors[1].b) / 3u);
        colors[3].a = 255;
    }
    else {
        colors[2].r = (u8)((colors[0].r + colors[1].r) / 2u);
        colors[2].g = (u8)((colors[0].g + colors[1].g) / 2u);
        colors[2].b = (u8)((colors[0].b + colors[1].b) / 2u);
        colors[2].a = 255;
        colors[3].r = 0;
        colors[3].g = 0;
        colors[3].b = 0;
        colors[3].a = 0;
    }

    const u32 indices = DDSReadLE32(block + 4);
    for (u32 i = 0; i < 16; i++) {
        pixels[i] = colors[(indices >> (i * 2)) & 3u];
    }
}

static void DDSDecodeBC4Values(const u8* block, u8* values)
{
    const u8 a0 = block[0];
    const u8 a1 = block[1];
    u8 palette[8];
    u64 selectors = 0;

    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        palette[2] = (u8)((6u * a0 + 1u * a1) / 7u);
        palette[3] = (u8)((5u * a0 + 2u * a1) / 7u);
        palette[4] = (u8)((4u * a0 + 3u * a1) / 7u);
        palette[5] = (u8)((3u * a0 + 4u * a1) / 7u);
        palette[6] = (u8)((2u * a0 + 5u * a1) / 7u);
        palette[7] = (u8)((1u * a0 + 6u * a1) / 7u);
    }
    else {
        palette[2] = (u8)((4u * a0 + 1u * a1) / 5u);
        palette[3] = (u8)((3u * a0 + 2u * a1) / 5u);
        palette[4] = (u8)((2u * a0 + 3u * a1) / 5u);
        palette[5] = (u8)((1u * a0 + 4u * a1) / 5u);
        palette[6] = 0;
        palette[7] = 255;
    }

    for (u32 i = 0; i < 6; i++) {
        selectors |= (u64)block[2 + i] << (i * 8);
    }

    for (u32 i = 0; i < 16; i++) {
        values[i] = palette[(selectors >> (i * 3)) & 7u];
    }
}

static void DDSDecodeBC3Block(const u8* block, DDSColor* pixels)
{
    u8 alpha[16];
    DDSDecodeBC1Block(block + 8, pixels);
    DDSDecodeBC4Values(block, alpha);
    for (u32 i = 0; i < 16; i++) {
        pixels[i].a = alpha[i];
    }
}

static void DDSDecodeBC4Block(const u8* block, DDSColor* pixels)
{
    u8 red[16];
    DDSDecodeBC4Values(block, red);
    for (u32 i = 0; i < 16; i++) {
        pixels[i].r = red[i];
        pixels[i].g = red[i];
        pixels[i].b = red[i];
        pixels[i].a = 255;
    }
}

static void DDSDecodeBC5Block(const u8* block, DDSColor* pixels)
{
    u8 red[16];
    u8 green[16];
    DDSDecodeBC4Values(block, red);
    DDSDecodeBC4Values(block + 8, green);
    for (u32 i = 0; i < 16; i++)
    {
        pixels[i].r = red[i];
        pixels[i].g = green[i];
        pixels[i].b = 0;
        pixels[i].a = 255;
    }
}

static void DDSDecodeUncompressedRange(u32 beginY, u32 endY, void* userData)
{
    DDSUncompressedDecodeTask* task = (DDSUncompressedDecodeTask*)userData;

    for (u32 y = beginY; y < endY; y++)
    {
        const u8* in = task->src + (u64)y * task->width * task->bytesPerPixel;
        u8* out = task->dst + (u64)y * task->width * 4u;

        switch (task->format)
        {
            case TDDS_R8G8B8A8_UNORM:
            case TDDS_R8G8B8A8_SRGB:
                SDL_memcpy(out, in, (size_t)task->width * 4u);
                break;

            case TDDS_B8G8R8A8_UNORM:
            case TDDS_B8G8R8A8_SRGB:
                for (u32 x = 0; x < task->width; x++)
                {
                    out[x * 4u + 0u] = in[x * 4u + 2u];
                    out[x * 4u + 1u] = in[x * 4u + 1u];
                    out[x * 4u + 2u] = in[x * 4u + 0u];
                    out[x * 4u + 3u] = in[x * 4u + 3u];
                }
                break;

            case TDDS_B8G8R8X8_UNORM:
                for (u32 x = 0; x < task->width; x++)
                {
                    out[x * 4u + 0u] = in[x * 4u + 2u];
                    out[x * 4u + 1u] = in[x * 4u + 1u];
                    out[x * 4u + 2u] = in[x * 4u + 0u];
                    out[x * 4u + 3u] = 255;
                }
                break;

            case TDDS_R8_UNORM:
                for (u32 x = 0; x < task->width; x++)
                {
                    out[x * 4u + 0u] = in[x];
                    out[x * 4u + 1u] = in[x];
                    out[x * 4u + 2u] = in[x];
                    out[x * 4u + 3u] = 255;
                }
                break;

            case TDDS_R8G8_UNORM:
                for (u32 x = 0; x < task->width; x++)
                {
                    out[x * 4u + 0u] = in[x * 2u + 0u];
                    out[x * 4u + 1u] = in[x * 2u + 1u];
                    out[x * 4u + 2u] = 0;
                    out[x * 4u + 3u] = 255;
                }
                break;

            default:
                break;
        }
    }
}

static bool DDSDecodeUncompressed(TinyDDS_Format format, const u8* src, u32 width, u32 height, u32 size, u8* dst)
{
    u32 bytesPerPixel = 0;
    switch (format)
    {
        case TDDS_R8G8B8A8_UNORM:
        case TDDS_R8G8B8A8_SRGB:
        case TDDS_B8G8R8A8_UNORM:
        case TDDS_B8G8R8A8_SRGB:
        case TDDS_B8G8R8X8_UNORM:
            bytesPerPixel = 4;
            break;
        case TDDS_R8_UNORM: bytesPerPixel = 1; break;
        case TDDS_R8G8_UNORM: bytesPerPixel = 2; break;
        default: return false;
    }

    if ((u64)size < (u64)width * height * bytesPerPixel) 
        return false;

    DDSUncompressedDecodeTask task;
    task.format = format;
    task.src = src;
    task.dst = dst;
    task.width = width;
    task.bytesPerPixel = bytesPerPixel;
    ParallelFor(height, 128, DDSDecodeUncompressedRange, &task);
    return true;
}

static void DDSDecodeCompressedRange(u32 beginBlockY, u32 endBlockY, void* userData)
{
    DDSCompressedDecodeTask* task = (DDSCompressedDecodeTask*)userData;

    for (u32 by = beginBlockY; by < endBlockY; by++)
    {
        for (u32 bx = 0; bx < task->blocksX; bx++)
        {
            const u8* block = task->src + (u64)by * task->rowPitch + (u64)bx * task->blockBytes;
            DDSColor pixels[16];

            switch (task->decodeFormat)
            {
                case DDS_BLOCK_DECODE_BC1: DDSDecodeBC1Block(block, pixels); break;
                case DDS_BLOCK_DECODE_BC3: DDSDecodeBC3Block(block, pixels); break;
                case DDS_BLOCK_DECODE_BC4: DDSDecodeBC4Block(block, pixels); break;
                case DDS_BLOCK_DECODE_BC5: DDSDecodeBC5Block(block, pixels); break;
            }

            for (u32 py = 0; py < 4; py++)
            {
                const u32 y = by * 4u + py;
                if (y >= task->height)  break;

                for (u32 px = 0; px < 4; px++)
                {
                    const u32 x = bx * 4u + px;
                    if (x >= task->width) break;

                    const DDSColor p = pixels[py * 4u + px];
                    u8* out = task->dst + ((u64)y * task->width + x) * 4u;
                    out[0] = p.r;
                    out[1] = p.g;
                    out[2] = p.b;
                    out[3] = p.a;
                }
            }
        }
    }
}

static bool DDSDecodeCompressed(TinyDDS_Format format, const u8* src, u32 width, u32 height, u32 size, u8* dst)
{
    const u32 blockBytes = (DDSIsBC1(format) || DDSIsBC4(format)) ? 8u : 16u;
    const u32 blocksX = (width + 3u) >> 2;
    const u32 blocksY = (height + 3u) >> 2;
    const u32 rowPitch = blocksX * blockBytes;
    DDSBlockDecodeFormat decodeFormat;

    if ((u64)size < (u64)rowPitch * blocksY) 
        return false;

    if      (DDSIsBC1(format)) { decodeFormat = DDS_BLOCK_DECODE_BC1; }
    else if (DDSIsBC3(format)) { decodeFormat = DDS_BLOCK_DECODE_BC3; }
    else if (DDSIsBC4(format)) { decodeFormat = DDS_BLOCK_DECODE_BC4; }
    else if (DDSIsBC5(format)) { decodeFormat = DDS_BLOCK_DECODE_BC5; }
    else { return false; }

    DDSCompressedDecodeTask task;
    task.decodeFormat = decodeFormat;
    task.src = src;
    task.dst = dst;
    task.width = width;
    task.height = height;
    task.blocksX = blocksX;
    task.blockBytes = blockBytes;
    task.rowPitch = rowPitch;
    ParallelFor(blocksY, 16, DDSDecodeCompressedRange, &task);

    return true;
}

static void DDSTinyErrorCallback(void* user, const char* msg) {
    (void)user;
    AX_WARN("tinydds: %s", msg);
}

static void* DDSTinyAllocCallback(void* user, size_t size) {
    (void)user;
    return AllocateTLSFGlobal(size);
}

static void DDSTinyFreeCallback(void* user, void* memory) {
    (void)user;
    DeAllocateTLSFGlobal(memory);
}

static size_t DDSTinyReadCallback(void* user, void* buffer, size_t byteCount) {
    return SDL_ReadIO((SDL_IOStream*)user, buffer, byteCount);
}

static bool DDSTinySeekCallback(void* user, int64_t offset) {
    return SDL_SeekIO((SDL_IOStream*)user, offset, SDL_IO_SEEK_SET) >= 0;
}

static int64_t DDSTinyTellCallback(void* user) {
    return SDL_TellIO((SDL_IOStream*)user);
}

bool DDSLoadDecompressImage(const char* inputFilename, DDSImage* outImage)
{
    if (!inputFilename || !outImage) {
        AX_WARN("DDSLoadImage invalid arguments");
        return false;
    }

    outImage->width = 0;
    outImage->height = 0;
    outImage->pixels = 0;

    SDL_IOStream* file = SDL_IOFromFile(inputFilename, "rb");
    if (!file) {
        AX_WARN("can't open DDS file %s", inputFilename);
        return false;
    }

    TinyDDS_Callbacks callbacks;
    callbacks.errorFn = DDSTinyErrorCallback;
    callbacks.allocFn = DDSTinyAllocCallback;
    callbacks.freeFn = DDSTinyFreeCallback;
    callbacks.readFn = DDSTinyReadCallback;
    callbacks.seekFn = DDSTinySeekCallback;
    callbacks.tellFn = DDSTinyTellCallback;

    TinyDDS_ContextHandle dds = TinyDDS_CreateContext(&callbacks, file);
    if (!dds) {
        AX_WARN("DDS context creation failed: %s", inputFilename);
        SDL_CloseIO(file);
        return false;
    }

    if (!TinyDDS_ReadHeader(dds)) {
        AX_WARN("failed parsing DDS header: %s", inputFilename);
        TinyDDS_DestroyContext(dds);
        SDL_CloseIO(file);
        return false;
    }

    if (!TinyDDS_Is2D(dds) || TinyDDS_ArraySlices(dds) > 1 || TinyDDS_IsCubemap(dds)) {
        AX_WARN("DDS arrays, cubemaps, and 3D textures are not supported: %s", inputFilename);
        TinyDDS_DestroyContext(dds);
        SDL_CloseIO(file);
        return false;
    }

    const u32 width = TinyDDS_Width(dds);
    const u32 height = TinyDDS_Height(dds);
    const u8* src = (const u8*)TinyDDS_ImageRawData(dds, 0);
    const u32 size = TinyDDS_ImageSize(dds, 0);
    const TinyDDS_Format format = TinyDDS_GetFormat(dds);

    if (!width || !height || !src || !size) {
        AX_WARN("DDS has no usable base image: %s", inputFilename);
        TinyDDS_DestroyContext(dds);
        SDL_CloseIO(file);
        return false;
    }

    const u64 decodedSize = (u64)width * height * 4u;
    u8* decoded = (u8*)AllocateTLSFGlobal((size_t)decodedSize);
    if (!decoded) {
        AX_WARN("DDS allocation failed: %s", inputFilename);
        TinyDDS_DestroyContext(dds);
        SDL_CloseIO(file);
        return false;
    }

    bool ok;
    if (DDSIsCompressed(format)) {
        ok = DDSDecodeCompressed(format, src, width, height, size, decoded);
    }
    else {
        ok = DDSDecodeUncompressed(format, src, width, height, size, decoded);
    }

    if (!ok) {
        AX_WARN("unsupported DDS format %d: %s", (int)format, inputFilename);
        DeAllocateTLSFGlobal(decoded);
        TinyDDS_DestroyContext(dds);
        SDL_CloseIO(file);
        return false;
    }

    outImage->width = width;
    outImage->height = height;
    outImage->pixels = decoded;

    TinyDDS_DestroyContext(dds);
    SDL_CloseIO(file);
    return true;
}

void DDSFreeImage(DDSImage* image)
{
    if (!image) return;

    DeAllocateTLSFGlobal(image->pixels);
    image->width = 0;
    image->height = 0;
    image->pixels = 0;
}
