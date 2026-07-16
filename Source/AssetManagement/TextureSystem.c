#include "Include/TextureSystem.h"
#include "Include/BasisBinding.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/FileSystem.h"
#include "Include/Async.h"
#include "Include/Bitset.h"
#include "Math/Half.h"
#include "Math/Vector.h"

#define SAAlloc(size) AllocateTLSFGlobal(size)
#define SARealloc(mem, size) ReAllocateTLSFGlobal(mem, size)
#define SAFree(mem) DeAllocateTLSFGlobal(mem)
#define SAAssert(cond) ASSERT(cond)
#define SA_SMOL_ATLAS_IMPLEMENTATION
#include "Extern/SmolAtlas.h"

#if defined(PLATFORM_MACOSX)
#include "Shaders/msl/TexturePageCopyRGBA.msl.h"
#include "Shaders/msl/TexturePageCopyRG.msl.h"

#define Shaders_TexturePageCopyRGBA_spv Shaders_TexturePageCopyRGBA_msl
#define Shaders_TexturePageCopyRG_spv Shaders_TexturePageCopyRG_msl
#elif defined(PLATFORM_WINDOWS)
#include "Shaders/spv/TexturePageCopyRGBA.spv.h"
#include "Shaders/spv/TexturePageCopyRG.spv.h"
#endif

enum { TextureType_Normal = 1u << 0, TextureType_MetallicRoughness = 1u << 1 };
enum { TextureDesc_Invalid = 0u, TextureDesc_Albedo = 1u, TextureDesc_Normal = 2u, TextureDesc_MetallicRoughness = 3u };
enum { TextureDesc_DefaultAlbedo = 1u, TextureDesc_DefaultNormal = 2u, TextureDesc_DefaultMetallicRoughness = 4u };
enum { TextureDesc_DefaultCount = 4u };

enum { CompressedPageAlign = TEXTURE_PAGE_ALIGN, CompressedDirectMips = TEXTURE_PAGE_DIRECT_MIPS };
enum { CompressedPageMaxMips = TEXTURE_PAGE_MAX_MIPS };
enum { TextureDefaultSize = 16u };

typedef struct TextureClassInfo_
{
    const char* label;
    SDL_GPUTextureFormat uncompressedFormat;
    u32 defaultDescriptor;
    u32 defaultFlag;
    u32 channelMode;
} TextureClassInfo;

typedef struct TexturePlacement_
{
    u32 imageIndex;
    u32 page;
    u32 x, y;
    u32 width, height;
    u32 padding;
    SmolAtlasItem* item;
} TexturePlacement;

typedef struct PageCopyRequest_
{
    u32 textureClass;
    u32 sourceIndex;
    u32 page;
    u32 x, y;
    u32 width, height;
    u32 padding;
} PageCopyRequest;

extern RenderState g_RenderState;
extern SDL_GPUDevice* g_GPUDevice;

static SDL_GPUComputePipeline* gCopyRGBAPipeline;
static SDL_GPUComputePipeline* gCopyRGPipeline;

// transient queue, only lives inside one TextureSystem_AppendBundle call
static PageCopyRequest gCopyRequests[MAX_TEXTURE_DESCRIPTORS];
static u32 gNumCopyRequests;

static u32 MipCountForSize(u32 width, u32 height)
{
    u32 levels = 1;
    u32 dim = width > height ? width : height;
    while (dim > 1)
    {
        dim >>= 1;
        levels++;
    }
    return levels;
}

static u32 AlignUpu32(u32 value, u32 align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static bool IsBlockCompressedFormat(SDL_GPUTextureFormat format)
{
    return format == SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM ||
           format == SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM ||
           format == SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
}

static u32 CompressedMipBytes(SDL_GPUTextureFormat format, u32 width, u32 height)
{
    u32 blockSize = SDL_GPUTextureFormatTexelBlockSize(format);
    u32 blocksX = (width + 3u) / 4u;
    u32 blocksY = (height + 3u) / 4u;
    return blocksX * blocksY * blockSize;
}

static SDL_GPUTextureFormat SelectAlbedoPageFormat(void)
{
#if defined(__ANDROID__)
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
#else
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
#endif
    return SDL_GPUTextureSupportsFormat(g_GPUDevice, format, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREUSAGE_SAMPLER) ?
           format : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

static SDL_GPUTextureFormat SelectRGPageFormat(void)
{
#if defined(__ANDROID__)
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
#else
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
#endif
    return SDL_GPUTextureSupportsFormat(g_GPUDevice, format, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREUSAGE_SAMPLER) ?
           format : SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
}

static TextureClassInfo GetTextureClassInfo(u32 textureClass)
{
    if (textureClass == TextureClass_Normal)
    {
        return (TextureClassInfo){
            "NormalPages", SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
            TextureDesc_Normal, TextureDesc_DefaultNormal, 0u
        };
    }
    if (textureClass == TextureClass_MetallicRoughness)
    {
        return (TextureClassInfo){
            "MetallicRoughnessPages", SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
            TextureDesc_MetallicRoughness, TextureDesc_DefaultMetallicRoughness, 1u
        };
    }
    return (TextureClassInfo){
        "AlbedoPages", SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        TextureDesc_Albedo, TextureDesc_DefaultAlbedo, 0u
    };
}

static SDL_GPUTextureFormat ClassPageFormat(const TextureSystem* ts, u32 textureClass)
{
    if (!ts->compressed) return GetTextureClassInfo(textureClass).uncompressedFormat;
    return textureClass == TextureClass_Albedo ? ts->albedoFormat : ts->rgFormat;
}

static u32 PageMipCount(const TextureSystem* ts)
{
    return ts->compressed ? CompressedPageMaxMips : MipCountForSize(TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE);
}

static void InitCopyPipelines(void)
{
    gCopyRGBAPipeline = SDL_CreateGPUComputePipeline(g_GPUDevice, &(SDL_GPUComputePipelineCreateInfo){
        .code = Shaders_TexturePageCopyRGBA_spv,
        .code_size = sizeof(Shaders_TexturePageCopyRGBA_spv),
        .entrypoint = AX_GPU_COMPUTE_ENTRYPOINT,
        .format = AX_GPU_SHADER_FORMAT,
        .num_samplers = 1,
        .num_readwrite_storage_textures = 1,
        .num_uniform_buffers = 1,
        .threadcount_x = 8,
        .threadcount_y = 8,
        .threadcount_z = 1,
    });
    CHECK_CREATE(gCopyRGBAPipeline, "Texture Page Copy RGBA Pipeline");

    gCopyRGPipeline = SDL_CreateGPUComputePipeline(g_GPUDevice, &(SDL_GPUComputePipelineCreateInfo){
        .code = Shaders_TexturePageCopyRG_spv,
        .code_size = sizeof(Shaders_TexturePageCopyRG_spv),
        .entrypoint = AX_GPU_COMPUTE_ENTRYPOINT,
        .format = AX_GPU_SHADER_FORMAT,
        .num_samplers = 1,
        .num_readwrite_storage_textures = 1,
        .num_uniform_buffers = 1,
        .threadcount_x = 8,
        .threadcount_y = 8,
        .threadcount_z = 1,
    });
    CHECK_CREATE(gCopyRGPipeline, "Texture Page Copy RG Pipeline");
}

void TextureSystem_InitDevice(void)
{
    InitCopyPipelines();
}

void TextureSystem_DestroyDevice(void)
{
    if (gCopyRGBAPipeline) SDL_ReleaseGPUComputePipeline(g_GPUDevice, gCopyRGBAPipeline);
    if (gCopyRGPipeline)   SDL_ReleaseGPUComputePipeline(g_GPUDevice, gCopyRGPipeline);
    gCopyRGBAPipeline = NULL;
    gCopyRGPipeline = NULL;
}

void TextureSystem_ReleaseTextures(Texture* textures, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        if (textures[i].handle)
        {
            SDL_ReleaseGPUTexture(g_GPUDevice, textures[i].handle);
            textures[i].handle = NULL;
        }
        if (textures[i].buffer && textures[i].bufferSize)
        {
            SDL_free(textures[i].buffer);
            textures[i].buffer = NULL;
            textures[i].bufferSize = 0;
        }
    }
}

static void ReleasePageTexture(Texture* texture)
{
    if (texture->handle) SDL_ReleaseGPUTexture(g_GPUDevice, texture->handle);
    *texture = (Texture){0};
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                       Descriptors and Materials                          */
/*//////////////////////////////////////////////////////////////////////////*/

// item is the atlas placement backing this descriptor, or NULL for descriptors that
// have no reclaimable page space (defaults, or entries restored from a baked dump)
static u32 AddDescriptor(TextureSystem* ts, u32 textureClass, SmolAtlasItem* item, u32 pageIndex, float x, float y, float w, float h)
{
    s32 descriptorIdx = BitsetFindFirstEmpty(ts->descriptorSlots, MAX_TEXTURE_DESCRIPTORS);
    if (descriptorIdx < 0)
    {
        AX_ERROR("maximum texture descriptors reached");
        return TextureDesc_Invalid;
    }

    u32 idx = (u32)descriptorIdx;
    BitsetSet(ts->descriptorSlots, descriptorIdx);
    if (idx >= ts->numDescriptors) ts->numDescriptors = idx + 1u;

    TextureDescriptor* desc = &ts->descriptors[idx];
    desc->pageIndex = pageIndex;
    desc->flags = 0;
    desc->uvScale.x = w / (float)TEXTURE_PAGE_SIZE;
    desc->uvScale.y = h / (float)TEXTURE_PAGE_SIZE;
    desc->uvBias.x = x / (float)TEXTURE_PAGE_SIZE;
    desc->uvBias.y = y / (float)TEXTURE_PAGE_SIZE;

    ts->descriptorPacking[idx].textureClass = textureClass;
    ts->descriptorPacking[idx].item = item;
    return idx;
}

static u32 AddDescriptorFlags(TextureSystem* ts, u32 textureClass, SmolAtlasItem* item, u32 pageIndex, float x, float y, float w, float h, u32 flags)
{
    u32 idx = AddDescriptor(ts, textureClass, item, pageIndex, x, y, w, h);
    ts->descriptors[idx].flags = flags;
    return idx;
}

static void UpdateDescriptorWatermark(TextureSystem* ts)
{
    while (ts->numDescriptors > TextureDesc_DefaultCount &&
           !BitsetGet(ts->descriptorSlots, (s32)(ts->numDescriptors - 1u)))
        ts->numDescriptors--;
}

static void FreeDescriptor(TextureSystem* ts, u32 descriptorIdx)
{
    if (descriptorIdx < TextureDesc_DefaultCount || descriptorIdx >= MAX_TEXTURE_DESCRIPTORS) return;

    TextureDescriptorPacking* packing = &ts->descriptorPacking[descriptorIdx];
    if (packing->item)
    {
        TexturePageClass* cls = &ts->classes[packing->textureClass];
        SAItemRemove(cls->packer[ts->descriptors[descriptorIdx].pageIndex], packing->item);
    }
    *packing = (TextureDescriptorPacking){0};

    BitsetReset(ts->descriptorSlots, (s32)descriptorIdx);
    ts->descriptors[descriptorIdx] = (TextureDescriptor){0};
}

static void MarkDescriptorOccupied(TextureSystem* ts, u32 descriptorIdx)
{
    if (descriptorIdx >= MAX_TEXTURE_DESCRIPTORS) return;
    BitsetSet(ts->descriptorSlots, (s32)descriptorIdx);
}

static void RebuildDescriptorSlotsFromMaterials(TextureSystem* ts, const MaterialGPU* materials, u32 materialCount)
{
    MemsetZero(ts->descriptorSlots, ((MAX_TEXTURE_DESCRIPTORS + 63u) >> 6) * sizeof(u64));
    for (u32 i = 0; i < TextureDesc_DefaultCount; i++)
        MarkDescriptorOccupied(ts, i);

    for (u32 i = 0; i < materialCount; i++)
    {
        MarkDescriptorOccupied(ts, materials[i].albedoDescriptor);
        MarkDescriptorOccupied(ts, materials[i].normalDescriptor);
        MarkDescriptorOccupied(ts, materials[i].metallicRoughnessDescriptor);
    }
}

static void AddDefaultDescriptors(TextureSystem* ts)
{
    MemsetZero(ts->descriptorSlots, ((MAX_TEXTURE_DESCRIPTORS + 63u) >> 6) * sizeof(u64));
    MemsetZero(ts->descriptorPacking, sizeof(TextureDescriptorPacking) * MAX_TEXTURE_DESCRIPTORS);
    ts->numDescriptors = 0;
    // defaults sit on the permanently reserved page-0 origin block, no atlas item to track
    AddDescriptorFlags(ts, 0, NULL, 0, 0, 0, TextureDefaultSize, TextureDefaultSize, TextureDesc_DefaultAlbedo); // invalid also samples harmless default
    AddDescriptorFlags(ts, 0, NULL, 0, 0, 0, TextureDefaultSize, TextureDefaultSize, TextureDesc_DefaultAlbedo);
    AddDescriptorFlags(ts, 0, NULL, 0, 0, 0, TextureDefaultSize, TextureDefaultSize, TextureDesc_DefaultNormal);
    AddDescriptorFlags(ts, 0, NULL, 0, 0, 0, TextureDefaultSize, TextureDefaultSize, TextureDesc_DefaultMetallicRoughness);
}

static u32 PackMaterialFlags(const AMaterial* material)
{
    u32 cutoff = (u32)(Saturatef32(material->alphaCutoff) * 255.0f + 0.5f);
    u32 flags = (cutoff << MATERIAL_ALPHA_CUTOFF_SHIFT) & MATERIAL_ALPHA_CUTOFF_MASK;
    if (material->alphaMode == AMaterialAlphaMode_Mask)
        flags |= MATERIAL_FLAG_ALPHA_MASK;
    return flags;
}

// resolves a bundle texture slot to a bundle local image index. out: false when unset
static bool ResolveBundleImage(const SceneBundle* bundle, u32 textureIndex, u32* outImage)
{
    if (textureIndex >= (u32)bundle->numTextures) return false;
    s32 source = bundle->textures[textureIndex].source;
    if (source < 0 || source >= bundle->numImages) return false;
    *outImage = (u32)source;
    return true;
}

static void MarkWantedTexture(u8* wanted[TextureClass_Count], u32 numImages, u32 textureClass, u32 imageIndex)
{
    if (imageIndex >= numImages || textureClass >= TextureClass_Count)
        return;

    for (u32 c = 0; c < TextureClass_Count; c++)
        wanted[c][imageIndex] = 0;
    wanted[textureClass][imageIndex] = 1;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                            Page Management                               */
/*//////////////////////////////////////////////////////////////////////////*/

static Texture CreatePageTexture(const TextureSystem* ts, u32 textureClass, u32 layerCount)
{
    Texture texture = {0};
    texture.width = TEXTURE_PAGE_SIZE;
    texture.height = TEXTURE_PAGE_SIZE;
    texture.format = ClassPageFormat(ts, textureClass);
    texture.mipLevels = PageMipCount(ts);

    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!ts->compressed) usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;

    SDL_GPUTextureCreateInfo texDesc = {
        .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
        .format = texture.format,
        .width = TEXTURE_PAGE_SIZE,
        .height = TEXTURE_PAGE_SIZE,
        .layer_count_or_depth = layerCount,
        .num_levels = texture.mipLevels,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
        .usage = usage,
        .props = SDL_CreateProperties()
    };
    SDL_SetStringProperty(texDesc.props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, GetTextureClassInfo(textureClass).label);
    texture.handle = SDL_CreateGPUTexture(g_GPUDevice, &texDesc);
    SDL_DestroyProperties(texDesc.props);
    CHECK_CREATE(texture.handle, GetTextureClassInfo(textureClass).label);
    return texture;
}

// compressed pages have no compute clear path, upload zero blocks so the gaps
// between placements stay defined for sampler bleed
static void ZeroFillCompressedLayers(const TextureSystem* ts, u32 textureClass, SDL_GPUCopyPass* copyPass,
                                     SDL_GPUTexture* handle, u32 firstLayer, u32 endLayer,
                                     SDL_GPUTransferBuffer** outTransfer)
{
    SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
    u32 mip0Bytes = CompressedMipBytes(format, TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE);

    SDL_GPUTransferBuffer* zeroBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = mip0Bytes
    });
    void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, zeroBuffer, false);
    SmallMemSet(map, 0, mip0Bytes);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, zeroBuffer);

    for (u32 layer = firstLayer; layer < endLayer; layer++)
    {
        for (u32 mip = 0; mip < CompressedPageMaxMips; mip++)
        {
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
            SDL_GPUTextureTransferInfo transferInfo = { .transfer_buffer = zeroBuffer, .offset = 0 };
            SDL_GPUTextureRegion region = {
                .texture = handle, .mip_level = mip, .layer = layer,
                .w = mipSize, .h = mipSize, .d = 1
            };
            SDL_UploadToGPUTexture(copyPass, &transferInfo, &region, false);
        }
    }
    *outTransfer = zeroBuffer;
}

// opens a new page of a class: live packer state plus tail mip shadows for compressed pages
static void OpenPage(TextureSystem* ts, u32 textureClass)
{
    TexturePageClass* cls = &ts->classes[textureClass];
    u32 page = cls->openPages++;
    u32 alignment = ts->compressed ? CompressedPageAlign : 1u;

    cls->packer[page] = SACreate(TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE);

    if (ts->compressed)
    {
        SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
        for (u32 t = 0; t < TEXTURE_PAGE_TAIL_MIPS; t++)
        {
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> (CompressedDirectMips + t), 1u);
            cls->tailMips[page][t] = (u8*)AllocZeroTLSFGlobal(1, CompressedMipBytes(format, mipSize, mipSize));
        }
    }

    if (page == 0)
    {
        // keep the default block region at the page origin, permanently reserved (never removed)
        u32 reserveSize = AlignUpu32(TextureDefaultSize, alignment);
        SAPack(cls->packer[0], (int)reserveSize, (int)reserveSize);
    }
}

static void WriteBC4ConstantBlock(u8* dst, u8 value)
{
    dst[0] = value;
    dst[1] = value;
    for (u32 i = 2; i < 8; i++) dst[i] = 0;
}

// uploads the 16x16 default block of page 0. compressed albedo keeps the zero fill
// (matches the previous zeroed page behavior), bc5 classes get their neutral pattern
static void UploadDefaultRegion(TextureSystem* ts, u32 textureClass)
{
    TexturePageClass* cls = &ts->classes[textureClass];

    if (!ts->compressed)
    {
        u8 albedo[TextureDefaultSize * TextureDefaultSize * 4];
        u8 rg[TextureDefaultSize * TextureDefaultSize * 2];
        for (u32 i = 0; i < TextureDefaultSize * TextureDefaultSize; i++)
        {
            albedo[i * 4 + 0] = 255; albedo[i * 4 + 1] = 255;
            albedo[i * 4 + 2] = 255; albedo[i * 4 + 3] = 255;
            rg[i * 2 + 0] = textureClass == TextureClass_Normal ? 128 : 0;
            rg[i * 2 + 1] = textureClass == TextureClass_Normal ? 128 : 255;
        }
        const void* data = textureClass == TextureClass_Albedo ? (const void*)albedo : (const void*)rg;
        UploadTextureRegion(cls->pages, 0, 0, 0, TextureDefaultSize, TextureDefaultSize, TextureDefaultSize, TextureDefaultSize, data);
        return;
    }

    SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
    if (format != SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM) return; // albedo default stays zero filled

    u8 x = textureClass == TextureClass_Normal ? 128 : 0;
    u8 y = textureClass == TextureClass_Normal ? 128 : 255;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBuffer* transfers[CompressedDirectMips];
    u32 numTransfers = 0;

    for (u32 mip = 0; mip < CompressedDirectMips; mip++)
    {
        u32 defaultMipSize = Maxu32(TextureDefaultSize >> mip, 1u);
        u32 blocks = (defaultMipSize + 3u) / 4u;
        u32 byteCount = blocks * blocks * 16u;

        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = byteCount });
        u8* map = (u8*)SDL_MapGPUTransferBuffer(g_GPUDevice, tb, false);
        for (u32 b = 0; b < blocks * blocks; b++)
        {
            WriteBC4ConstantBlock(map + b * 16u, x);
            WriteBC4ConstantBlock(map + b * 16u + 8u, y);
        }
        SDL_UnmapGPUTransferBuffer(g_GPUDevice, tb);

        SDL_GPUTextureRegion region = {
            .texture = cls->pages.handle, .mip_level = mip, .layer = 0,
            .w = blocks * 4u, .h = blocks * 4u, .d = 1
        };
        SDL_UploadToGPUTexture(copyPass, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb }, &region, false);
        transfers[numTransfers++] = tb;
    }

    // tail shadows: one neutral block at the origin, uploaded with the whole tail mips
    for (u32 t = 0; t < TEXTURE_PAGE_TAIL_MIPS; t++)
    {
        WriteBC4ConstantBlock(cls->tailMips[0][t], x);
        WriteBC4ConstantBlock(cls->tailMips[0][t] + 8u, y);
    }
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    for (u32 i = 0; i < numTransfers; i++)
        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfers[i]);
}

static void CreateInitialPages(TextureSystem* ts)
{
    AddDefaultDescriptors(ts);

    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        TexturePageClass* cls = &ts->classes[c];
        cls->layerCount = 1;
        cls->pages = CreatePageTexture(ts, c, 1);

        if (ts->compressed)
        {
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBuffer* zeroBuffer = NULL;
            ZeroFillCompressedLayers(ts, c, copyPass, cls->pages.handle, 0, 1, &zeroBuffer);
            SDL_EndGPUCopyPass(copyPass);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
            SDL_ReleaseGPUFence(g_GPUDevice, fence);
            SDL_ReleaseGPUTransferBuffer(g_GPUDevice, zeroBuffer);
        }

        OpenPage(ts, c);
        UploadDefaultRegion(ts, c);
    }

    UpdateGPUBuffer(ts->descriptorBuffer, ts->descriptors, sizeof(TextureDescriptor) * ts->numDescriptors, 0);
}

// grows the gpu layer count of a class to the next power of two that fits openPages,
// copying the existing layers over
static void GrowPageLayers(TextureSystem* ts, u32 textureClass)
{
    TexturePageClass* cls = &ts->classes[textureClass];
    if (cls->openPages <= cls->layerCount) return;

    u32 newCount = cls->layerCount;
    while (newCount < cls->openPages) newCount += 1;
    if (newCount > TEXTURE_PAGE_LAYERS) newCount = TEXTURE_PAGE_LAYERS;

    Texture newTexture = CreatePageTexture(ts, textureClass, newCount);
    u32 mipCount = PageMipCount(ts);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBuffer* zeroBuffer = NULL;
    if (ts->compressed)
        ZeroFillCompressedLayers(ts, textureClass, copyPass, newTexture.handle, cls->layerCount, newCount, &zeroBuffer);

    for (u32 layer = 0; layer < cls->layerCount; layer++)
    {
        for (u32 mip = 0; mip < mipCount; mip++)
        {
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
            SDL_GPUTextureLocation src = { .texture = cls->pages.handle, .mip_level = mip, .layer = layer };
            SDL_GPUTextureLocation dst = { .texture = newTexture.handle, .mip_level = mip, .layer = layer };
            SDL_CopyGPUTextureToTexture(copyPass, &src, &dst, mipSize, mipSize, 1, false);
        }
    }
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    if (zeroBuffer) SDL_ReleaseGPUTransferBuffer(g_GPUDevice, zeroBuffer);

    ReleasePageTexture(&cls->pages);
    cls->pages = newTexture;
    AX_LOG("texture class %d pages grown to %d layers", textureClass, newCount);
    cls->layerCount = newCount;
}

static void ReleaseClassState(TextureSystem* ts)
{
    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        TexturePageClass* cls = &ts->classes[c];
        ReleasePageTexture(&cls->pages);
        for (u32 page = 0; page < cls->openPages; page++)
        {
            SADestroy(cls->packer[page]);
            cls->packer[page] = NULL;
            for (u32 t = 0; t < TEXTURE_PAGE_TAIL_MIPS; t++)
            {
                if (cls->tailMips[page][t]) DeAllocateTLSFGlobal(cls->tailMips[page][t]);
                cls->tailMips[page][t] = NULL;
            }
        }
        cls->openPages = 0;
        cls->layerCount = 0;
    }
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                          Incremental Packing                             */
/*//////////////////////////////////////////////////////////////////////////*/

typedef struct TexturePendingRect_
{
    u32 imageIndex;
    u32 width, height;
    u8  packed;
} TexturePendingRect;

// packs the wanted staging images into the class pages, continuing on the persistent
// per page packers and opening new pages as needed. out: number of placements
static u32 PackClassIncremental(TextureSystem* ts, u32 textureClass, const Texture* staging, const u8* wanted,
                                u32 numImages, TexturePlacement* outPlacements)
{
    TexturePageClass* cls = &ts->classes[textureClass];
    u32 alignment = ts->compressed ? CompressedPageAlign : 1u;

    TexturePendingRect* rects = (TexturePendingRect*)ArenaPushGlobal(numImages * sizeof(TexturePendingRect));
    u32 numRects = 0;

    for (u32 i = 0; i < numImages; i++)
    {
        const Texture* tex = &staging[i];
        if (!wanted[i] || tex->width <= 0 || tex->height <= 0) continue;
        if (ts->compressed)
        {
            if (!tex->buffer || tex->bufferSize == 0) continue;
        }
        else if (!tex->handle) continue;

        u32 physicalW = AlignUpu32((u32)tex->width, alignment);
        u32 physicalH = AlignUpu32((u32)tex->height, alignment);
        if (physicalW > TEXTURE_PAGE_SIZE || physicalH > TEXTURE_PAGE_SIZE)
        {
            AX_WARN("texture %d too large for page: %dx%d", i, tex->width, tex->height);
            continue;
        }
        rects[numRects].imageIndex = i;
        rects[numRects].width = physicalW;
        rects[numRects].height = physicalH;
        rects[numRects].packed = 0;
        numRects++;
    }

    u32 placed = 0;
    u32 remaining = numRects;
    for (u32 page = 0; remaining > 0 && page < TEXTURE_PAGE_LAYERS; page++)
    {
        if (page >= cls->openPages)
            OpenPage(ts, textureClass);

        for (u32 r = 0; r < numRects; r++)
        {
            if (rects[r].packed) continue;
            SmolAtlasItem* item = SAPack(cls->packer[page], (int)rects[r].width, (int)rects[r].height);
            if (!item) continue;

            rects[r].packed = 1;
            remaining--;
            u32 imageIdx = rects[r].imageIndex;
            outPlacements[placed++] = (TexturePlacement){
                imageIdx, page, (u32)SAItemX(item), (u32)SAItemY(item),
                (u32)staging[imageIdx].width, (u32)staging[imageIdx].height, 0u, item
            };
        }
    }

    ArenaPopGlobal(numImages * sizeof(TexturePendingRect));
    if (remaining > 0)
        AX_WARN("texture class %d packer ran out of %d pages, %d textures fall back to defaults",
                textureClass, TEXTURE_PAGE_LAYERS, remaining);
    return placed;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                          Compressed Upload Path                          */
/*//////////////////////////////////////////////////////////////////////////*/

// nearest block sampled copy of one transcoded mip into a tight destination block array
static void BuildRegionBlocks(const BasisuTranscodedImage* image, u32 bytesPerBlock, u32 mip,
                              u32 dstBlocksX, u32 dstBlocksY, u8* dst)
{
    u32 sourceMip = mip < image->mipLevels ? mip : image->mipLevels - 1u;
    const BasisuTranscodedLevel* level = &image->levels[sourceMip];
    u32 srcBlocksX = (level->width + 3u) / 4u;
    u32 srcBlocksY = (level->height + 3u) / 4u;
    const u8* srcBlocks = (const u8*)image->data + level->offset;

    for (u32 row = 0; row < dstBlocksY; row++)
    {
        u32 srcRow = (row * srcBlocksY) / dstBlocksY;
        for (u32 col = 0; col < dstBlocksX; col++)
        {
            u32 srcCol = (col * srcBlocksX) / dstBlocksX;
            MemCopy(dst + (row * dstBlocksX + col) * bytesPerBlock,
                    srcBlocks + (srcRow * srcBlocksX + srcCol) * bytesPerBlock, bytesPerBlock);
        }
    }
}

// blends the placement's tail mips into the cpu page shadows. block coordinates truncate
// here, the resulting shift of up to three texels only affects the 128px and lower mips
static void BlendTailShadows(TexturePageClass* cls, SDL_GPUTextureFormat format,
                             const BasisuTranscodedImage* image, const TexturePlacement* p)
{
    u32 bytesPerBlock = SDL_GPUTextureFormatTexelBlockSize(format);
    for (u32 t = 0; t < TEXTURE_PAGE_TAIL_MIPS; t++)
    {
        u32 mip = CompressedDirectMips + t;
        u32 pageMipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
        u32 pageBlocksX = (pageMipSize + 3u) / 4u;
        u32 pageBlocksY = pageBlocksX;
        u32 blockX = (p->x >> mip) / 4u;
        u32 blockY = (p->y >> mip) / 4u;
        u32 dstW = Maxu32(p->width >> mip, 1u);
        u32 dstH = Maxu32(p->height >> mip, 1u);
        u32 copyBlocksX = (dstW + 3u) / 4u;
        u32 copyBlocksY = (dstH + 3u) / 4u;
        if (blockX >= pageBlocksX || blockY >= pageBlocksY) continue;
        if (blockX + copyBlocksX > pageBlocksX) copyBlocksX = pageBlocksX - blockX;
        if (blockY + copyBlocksY > pageBlocksY) copyBlocksY = pageBlocksY - blockY;

        u32 sourceMip = mip < image->mipLevels ? mip : image->mipLevels - 1u;
        const BasisuTranscodedLevel* level = &image->levels[sourceMip];
        u32 srcBlocksX = (level->width + 3u) / 4u;
        u32 srcBlocksY = (level->height + 3u) / 4u;
        const u8* srcBlocks = (const u8*)image->data + level->offset;
        u8* shadow = cls->tailMips[p->page][t];

        for (u32 row = 0; row < copyBlocksY; row++)
        {
            u32 srcRow = (row * srcBlocksY) / copyBlocksY;
            for (u32 col = 0; col < copyBlocksX; col++)
            {
                u32 srcCol = (col * srcBlocksX) / copyBlocksX;
                MemCopy(shadow + ((blockY + row) * pageBlocksX + blockX + col) * bytesPerBlock,
                        srcBlocks + (srcRow * srcBlocksX + srcCol) * bytesPerBlock, bytesPerBlock);
            }
        }
    }
}

// transcodes and uploads the packed placements: direct block aligned region uploads for the
// first mips, shadow blend + whole mip re-upload for the tail. fills ok[] per placement
static void UploadCompressedPlacements(TextureSystem* ts, u32 textureClass, const Texture* staging,
                                       const TexturePlacement* placements, u32 count, u8* ok)
{
    if (count == 0) return;

    TexturePageClass* cls = &ts->classes[textureClass];
    SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
    u32 bytesPerBlock = SDL_GPUTextureFormatTexelBlockSize(format);
    bool tailTouched[TEXTURE_PAGE_LAYERS] = {0};

    u32 maxTransfers = count * CompressedDirectMips + TEXTURE_PAGE_LAYERS * TEXTURE_PAGE_TAIL_MIPS;
    SDL_GPUTransferBuffer** transfers = (SDL_GPUTransferBuffer**)ArenaPushGlobal(maxTransfers * sizeof(void*));
    u32 numTransfers = 0;
    u8* scratch = (u8*)ArenaPushGlobal(CompressedMipBytes(format, TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE));

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    for (u32 i = 0; i < count; i++)
    {
        const TexturePlacement* p = &placements[i];
        const Texture* src = &staging[p->imageIndex];
        ok[i] = 0;

        BasisuTranscodedImage image;
        if (!BasisuTranscodeImage(src->buffer, src->bufferSize, format, &image))
        {
            AX_WARN("texture transcode failed for image %d, using default", p->imageIndex);
            continue;
        }
        if (image.mipLevels == 0)
        {
            BasisuFreeTranscodedImage(&image);
            AX_WARN("texture transcode produced no mips for image %d, using default", p->imageIndex);
            continue;
        }
        ok[i] = 1;

        for (u32 mip = 0; mip < CompressedDirectMips; mip++)
        {
            u32 dstW = Maxu32(p->width >> mip, 1u);
            u32 dstH = Maxu32(p->height >> mip, 1u);
            u32 dstBlocksX = (dstW + 3u) / 4u;
            u32 dstBlocksY = (dstH + 3u) / 4u;
            u32 byteCount = dstBlocksX * dstBlocksY * bytesPerBlock;

            BuildRegionBlocks(&image, bytesPerBlock, mip, dstBlocksX, dstBlocksY, scratch);

            SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
                .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = byteCount });
            void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, tb, false);
            MemCopy(map, scratch, byteCount);
            SDL_UnmapGPUTransferBuffer(g_GPUDevice, tb);
            transfers[numTransfers++] = tb;

            // CompressedPageAlign keeps x>>mip and y>>mip block aligned for the direct mips
            SDL_GPUTextureRegion region = {
                .texture = cls->pages.handle, .mip_level = mip, .layer = p->page,
                .x = p->x >> mip, .y = p->y >> mip,
                .w = dstBlocksX * 4u, .h = dstBlocksY * 4u, .d = 1
            };
            SDL_UploadToGPUTexture(copyPass, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb }, &region, false);
        }

        BlendTailShadows(cls, format, &image, p);
        tailTouched[p->page] = true;
        BasisuFreeTranscodedImage(&image);
    }

    for (u32 page = 0; page < cls->openPages; page++)
    {
        if (!tailTouched[page]) continue;
        for (u32 t = 0; t < TEXTURE_PAGE_TAIL_MIPS; t++)
        {
            u32 mip = CompressedDirectMips + t;
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
            u32 byteCount = CompressedMipBytes(format, mipSize, mipSize);

            SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
                .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = byteCount });
            void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, tb, false);
            MemCopy(map, cls->tailMips[page][t], byteCount);
            SDL_UnmapGPUTransferBuffer(g_GPUDevice, tb);
            transfers[numTransfers++] = tb;

            SDL_GPUTextureRegion region = {
                .texture = cls->pages.handle, .mip_level = mip, .layer = page,
                .w = mipSize, .h = mipSize, .d = 1
            };
            SDL_UploadToGPUTexture(copyPass, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = tb }, &region, false);
        }
    }

    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    for (u32 i = 0; i < numTransfers; i++)
        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfers[i]);

    ArenaPopGlobal(CompressedMipBytes(format, TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE));
    ArenaPopGlobal(maxTransfers * sizeof(void*));
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                        Uncompressed Upload Path                          */
/*//////////////////////////////////////////////////////////////////////////*/

static void QueuePackedTextureCopy(u32 textureClass, u32 sourceIndex, const Texture* src, u32 page, u32 x, u32 y, u32 padding)
{
    if (!src || !src->handle || src->width <= 0 || src->height <= 0) return;
    if (gNumCopyRequests >= MAX_TEXTURE_DESCRIPTORS)
    {
        AX_WARN("maximum texture page copy requests reached");
        return;
    }
    PageCopyRequest* req = &gCopyRequests[gNumCopyRequests++];
    req->textureClass = textureClass;
    req->sourceIndex = sourceIndex;
    req->page = page;
    req->x = x;
    req->y = y;
    req->width = (u32)src->width;
    req->height = (u32)src->height;
    req->padding = padding;
}

static void DispatchPageCopies(TextureSystem* ts, const Texture* staging)
{
    if (gNumCopyRequests == 0) return;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    for (u32 i = 0; i < gNumCopyRequests; i++)
    {
        PageCopyRequest* req = &gCopyRequests[i];
        const Texture* src = &staging[req->sourceIndex];
        const Texture* dst = &ts->classes[req->textureClass].pages;
        if (!src->handle || !dst->handle) continue;

        u32 sourceMipCount = MipCountForSize(req->width, req->height);
        if (src->mipLevels > 0 && src->mipLevels < sourceMipCount) sourceMipCount = src->mipLevels;
        u32 pageMipCount = PageMipCount(ts);
        for (u32 mip = 0; mip < pageMipCount; mip++)
        {
            u32 sourceMip = mip < sourceMipCount ? mip : sourceMipCount - 1u;
            u32 mipWidth  = Maxu32(req->width >> mip, 1u);
            u32 mipHeight = Maxu32(req->height >> mip, 1u);
            u32 sourceWidth  = Maxu32(req->width >> sourceMip, 1u);
            u32 sourceHeight = Maxu32(req->height >> sourceMip, 1u);
            u32 pageMipSize = TEXTURE_PAGE_SIZE >> mip;
            if (pageMipSize == 0) pageMipSize = 1;
            u32 innerX = req->x >> mip;
            u32 innerY = req->y >> mip;
            u32 gutter = req->padding && innerX > 0 && innerY > 0 &&
                         (innerX + mipWidth) < pageMipSize &&
                         (innerY + mipHeight) < pageMipSize ? 1u : 0u;
            u32 copyWidth = mipWidth + gutter * 2u;
            u32 copyHeight = mipHeight + gutter * 2u;
            SDL_GPUStorageTextureReadWriteBinding rwTexture = {
                .texture = dst->handle,
                .mip_level = mip,
                .layer = req->page,
                .cycle = false
            };
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &rwTexture, 1, NULL, 0);
            SDL_BindGPUComputePipeline(pass, req->textureClass == TextureClass_Albedo ? gCopyRGBAPipeline : gCopyRGPipeline);
            SDL_BindGPUComputeSamplers(pass, 0, &(SDL_GPUTextureSamplerBinding){
                .texture = src->handle,
                .sampler = g_RenderState.sampler
            }, 1);
            struct { u32 dstOffset[2]; u32 copySize[2]; u32 sourceSize[2]; u32 sourceMip; u32 gutter; u32 channelMode; } params = {
                { innerX - gutter, innerY - gutter },
                { copyWidth, copyHeight },
                { sourceWidth, sourceHeight },
                sourceMip,
                gutter,
                GetTextureClassInfo(req->textureClass).channelMode
            };
            SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
            SDL_DispatchGPUCompute(pass, (copyWidth + 7u) / 8u, (copyHeight + 7u) / 8u, 1);
            SDL_EndGPUComputePass(pass);
        }
    }

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Instance API                                */
/*//////////////////////////////////////////////////////////////////////////*/

void TextureSystem_Init(TextureSystem* ts)
{
    MemsetZero(ts, sizeof(*ts));
    ts->descriptors = (TextureDescriptor*)AllocZeroTLSFGlobal(MAX_TEXTURE_DESCRIPTORS, sizeof(TextureDescriptor));
    ts->descriptorPacking = (TextureDescriptorPacking*)AllocZeroTLSFGlobal(MAX_TEXTURE_DESCRIPTORS, sizeof(TextureDescriptorPacking));
    ts->materials   = (MaterialGPU*)AllocZeroTLSFGlobal(MAX_GPU_MATERIALS, sizeof(MaterialGPU));
    ts->descriptorSlots = (u64*)AllocZeroTLSFGlobal((MAX_TEXTURE_DESCRIPTORS + 63u) >> 6, sizeof(u64));
    ts->descriptorBuffer = CreateBuffer(NULL, sizeof(TextureDescriptor) * MAX_TEXTURE_DESCRIPTORS, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "TextureDescriptors");
    ts->materialBuffer   = CreateBuffer(NULL, sizeof(MaterialGPU) * MAX_GPU_MATERIALS, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "Materials");

    ts->albedoFormat = SelectAlbedoPageFormat();
    ts->rgFormat     = SelectRGPageFormat();
    ts->compressed   = IsBlockCompressedFormat(ts->albedoFormat) && IsBlockCompressedFormat(ts->rgFormat) ? 1u : 0u;

    CreateInitialPages(ts);
}

void TextureSystem_Destroy(TextureSystem* ts)
{
    ReleaseClassState(ts);
    if (ts->descriptorBuffer) SDL_ReleaseGPUBuffer(g_GPUDevice, ts->descriptorBuffer);
    if (ts->materialBuffer)   SDL_ReleaseGPUBuffer(g_GPUDevice, ts->materialBuffer);
    if (ts->descriptors)      DeAllocateTLSFGlobal(ts->descriptors);
    if (ts->descriptorPacking) DeAllocateTLSFGlobal(ts->descriptorPacking);
    if (ts->materials)        DeAllocateTLSFGlobal(ts->materials);
    if (ts->descriptorSlots)  DeAllocateTLSFGlobal(ts->descriptorSlots);
    MemsetZero(ts, sizeof(*ts));
}

void TextureSystem_ResetPacking(TextureSystem* ts)
{
    ReleaseClassState(ts);
    CreateInitialPages(ts);
}

s32 TextureSystem_AppendBundle(TextureSystem* ts, const SceneBundle* bundle, const Texture* stagingTextures, u32 materialOffset)
{
    double startTime = TimeSinceStartup();
    u32 numImages = bundle->numImages > 0 ? (u32)bundle->numImages : 0u;
    u32 numMaterials = bundle->numMaterials > 0 ? (u32)bundle->numMaterials : 0u;

    if (materialOffset + numMaterials > MAX_GPU_MATERIALS)
    {
        AX_ERROR("maximum GPU materials reached: offset=%d count=%d", materialOffset, numMaterials);
        return 0;
    }

    ArenaMark mark = ArenaSave(&GlobalArena);
    u8*  wanted[TextureClass_Count];
    u32* descMap[TextureClass_Count];
    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        wanted[c]  = (u8*)ArenaAllocZero(&GlobalArena, Maxu32(numImages, 1u));
        descMap[c] = (u32*)ArenaAlloc(&GlobalArena, Maxu32(numImages, 1u) * sizeof(u32));
        u32 defaultDescriptor = GetTextureClassInfo(c).defaultDescriptor;
        for (u32 i = 0; i < numImages; i++) descMap[c][i] = defaultDescriptor;
    }
    TexturePlacement* placements = (TexturePlacement*)ArenaAlloc(&GlobalArena, Maxu32(numImages, 1u) * sizeof(TexturePlacement));
    u8* placementOk = (u8*)ArenaAlloc(&GlobalArena, Maxu32(numImages, 1u));

    for (u32 m = 0; m < numMaterials; m++)
    {
        const AMaterial* src = &bundle->materials[m];
        u32 image;
        if (ResolveBundleImage(bundle, src->textures[0].index, &image))
            MarkWantedTexture(wanted, numImages, TextureClass_Normal, image);
        if (ResolveBundleImage(bundle, src->metallicRoughnessTexture.index, &image))
            MarkWantedTexture(wanted, numImages, TextureClass_MetallicRoughness, image);
        // base color wins conflicts because it is visible albedo, while MR/normal can fall back safely
        if (ResolveBundleImage(bundle, src->baseColorTexture.index, &image))
            MarkWantedTexture(wanted, numImages, TextureClass_Albedo, image);
    }

    gNumCopyRequests = 0;

    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        u32 placedCount = PackClassIncremental(ts, c, stagingTextures, wanted[c], numImages, placements);
        GrowPageLayers(ts, c);

        if (ts->compressed)
        {
            UploadCompressedPlacements(ts, c, stagingTextures, placements, placedCount, placementOk);
        }
        else
        {
            for (u32 i = 0; i < placedCount; i++)
            {
                TexturePlacement* p = &placements[i];
                QueuePackedTextureCopy(c, p->imageIndex, &stagingTextures[p->imageIndex], p->page, p->x, p->y, p->padding);
                placementOk[i] = 1;
            }
        }

        for (u32 i = 0; i < placedCount; i++)
        {
            if (!placementOk[i]) continue;
            TexturePlacement* p = &placements[i];
            descMap[c][p->imageIndex] = AddDescriptor(ts, c, p->item, p->page, (float)p->x, (float)p->y, (float)p->width, (float)p->height);
        }
        AX_LOG("texture class %d packed %d images", c, placedCount);
    }

    if (!ts->compressed)
        DispatchPageCopies(ts, stagingTextures);

    for (u32 m = 0; m < numMaterials; m++)
    {
        const AMaterial* src = &bundle->materials[m];
        MaterialGPU* dst = &ts->materials[materialOffset + m];
        dst->albedoDescriptor = TextureDesc_Albedo;
        dst->normalDescriptor = TextureDesc_Normal;
        dst->metallicRoughnessDescriptor = TextureDesc_MetallicRoughness;
        dst->flags = PackMaterialFlags(src);
        dst->baseColorFactor = src->baseColorFactor;
        dst->metallicRoughnessFactor = ((u32)src->metallicFactor << 16u) | src->roughnessFactor;

        u32 image;
        if (ResolveBundleImage(bundle, src->baseColorTexture.index, &image))
            dst->albedoDescriptor = descMap[TextureClass_Albedo][image];
        if (ResolveBundleImage(bundle, src->textures[0].index, &image))
            dst->normalDescriptor = descMap[TextureClass_Normal][image];
        if (ResolveBundleImage(bundle, src->metallicRoughnessTexture.index, &image))
            dst->metallicRoughnessDescriptor = descMap[TextureClass_MetallicRoughness][image];
    }
    if (materialOffset + numMaterials > ts->materialWatermark)
        ts->materialWatermark = materialOffset + numMaterials;

    if (ts->numDescriptors > 0)
        UpdateGPUBuffer(ts->descriptorBuffer, ts->descriptors,
                        ts->numDescriptors * sizeof(TextureDescriptor), 0);
    if (numMaterials > 0)
        UpdateGPUBuffer(ts->materialBuffer, ts->materials + materialOffset,
                        numMaterials * sizeof(MaterialGPU), materialOffset * sizeof(MaterialGPU));

    ArenaRestore(&GlobalArena, mark);
    AX_LOG("texture bundle appended images=%d materials=%d descriptors=%d watermark=%d %.2fs",
           numImages, numMaterials, ts->numDescriptors, ts->materialWatermark, TimeSinceStartup() - startTime);
    return 1;
}

void TextureSystem_RemoveBundle(TextureSystem* ts, const SceneBundle* bundle, u32 materialOffset)
{
    u32 numMaterials = bundle->numMaterials > 0 ? (u32)bundle->numMaterials : 0u;
    if (numMaterials == 0 || materialOffset + numMaterials > MAX_GPU_MATERIALS) return;

    // page atlas space still leaks until TextureSystem_ResetPacking, but descriptor
    // slots are only referenced through these material slots and can be reused now.
    for (u32 m = 0; m < numMaterials; m++)
    {
        MaterialGPU* material = &ts->materials[materialOffset + m];
        FreeDescriptor(ts, material->albedoDescriptor);
        FreeDescriptor(ts, material->normalDescriptor);
        FreeDescriptor(ts, material->metallicRoughnessDescriptor);
    }
    UpdateDescriptorWatermark(ts);

    for (u32 m = 0; m < numMaterials; m++)
    {
        MaterialGPU* dst = &ts->materials[materialOffset + m];
        dst->albedoDescriptor = TextureDesc_Albedo;
        dst->normalDescriptor = TextureDesc_Normal;
        dst->metallicRoughnessDescriptor = TextureDesc_MetallicRoughness;
        dst->flags = 0;
        dst->baseColorFactor = 0xFFFFFFFFu;
        dst->metallicRoughnessFactor = 0x0000FFFFu;
    }
    UpdateGPUBuffer(ts->materialBuffer, ts->materials + materialOffset,
                    numMaterials * sizeof(MaterialGPU), materialOffset * sizeof(MaterialGPU));
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                        Baked Page Dump Save / Restore                    */
/*//////////////////////////////////////////////////////////////////////////*/

// raw page dump: header followed by layer major mip data in the platform's block
// compressed format. editor local cache, game builds will bake basis instead
#define BAKED_PAGE_MAGIC   0x50545841u // "AXTP"
#define BAKED_PAGE_VERSION 1u

typedef struct BakedPageHeader_
{
    u32 magic;
    u32 version;
    u32 format;   // SDL_GPUTextureFormat the dump was taken in
    u32 pageSize;
    u32 layers;
    u32 mips;
} BakedPageHeader;

// per layer byte size and per mip offsets of a page dump
static u32 BakedPageLayout(SDL_GPUTextureFormat format, u32 mipOffsets[TEXTURE_PAGE_MAX_MIPS])
{
    u32 perLayer = 0;
    for (u32 mip = 0; mip < TEXTURE_PAGE_MAX_MIPS; mip++)
    {
        u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
        mipOffsets[mip] = perLayer;
        perLayer += CompressedMipBytes(format, mipSize, mipSize);
    }
    return perLayer;
}

static void BakedPagesWriteDone(void* userData, s32 result)
{
    if (!result) AX_ERROR("baked page dump write failed");
    SDL_free(userData);
}

s32 TextureSystem_SaveBakedClass(TextureSystem* ts, u32 textureClass, const char* path)
{
    if (!ts->compressed)
    {
        AX_WARN("baked page dump needs block compressed pages");
        return 0;
    }
    TexturePageClass* cls = &ts->classes[textureClass];
    if (!cls->pages.handle || cls->openPages == 0u) return 0;

    SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
    u32 layers = cls->openPages;
    u32 mipOffsets[TEXTURE_PAGE_MAX_MIPS];
    u32 perLayer = BakedPageLayout(format, mipOffsets);
    u64 payloadBytes = (u64)perLayer * layers;

    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (u32)payloadBytes });
    if (!tb) return 0;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    for (u32 layer = 0; layer < layers; layer++)
    {
        for (u32 mip = 0; mip < TEXTURE_PAGE_MAX_MIPS; mip++)
        {
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
            SDL_GPUTextureRegion region = {
                .texture = cls->pages.handle, .mip_level = mip, .layer = layer,
                .w = mipSize, .h = mipSize, .d = 1
            };
            SDL_GPUTextureTransferInfo dst = {
                .transfer_buffer = tb,
                .offset = layer * perLayer + mipOffsets[mip]
            };
            SDL_DownloadFromGPUTexture(copyPass, &region, &dst);
        }
    }
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);

    u64 totalBytes = sizeof(BakedPageHeader) + payloadBytes;
    u8* fileData = (u8*)SDL_malloc(totalBytes);
    if (!fileData)
    {
        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, tb);
        return 0;
    }

    BakedPageHeader* header = (BakedPageHeader*)fileData;
    header->magic    = BAKED_PAGE_MAGIC;
    header->version  = BAKED_PAGE_VERSION;
    header->format   = (u32)format;
    header->pageSize = TEXTURE_PAGE_SIZE;
    header->layers   = layers;
    header->mips     = TEXTURE_PAGE_MAX_MIPS;

    void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, tb, false);
    MemCopy(fileData + sizeof(BakedPageHeader), map, payloadBytes);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, tb);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, tb);

    // the file write does not block the editor, the buffer frees in the callback
    if (!WriteAllBytesAsync(path, fileData, totalBytes, BakedPagesWriteDone, fileData))
    {
        SDL_free(fileData);
        return 0;
    }
    AX_LOG("baked page dump queued: %s layers=%d %.1fmb", path, layers, (double)totalBytes / (1024.0 * 1024.0));
    return 1;
}

// uploads every layer and mip of a raw page dump into the class pages. tail mips are
// also copied into the cpu shadows. out: 0 on failure (missing file, format mismatch)
static s32 RestoreBakedClass(TextureSystem* ts, u32 textureClass, const char* atlasPath)
{
    u64 size = FileSize(atlasPath);
    u8* data = size > sizeof(BakedPageHeader) ? (u8*)SDL_malloc(size) : NULL;
    if (!data)
    {
        AX_ERROR("baked page dump missing or empty: %s", atlasPath);
        return 0;
    }
    ReadAllFile(atlasPath, (char*)data, size);

    SDL_GPUTextureFormat format = ClassPageFormat(ts, textureClass);
    const BakedPageHeader* header = (const BakedPageHeader*)data;
    u32 mipOffsets[TEXTURE_PAGE_MAX_MIPS];
    u32 perLayer = BakedPageLayout(format, mipOffsets);

    if (header->magic != BAKED_PAGE_MAGIC || header->version != BAKED_PAGE_VERSION ||
        header->pageSize != TEXTURE_PAGE_SIZE || header->mips != TEXTURE_PAGE_MAX_MIPS ||
        header->layers == 0u || header->layers > TEXTURE_PAGE_LAYERS ||
        header->format != (u32)format ||
        size < sizeof(BakedPageHeader) + (u64)perLayer * header->layers)
    {
        AX_WARN("baked page dump invalid or wrong platform format, repack needed: %s", atlasPath);
        SDL_free(data);
        return 0;
    }

    u32 numLayers = header->layers;
    const u8* payload = data + sizeof(BakedPageHeader);
    TexturePageClass* cls = &ts->classes[textureClass];

    // open the remaining pages so the tail mip shadows exist. the packer state of every
    // page is meaningless after a baked restore, appends must repack first
    while (cls->openPages < numLayers)
        OpenPage(ts, textureClass);
    if (numLayers > cls->layerCount)
    {
        ReleasePageTexture(&cls->pages);
        cls->pages = CreatePageTexture(ts, textureClass, numLayers);
        cls->layerCount = numLayers;
    }

    u64 payloadBytes = (u64)perLayer * numLayers;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (u32)payloadBytes });
    if (!tb) {
        SDL_free(data);
        return 0;
    }
    void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, tb, false);
    MemCopy(map, payload, payloadBytes);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    for (u32 layer = 0; layer < numLayers; layer++)
    {
        for (u32 mip = 0; mip < TEXTURE_PAGE_MAX_MIPS; mip++)
        {
            u32 mipSize = Maxu32(TEXTURE_PAGE_SIZE >> mip, 1u);
            SDL_GPUTextureRegion region = {
                .texture = cls->pages.handle, .mip_level = mip, .layer = layer,
                .w = mipSize, .h = mipSize, .d = 1
            };
            SDL_GPUTextureTransferInfo src = {
                .transfer_buffer = tb,
                .offset = layer * perLayer + mipOffsets[mip]
            };
            SDL_UploadToGPUTexture(copyPass, &src, &region, false);

            if (mip >= CompressedDirectMips)
            {
                u32 byteCount = CompressedMipBytes(format, mipSize, mipSize);
                MemCopy(cls->tailMips[layer][mip - CompressedDirectMips],
                        payload + layer * perLayer + mipOffsets[mip], byteCount);
            }
        }
    }
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, tb);
    SDL_free(data);
    return 1;
}

s32 TextureSystem_RestoreBaked(TextureSystem* ts,
                               const char* atlasPaths[TextureClass_Count],
                               const TextureDescriptor* descriptors, u32 numDescriptors,
                               const MaterialGPU* materials, u32 materialWatermark)
{
    if (!ts->compressed)
    {
        AX_WARN("baked atlas restore needs block compressed pages, falling back");
        return 0;
    }
    if (numDescriptors < TextureDesc_DefaultCount || numDescriptors > MAX_TEXTURE_DESCRIPTORS ||
        materialWatermark > MAX_GPU_MATERIALS)
    {
        AX_ERROR("baked restore tables out of range: descriptors=%d materials=%d", numDescriptors, materialWatermark);
        return 0;
    }
    for (u32 i = 0; i < materialWatermark; i++)
    {
        const MaterialGPU* material = &materials[i];
        if (material->albedoDescriptor >= numDescriptors || material->normalDescriptor >= numDescriptors ||
            material->metallicRoughnessDescriptor >= numDescriptors)
        {
            AX_ERROR("baked material descriptor out of range: material=%d descriptors=%d", i, numDescriptors);
            return 0;
        }
    }

    double startTime = TimeSinceStartup();
    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        if (!atlasPaths[c]) continue;
        if (!RestoreBakedClass(ts, c, atlasPaths[c]))
            return 0;
    }

    MemCopy(ts->descriptors, descriptors, numDescriptors * sizeof(TextureDescriptor));
    ts->numDescriptors = numDescriptors;
    RebuildDescriptorSlotsFromMaterials(ts, materials, materialWatermark);
    UpdateDescriptorWatermark(ts);
    UpdateGPUBuffer(ts->descriptorBuffer, ts->descriptors, ts->numDescriptors * sizeof(TextureDescriptor), 0);

    if (materialWatermark > 0)
    {
        MemCopy(ts->materials, materials, materialWatermark * sizeof(MaterialGPU));
        ts->materialWatermark = materialWatermark;
        UpdateGPUBuffer(ts->materialBuffer, ts->materials, materialWatermark * sizeof(MaterialGPU), 0);
    }

    AX_LOG("texture system restored from baked atlases descriptors=%d watermark=%d %.2fs",
           ts->numDescriptors, ts->materialWatermark, TimeSinceStartup() - startTime);
    return 1;
}
