#ifndef _GRAPHICS_
#define _GRAPHICS_

#include "Include/Graphics.h"
#include "Include/Rendering.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"
#include "Include/Algorithm.h"
#include "Include/Memory.h"
#include "Include/BasisBinding.h"
#include "Include/Random.h"
#include "Source/Terrain/TerrainInternal.h"

#if !defined(PLATFORM_MACOSX)
#include "Extern/SDL3/src/video/khronos/vulkan/vulkan.h"
#endif

#include "Extern/sinfl.h"
#include "Extern/ufbx.h"
#include "Extern/tlsf.h"
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_atomic.h>

#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM 

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#define STBI_MALLOC(size)           ( AllocateTLSFGlobal(size) )
#define STBI_FREE(ptr)              ( DeAllocateTLSFGlobal(ptr) )
#define STBI_REALLOC(ptr, size)     ( ReAllocateTLSFGlobal(ptr, size) )

#define STBIR_MALLOC(size, c)       ( AllocateTLSFGlobal(size) )
#define STBIR_FREE(ptr, c)          ( (void)(c), DeAllocateTLSFGlobal(ptr) )

#if !defined(PLATFORM_MACOSX)
#define VULKAN_MAKE_API_VERSION(variant, major, minor, patch) \
    ((((uint32_t)(variant)) << 29U) | (((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
#endif

#include "Extern/stb/stb_image.h"
#include "Extern/stb/stb_image_resize2.h"

extern SDL_GPUDevice* g_GPUDevice;
extern WindowState    g_WindowState;
extern RenderState    g_RenderState;
extern SDL_Window*    g_SDLWindow;

Graphics gGFX = {0};

extern void Quit(int rc);

/*//////////////////////////////////////////////////////////////////////////*/
/*                              Geometry Heaps                               */
/*//////////////////////////////////////////////////////////////////////////*/

// tlsf instances managing the cpu mega buffers directly, the headers live
// inside the buffers between bundle ranges
static tlsf_t g_GeometryTLSF[GeometryBuffer_Count];
// editor mesh import allocates/shrinks geometry ranges from a worker thread, so serialize the
// heap mutations against main-thread bundle add/remove. one tlsf call per critical section.
static SDL_SpinLock g_GeometryHeapLock;

static char* GeometryHeapBase(GeometryBufferKind kind)
{
    switch (kind)
    {
        case GeometryBuffer_SkinnedVertex: return (char*)gGFX.SkinnedVertexBuffer;
        case GeometryBuffer_SurfaceVertex: return (char*)gGFX.SurfaceVertexBuffer;
        case GeometryBuffer_TerrainVertex: return (char*)gGFX.TerrainVertexBuffer;
        case GeometryBuffer_TerrainIndex:  return (char*)gGFX.TerrainIndexBuffer;
        default:                           return (char*)gGFX.IndexBuffer;
    }
}

static u32 GeometryHeapStride(GeometryBufferKind kind)
{
    switch (kind)
    {
        case GeometryBuffer_SkinnedVertex: return sizeof(ASkinedVertex);
        case GeometryBuffer_SurfaceVertex: return sizeof(AVertex);
        case GeometryBuffer_TerrainVertex: return sizeof(TerrainVertex);
        default:                           return sizeof(u32);
    }
}

static void InitGeometryHeaps(void)
{
    const size_t poolBytes[GeometryBuffer_Count] = {
        sizeof(ASkinedVertex) * MAX_SKINNED_SOURCE_VERTEX,
        sizeof(AVertex) * MAX_SURFACE_VERTEX,
        sizeof(u32) * MAX_INDEX,
        sizeof(TerrainVertex) * TERRAIN_MAX_VERTICES,
        sizeof(u32) * TERRAIN_MAX_INDICES
    };

    for (u32 kind = 0; kind < GeometryBuffer_Count; kind++)
    {
        void* control = AllocateTLSFGlobal(tlsf_size());
        tlsf_t tlsf = control ? tlsf_create(control) : NULL;
        if (!tlsf || !tlsf_add_pool(tlsf, GeometryHeapBase((GeometryBufferKind)kind), poolBytes[kind]))
        {
            AX_ERROR("geometry heap init failed kind=%d", kind);
            if (!tlsf) DeAllocateTLSFGlobal(control);
            continue;
        }
        g_GeometryTLSF[kind] = tlsf;
    }
}

static void DestroyGeometryHeaps(void)
{
    for (u32 kind = 0; kind < GeometryBuffer_Count; kind++)
    {
        if (g_GeometryTLSF[kind]) DeAllocateTLSFGlobal(g_GeometryTLSF[kind]);
        g_GeometryTLSF[kind] = NULL;
    }
}

u32 GeometryHeapAlloc(GeometryBufferKind kind, u32 count, void** raw)
{
    *raw = NULL;
    if (count == 0 || !g_GeometryTLSF[kind]) return GEOMETRY_ALLOC_FAIL;

    // over allocate by stride-1 so the data start can be rounded up to the
    // element stride, the strides are not power of two so tlsf can't align it.
    // memalign with the minimum tlsf alignment, this fork's tlsf_malloc is
    // 16 aligned which takes a gap path that wastes 48 bytes per allocation
    size_t stride = GeometryHeapStride(kind);
    size_t bytes = (size_t)count * stride + (stride - 1u);
    SDL_LockSpinlock(&g_GeometryHeapLock);
    void* ptr = tlsf_memalign(g_GeometryTLSF[kind], tlsf_align_size(), bytes);
    SDL_UnlockSpinlock(&g_GeometryHeapLock);
    if (!ptr) return GEOMETRY_ALLOC_FAIL;

    size_t rawOffset = (size_t)((char*)ptr - GeometryHeapBase(kind));
    *raw = ptr;
    return (u32)((rawOffset + stride - 1u) / stride);
}

void GeometryHeapShrink(GeometryBufferKind kind, void* raw, u32 offset, u32 newCount)
{
    if (!raw || newCount == 0 || !g_GeometryTLSF[kind]) return;
    size_t stride = GeometryHeapStride(kind);
    size_t dataEnd = ((size_t)offset + newCount) * stride;
    size_t newBytes = dataEnd - (size_t)((char*)raw - GeometryHeapBase(kind));
    SDL_LockSpinlock(&g_GeometryHeapLock);
    void* ptr = tlsf_realloc(g_GeometryTLSF[kind], raw, newBytes);
    SDL_UnlockSpinlock(&g_GeometryHeapLock);
    ASSERT(ptr == raw); // shrink trims in place, never moves
    (void)ptr;
}

void GeometryHeapFree(GeometryBufferKind kind, void* raw)
{
    if (!raw || !g_GeometryTLSF[kind]) return;
    SDL_LockSpinlock(&g_GeometryHeapLock);
    tlsf_free(g_GeometryTLSF[kind], raw);
    SDL_UnlockSpinlock(&g_GeometryHeapLock);
}

// samples must be 1, 2, 4, or 8
static SDL_GPUSampleCount SampleCountFromValue(u32 samples)
{
    return (SDL_GPUSampleCount)TrailingZeroCount32(samples);
}

static u32 SampleCountValue(SDL_GPUSampleCount sampleCount)
{
    return 1u << (u32)sampleCount;
}

bool GraphicsApplyMSAASettings(void)
{
    u32 requested = g_RenderSettings.msaaSamples;
    if      (requested >= 8u) requested = 8u;
    else if (requested >= 4u) requested = 4u;
    else if (requested >= 2u) requested = 2u;
    else requested = 1u;

    SDL_GPUSampleCount selected = SDL_GPU_SAMPLECOUNT_1;
	for (u32 i = requested; i > 0u; i >>= 1)
	{
		SDL_GPUSampleCount candidate = SampleCountFromValue(i);

		if (candidate == SDL_GPU_SAMPLECOUNT_1 ||
			(SDL_GPUTextureSupportsSampleCount(g_GPUDevice, TEX_FMT_HALF4, candidate) &&
			 SDL_GPUTextureSupportsSampleCount(g_GPUDevice, TEX_FMT_D32_FLT, candidate)))
		{
			selected = candidate;
			break;
		}
	}

    u32 selectedSamples = SampleCountValue(selected);
    if (selectedSamples != requested)
        AX_WARN("Requested MSAA %ux unsupported, using %ux", requested, selectedSamples);
    g_RenderSettings.msaaSamples = selectedSamples;

    if (g_RenderState.sceneSampleCount == selected) return false;
    g_RenderState.sceneSampleCount = selected;
    return true;
}

u32 GraphicsGetActiveMSAASamples(void)
{
    return SampleCountValue(g_RenderState.sceneSampleCount);
}

void GraphicsInit(bool msaa)
{
#if defined(PLATFORM_MACOSX)
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "metal");
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "metal");
#else
    VkPhysicalDeviceFeatures vk10_features = { .shaderInt16 = VK_TRUE };

    VkPhysicalDeviceVulkan12Features vk12_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = NULL,
        .shaderFloat16 = VK_TRUE,
    };

    VkPhysicalDeviceVulkan11Features vk11_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &vk12_features,
        .shaderDrawParameters = VK_TRUE,
    };

    SDL_GPUVulkanOptions options = {
        .vulkan_api_version = VULKAN_MAKE_API_VERSION(0, 1, 2, 0),
        .feature_list = &vk11_features,
        .vulkan_10_physical_device_features = &vk10_features,
    };

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &options);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
#endif
    g_GPUDevice = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);

    CHECK_CREATE(g_GPUDevice, "GPU device");
    if (!SDL_SetGPUAllowedFramesInFlight(g_GPUDevice, 3u))
    {
        AX_WARN("Failed to set GPU frames in flight: %s", SDL_GetError());
    }
    SDL_ClaimWindowForGPUDevice(g_GPUDevice, g_SDLWindow);
    
    if (!msaa) g_RenderSettings.msaaSamples = 1u;
    GraphicsApplyMSAASettings();
    CreateWindowBuffers();
    WindowState* winstate = &g_WindowState;
    winstate->tex_shadow_depth       = CreateTexture2D(SHADOW_MAP_SIZE  , SHADOW_MAP_SIZE  , TEX_FMT_D32_FLT, TEX_DEPTH_STENCIL, TEX_SMP_CNT1, SHADOW_CASCADE_COUNT, "Shadow Depth Texture");
    winstate->tex_shadow_color       = CreateTexture2D(SHADOW_MAP_SIZE  , SHADOW_MAP_SIZE  , TEX_FMT_R32_FLT, TEX_COLOR_TARGET | TEX_SAMPLER, TEX_SMP_CNT1, SHADOW_CASCADE_COUNT, "Shadow Color Texture");
    winstate->tex_point_shadow_depth = CreateTexture2D(POINT_SHADOW_ATLAS_WIDTH, POINT_SHADOW_SIZE, TEX_FMT_D32_FLT, TEX_DEPTH_STENCIL, TEX_SMP_CNT1, 1, "Point Shadow Depth Texture");
    winstate->tex_spot_shadow_depth  = CreateTexture2D(SPOT_SHADOW_SIZE , SPOT_SHADOW_SIZE , TEX_FMT_D32_FLT, TEX_DEPTH_STENCIL, TEX_SMP_CNT1, 1, "Spot Shadow Depth Texture");
    winstate->tex_point_shadow_color = CreateTexture2DArray(POINT_SHADOW_ATLAS_WIDTH, POINT_SHADOW_SIZE, POINT_SHADOW_MAX_LIGHTS, TEX_FMT_R32_FLT, TEX_COLOR_TARGET | TEX_SAMPLER, "Point Shadow Depth Texture");
    winstate->tex_spot_shadow_color  = CreateTexture2DArray(SPOT_SHADOW_SIZE , SPOT_SHADOW_SIZE , POINT_SHADOW_LAYER_COUNT, TEX_FMT_R32_FLT, TEX_COLOR_TARGET | TEX_SAMPLER, "Spot Shadow Depth Texture");
    
    g_RenderState.skyNoise3D = Create3DNoise3DTexture(64u);
    gGFX.SkinnedVertexBuffer = OSAllocAligned(sizeof(ASkinedVertex) * MAX_SKINNED_SOURCE_VERTEX, 4);
    gGFX.SurfaceVertexBuffer = OSAllocAligned(sizeof(AVertex) * MAX_SURFACE_VERTEX, 4);
    gGFX.TerrainVertexBuffer = OSAllocAligned(sizeof(TerrainVertex) * TERRAIN_MAX_VERTICES, 4);
    gGFX.TerrainIndexBuffer  = OSAllocAligned(sizeof(u32) * TERRAIN_MAX_INDICES, 4);
    gGFX.IndexBuffer         = OSAllocAligned(sizeof(u32) * MAX_INDEX + 16, 4); // 16->give little bit of space for memcpy
    if (!gGFX.SkinnedVertexBuffer || !gGFX.SurfaceVertexBuffer || !gGFX.TerrainVertexBuffer || !gGFX.TerrainIndexBuffer || !gGFX.IndexBuffer)
        AX_ERROR("graphics CPU buffer allocation failed skinned=%p surface=%p terrain=%p terrainIndex=%p index=%p", gGFX.SkinnedVertexBuffer, gGFX.SurfaceVertexBuffer, gGFX.TerrainVertexBuffer, gGFX.TerrainIndexBuffer, gGFX.IndexBuffer);

    InitGeometryHeaps();
}

static u32 g_SceneViewWidth, g_SceneViewHeight; // 0 = scene renders fullscreen

void SetSceneViewSize(u32 width, u32 height)
{
    g_SceneViewWidth = width;
    g_SceneViewHeight = height;
}

bool GetSceneViewSize(u32* w, u32* h)
{
    if (g_SceneViewWidth == 0u || g_SceneViewHeight == 0u) return false;
    *w = g_SceneViewWidth;
    *h = g_SceneViewHeight;
    return true;
}

void GetRenderResolution(u32 windowW, u32 windowH, u32* outW, u32* outH)
{
    GetSceneViewSize(&windowW, &windowH);
    f32 scale = g_RenderSettings.renderScale;
    if (scale <= 0.0f) scale = 1.0f; // unset settings render at native resolution
    scale = Clampf32(scale, 0.25f, 2.0f);
    *outW = Maxu32((u32)((f32)windowW * scale + 0.5f), 1u);
    *outH = Maxu32((u32)((f32)windowH * scale + 0.5f), 1u);
}

static u32 GetMipCount(u32 width, u32 height);
static u32 GetBloomMipSize(u32 size, u32 mip);

void CreateWindowBuffers()
{
    WindowState* winstate = &g_WindowState;
    int windowW, windowH;
    /* create a depth texture for the window */
    SDL_GetWindowSizeInPixels(g_SDLWindow, (int*)&windowW, (int*)&windowH);
    u32 width, height;
    GetRenderResolution((u32)windowW, (u32)windowH, &width, &height);
    winstate->render_width  = width;
    winstate->render_height = height;
    u32 hbaoWidth  = Maxu32(width / 2u, 1u);
    u32 hbaoHeight = Maxu32(height / 2u, 1u);
    u32 bloomWidth  = Maxu32(width / 2u, 1u);
    u32 bloomHeight = Maxu32(height / 2u, 1u);
    u32 bloomMipCount = Minu32(GetMipCount(bloomWidth, bloomHeight), BLOOM_MAX_MIPS);
    SDL_GPUSampleCount sceneSampleCount = g_RenderState.sceneSampleCount ? g_RenderState.sceneSampleCount : SDL_GPU_SAMPLECOUNT_1;
    bool msaa = sceneSampleCount != SDL_GPU_SAMPLECOUNT_1;
    winstate->tex_color           = CreateSceneColorTexture(width, height, SDL_GPU_SAMPLECOUNT_1);
    winstate->tex_color_msaa      = msaa ? CreateSceneColorTexture(width, height, sceneSampleCount) : NULL;
    winstate->tex_depth_msaa      = msaa ? CreateTexture2D(width, height, TEX_FMT_D32_FLT, TEX_DEPTH_STENCIL, sceneSampleCount, 1, "MSAA Depth Texture") : NULL;
    winstate->tex_depth           = CreateTexture2D(width, height, TEX_FMT_D32_FLT, TEX_DEPTH_STENCIL | TEX_SAMPLER, TEX_SMP_CNT1, 1, "Depth Texture");
    winstate->tex_post            = CreateTexture2D(width, height, TEX_FMT_8UNORM4, TEX_SAMPLER | TEX_COLOR_TARGET | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "Post Process Texture");
    winstate->tex_hbao            = CreateTexture2D(hbaoWidth, hbaoHeight, TEX_FMT_8UNORM1, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "HBAO Texture");
    winstate->tex_hbao_blur       = CreateTexture2D(hbaoWidth, hbaoHeight, TEX_FMT_8UNORM1, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "HBAO Texture");
    winstate->tex_hbao_normal     = CreateTexture2D(hbaoWidth, hbaoHeight, TEX_FMT_8UNORM4, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "HBAO Normal Texture");
    winstate->tex_contact_shadow  = CreateTexture2D(hbaoWidth, hbaoHeight, TEX_FMT_8UNORM1, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "Contact Shadow Texture");
    for (u32 mip = 0; mip < bloomMipCount; mip++)
    {
        u32 mipWidth = GetBloomMipSize(bloomWidth, mip);
        u32 mipHeight = GetBloomMipSize(bloomHeight, mip);
        winstate->tex_bloom_downsample[mip] = CreateTexture2D(mipWidth, mipHeight, TEX_FMT_HALF4, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "Bloom Downsample Texture");
        if (mip + 1u < bloomMipCount)
            winstate->tex_bloom_upsample[mip] = CreateTexture2D(mipWidth, mipHeight, TEX_FMT_HALF4, TEX_SAMPLER | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "Bloom Upsample Texture");
    }
    winstate->tex_bloom_ping      = winstate->tex_bloom_downsample[0];
    winstate->tex_bloom_pong      = (bloomMipCount > 1u) ? winstate->tex_bloom_upsample[0] : winstate->tex_bloom_downsample[0];
    winstate->tex_mlaa_edge_mask  = CreateTexture2D(width, height, TEX_FMT_R32_UINT, TEX_COMP_READ | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "MLAA Edge Mask Texture");
    winstate->tex_mlaa_edge_count = CreateTexture2D(width, height, TEX_FMT_D32_FLT2, TEX_COMP_READ | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "MLAA Edge Count Texture");
    winstate->tex_mlaa_output     = CreateTexture2D(width, height, TEX_FMT_8UNORM4 , TEX_SAMPLER | TEX_COLOR_TARGET | TEX_COMP_WRITE, TEX_SMP_CNT1, 1, "MLAA Output Texture");
    winstate->tex_hiz_depth = CreateHiZDepthTexture(width, height);
    winstate->tex_hiz       = CreateHiZTexture(width, height, &winstate->hiz_mip_count);
    winstate->hiz_width     = width;
    winstate->hiz_height    = height;
    winstate->hiz_valid     = false;
    winstate->bloom_width   = bloomWidth;
    winstate->bloom_height  = bloomHeight;
    winstate->bloom_mip_count = bloomMipCount;
}

SDL_GPUBuffer* CreateBuffer(
    const void*          buffer,       // may be NULL
    size_t               bufferSize,
    SDL_GPUBufferUsageFlags bufferUsage,
    const char*          debugName)
{
    // Create the GPU buffer (no initial data)
    SDL_GPUBufferCreateInfo buffer_desc = {
        .usage = bufferUsage,
        .size  = bufferSize,
        .props = SDL_CreateProperties()
    };
    SDL_SetStringProperty(buffer_desc.props, SDL_PROP_GPU_BUFFER_CREATE_NAME_STRING, debugName);
    SDL_GPUBuffer* gpu_buffer = SDL_CreateGPUBuffer(g_GPUDevice, &buffer_desc);
    CHECK_CREATE(gpu_buffer, debugName);
    SDL_DestroyProperties(buffer_desc.props);

    // If no initial data was provided, we're done
    if (buffer == NULL) 
        return gpu_buffer;

    // --- Upload initial data using a transfer buffer ---
    SDL_GPUTransferBufferCreateInfo transfer_desc = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = bufferSize,
        .props = SDL_CreateProperties()
    };
    SDL_SetStringProperty(transfer_desc.props, SDL_PROP_GPU_TRANSFERBUFFER_CREATE_NAME_STRING, "Upload Transfer");
    SDL_GPUTransferBuffer* upload_buf = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transfer_desc);
    CHECK_CREATE(upload_buf, "Transfer buffer");
    SDL_DestroyProperties(transfer_desc.props);

    // Map and copy data
    void* map = SDL_MapGPUTransferBuffer(g_GPUDevice, upload_buf, false);
    MemCopy(map, buffer, bufferSize);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, upload_buf);

    // Copy to GPU buffer
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src_location = { .transfer_buffer = upload_buf, .offset = 0 };
    SDL_GPUBufferRegion dst_region = { .buffer = gpu_buffer, .offset = 0, .size  = bufferSize };
    SDL_UploadToGPUBuffer(copy_pass, &src_location, &dst_region, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);

    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, upload_buf);
    return gpu_buffer;
}

void UpdateGPUBufferCycle(SDL_GPUBuffer* buffer, const void* data, size_t bufferSize, size_t offset, bool cycle)
{
    SDL_GPUTransferBufferCreateInfo transferBufferCreateInfo;
    transferBufferCreateInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferBufferCreateInfo.size  = bufferSize;
    transferBufferCreateInfo.props = 0;

    SDL_GPUTransferBuffer* dataTransferBuffer;
    Uint8* dataTransferPtr;
    SDL_GPUCommandBuffer* uploadCmdBuf = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmdBuf);
    
    dataTransferBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transferBufferCreateInfo);
    dataTransferPtr = (Uint8*)SDL_MapGPUTransferBuffer(g_GPUDevice, dataTransferBuffer, true);
    MemCopy(dataTransferPtr, data, bufferSize);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, dataTransferBuffer);

    SDL_UploadToGPUBuffer(copyPass,
                          &(SDL_GPUTransferBufferLocation) { .transfer_buffer = dataTransferBuffer, .offset = 0}, 
                          &(SDL_GPUBufferRegion) { .buffer = buffer,  .offset = offset,  .size = bufferSize }, 
                          cycle);
    
    SDL_EndGPUCopyPass(copyPass);
    
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(uploadCmdBuf);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, dataTransferBuffer);
}

void UpdateGPUBuffer(SDL_GPUBuffer* buffer, const void* data, size_t bufferSize, size_t offset)
{
    UpdateGPUBufferCycle(buffer, data, bufferSize, offset, false);
}

static u32 GetMipCount(u32 width, u32 height)
{
    u32 levels = 1;
    u32 size = width > height ? width : height;
    while (size > 16) {
        size >>= 1u;
        levels++;
    }
    return levels;
}

static u32 GetBloomMipSize(u32 size, u32 mip)
{
    return Maxu32(size >> mip, 1u);
}

SDL_GPUTexture* CreateTexture2D(u32 width, u32 height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                SDL_GPUSampleCount sampleCount, u32 mipLevels, const char* label)
{
    SDL_GPUTextureCreateInfo createinfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = format,
        .usage = usage,
        .width = width,
        .height = height,
        .layer_count_or_depth = 1,
        .num_levels = mipLevels,
        .sample_count = sampleCount,
        .props = 0
    };

    SDL_GPUTexture* result = SDL_CreateGPUTexture(g_GPUDevice, &createinfo);
    CHECK_CREATE(result, label)
    return result;
}

SDL_GPUTexture* CreateTexture2DArray(u32 width, u32 height, u32 layers, SDL_GPUTextureFormat format,
                                     SDL_GPUTextureUsageFlags usage, const char* label)
{
    SDL_GPUTextureCreateInfo createinfo = {
        .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
        .format = format,
        .usage = usage,
        .width = width,
        .height = height,
        .layer_count_or_depth = layers,
        .num_levels = 1,
        .sample_count = TEX_SMP_CNT1,
        .props = 0
    };

    SDL_GPUTexture* result = SDL_CreateGPUTexture(g_GPUDevice, &createinfo);
    CHECK_CREATE(result, label)
    return result;
}

SDL_GPUTexture* Create3DNoise3DTexture(u32 size)
{
    SDL_GPUTextureCreateInfo createinfo = {
        .type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
        .format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
        .usage = TEX_SAMPLER,
        .width = size,
        .height = size,
        .layer_count_or_depth = size,
        .num_levels = 1,
        .sample_count = TEX_SMP_CNT1,
        .props = 0
    };

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(g_GPUDevice, &createinfo);
    CHECK_CREATE(texture, "Sky 3D Noise Texture");

    u32 voxelCount = size * size * size;
    u64* data = (u64*)ArenaPushGlobal(voxelCount);
    for (u32 z = 0; z < size * size * (size / sizeof(u64)); z++)
        data[z] = MurmurHash(z);

    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = voxelCount
    });
    CHECK_CREATE(transferBuffer, "Sky 3D Noise Transfer Buffer");
    u8* mapped = (u8*)SDL_MapGPUTransferBuffer(g_GPUDevice, transferBuffer, false);
    MemCopy(mapped, data, voxelCount);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, transferBuffer);
    ArenaPopGlobal(voxelCount);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    for (u32 layer = 0; layer < size; layer++)
    {
        SDL_UploadToGPUTexture(copyPass,
            &(SDL_GPUTextureTransferInfo){ .transfer_buffer = transferBuffer, .offset = layer * size * size, .pixels_per_row = size, .rows_per_layer = size },
            &(SDL_GPUTextureRegion){ .texture = texture, .mip_level = 0, .layer = layer, .x = 0, .y = 0, .z = 0, .w = size, .h = size, .d = 1 },
            false);
    }
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transferBuffer);
    return texture;
}

SDL_GPUTexture* CreateSceneColorTexture(u32 drawablew, u32 drawableh, SDL_GPUSampleCount sampleCount)
{
    SDL_GPUTextureUsageFlags usage = TEX_COLOR_TARGET;
    if (sampleCount == TEX_SMP_CNT1) usage |= TEX_SAMPLER | TEX_COMP_WRITE;
    return CreateTexture2D(drawablew, drawableh, TEX_FMT_HALF4, usage, sampleCount, 1, "Scene Color Texture");
}


SDL_GPUTexture* CreateHiZDepthTexture(u32 drawablew, u32 drawableh)
{
    return CreateTexture2D(drawablew, drawableh, TEX_FMT_R32_FLT, TEX_COLOR_TARGET | TEX_SAMPLER, TEX_SMP_CNT1, 1, "Hi-Z Resolved Depth Texture");
}

SDL_GPUTexture* CreateHiZTexture(u32 drawablew, u32 drawableh, u32* mipCount)
{
    u32 levels = GetMipCount(drawablew, drawableh);
    if (mipCount) *mipCount = levels;
    return CreateTexture2D(drawablew, drawableh, SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        SDL_GPU_SAMPLECOUNT_1, levels, "Hi-Z Texture");
}

Texture rImportTexture(const char* path, TexFlags flags, const char* label)
{
    int width, height, channels;
    unsigned char* image = NULL;
    Texture defTexture;
    defTexture.width  = 32;
    defTexture.height = 32;
    defTexture.handle = 0;
    defTexture.type = 0;
    defTexture.mipLevels = 1;

    if (!FileExist(path)) {
        AX_ERROR("image is not exist, using default texture! %s", path);
        return defTexture;
    }

    AFile asset = AFileOpen(path, AOpenFlag_ReadBinary);
    u64 size = AFileSize(asset);

    void* textureLoadBuffer = ArenaPushGlobal(size);
    
    AFileRead(textureLoadBuffer, size, asset, 1);
    image = stbi_load_from_memory(textureLoadBuffer, (int)size, &width, &height, &channels, 4);
    ArenaPopGlobal(size);

    AFileClose(asset);
    
    if (image == NULL) {
        AX_ERROR("image load failed! %s", path);
        return defTexture;
    }
    
    (void)channels;
    SDL_GPUTextureUsageFlags usage = TEX_SAMPLER;
    if (flags & TexFlags_MipMap) usage |= TEX_COLOR_TARGET;
    Texture texture = rCreateTexture(width, height, image, TEX_FMT_8UNORM4, flags, usage, label); 
    texture.type = 0;
    
    bool delBuff = (flags & TexFlags_DontDeleteCPUBuffer) == 0;
    if (delBuff)
    {
        stbi_image_free(image);
        texture.buffer = NULL;
    }
    return texture;
}

static Texture rCreateTextureEx(
    int width,
    int height,
    int layers,
    void* data,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    TexFlags flags,
    const char* label)
{
    bool isArray = layers > 1;

    u32 mipLevels = 1;
    if (flags & TexFlags_MipMap)
    {
        u32 maxDim = (u32)(width > height ? width : height);
        mipLevels = Log2u32(maxDim) + 1;
    }
    
    SDL_GPUTextureCreateInfo texDesc = {
        .type                 = isArray ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D,
        .format               = format,
        .width                = (u32)width,
        .height               = (u32)height,
        .layer_count_or_depth = (u32)layers,
        .num_levels           = mipLevels,
        .sample_count         = TEX_SMP_CNT1,
        .usage                = usage,
        .props                = SDL_CreateProperties()
    };

    if (label)
        SDL_SetStringProperty(texDesc.props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, label);

    Texture res = {0};
    res.width  = width;
    res.height = height;
    res.format = format;
    res.buffer = data;
    res.mipLevels = mipLevels;
    res.handle = SDL_CreateGPUTexture(g_GPUDevice, &texDesc);

    SDL_DestroyProperties(texDesc.props);

    if (!res.handle)
    {
        AX_WARN("SDL_CreateGPUTexture failed!");
        return res;
    }

    if (data)
    {
        u32 blockSize  = SDL_GPUTextureFormatTexelBlockSize(format);
        u32 layerSize  = blockSize * (u32)width * (u32)height;
        u32 uploadSize = layerSize * (u32)layers;

        SDL_GPUTransferBufferCreateInfo transferDesc = {
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size  = uploadSize
        };

        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transferDesc);

        Uint8* dst = (Uint8*)SDL_MapGPUTransferBuffer(g_GPUDevice, transferBuffer, false);

        MemCopy(dst, data, uploadSize);

        SDL_UnmapGPUTransferBuffer(g_GPUDevice, transferBuffer);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

        SDL_GPUTextureTransferInfo transferInfo = {
            .transfer_buffer = transferBuffer,
            .offset          = 0,
            .pixels_per_row  = (u32)width,
            .rows_per_layer  = (u32)height
        };

        SDL_GPUTextureRegion region = {
            .texture   = res.handle,
            .mip_level = 0,
            .layer     = 0,
            .x         = 0,
            .y         = 0,
            .z         = 0,
            .w         = (u32)width,
            .h         = (u32)height,
            .d         = isArray ? 1 : (u32)layers
        };

        if (isArray)
        {
            for (u32 layer = 0; layer < (u32)layers; layer++)
            {
                transferInfo.offset = layerSize * layer;
                region.layer = layer;
                region.d = 1;
                SDL_UploadToGPUTexture(copyPass, &transferInfo, &region, false);
            }
        }
        else
        {
            SDL_UploadToGPUTexture(copyPass, &transferInfo, &region, false);
        }

        SDL_EndGPUCopyPass(copyPass);

        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
        SDL_ReleaseGPUFence(g_GPUDevice, fence);

        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transferBuffer);
    }

    if (data && (flags & TexFlags_MipMap))
    {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);

        SDL_GenerateMipmapsForGPUTexture(cmd, res.handle);

        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
        SDL_ReleaseGPUFence(g_GPUDevice, fence);
    }

    return res;
}

Texture rCreateTexture(int width, int height, void* data, SDL_GPUTextureFormat format,
                       TexFlags flags, SDL_GPUTextureUsageFlags usage, const char* label)
{
    return rCreateTextureEx(width, height, 1, data, format, usage, flags, label);
}

Texture rCreateTexture2DArray(int width, int height, int layers, void* data, SDL_GPUTextureFormat format, 
                              TexFlags flags, SDL_GPUTextureUsageFlags usage, const char* label)
{
    return rCreateTextureEx(width, height, layers, data, format, usage, flags, label);
}

void UploadTextureRegion(Texture texture, u32 layer, u32 x, u32 y, u32 width, u32 height, u32 srcWidth, u32 srcHeight, const void* data)
{
    if (!texture.handle || !data || width == 0 || height == 0) return;

    u32 blockSize = SDL_GPUTextureFormatTexelBlockSize(texture.format);
    SDL_GPUTransferBufferCreateInfo transferDesc = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = (u32)(srcWidth * srcHeight * blockSize)
    };
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transferDesc);
    Uint8* dst = (Uint8*)SDL_MapGPUTransferBuffer(g_GPUDevice, transferBuffer, false);
    MemCopy(dst, data, transferDesc.size);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, transferBuffer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo transferInfo = {
        .transfer_buffer = transferBuffer,
        .offset = 0,
        .pixels_per_row = srcWidth,
        .rows_per_layer = srcHeight
    };
    SDL_GPUTextureRegion region = {
        .texture = texture.handle,
        .mip_level = 0,
        .layer = layer,
        .x = x,
        .y = y,
        .z = 0,
        .w = width,
        .h = height,
        .d = 1
    };
    SDL_UploadToGPUTexture(copyPass, &transferInfo, &region, false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transferBuffer);
}

void GenerateTextureMips(Texture texture)
{
    if (!texture.handle) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GenerateMipmapsForGPUTexture(cmd, texture.handle);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
}

void ReleaseTexture(Texture* texture)
{
    SDL_ReleaseGPUTexture(g_GPUDevice, texture->handle);
    texture->handle = NULL;
    texture->width = texture->height = 0;
}

void rDeleteTexture(Texture texture)
{

}


void GraphicsDestroy()
{
    WindowState* winstate = &g_WindowState;
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_depth);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_hiz_depth);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_color);
    if (winstate->tex_color_msaa) SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_color_msaa);
    if (winstate->tex_depth_msaa) SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_depth_msaa);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_post);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_hiz);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_hbao);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_hbao_blur);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_hbao_normal);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_contact_shadow);
    for (u32 i = 0; i < BLOOM_MAX_MIPS; i++)
    {
        if (winstate->tex_bloom_downsample[i]) SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_bloom_downsample[i]);
        if (winstate->tex_bloom_upsample[i])   SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_bloom_upsample[i]);
    }
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_mlaa_edge_mask);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_mlaa_edge_count);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_mlaa_output);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_shadow_depth);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_shadow_color);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_point_shadow_depth);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_point_shadow_color);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_spot_shadow_depth);
    SDL_ReleaseGPUTexture(g_GPUDevice, winstate->tex_spot_shadow_color);
    SDL_ReleaseGPUTexture(g_GPUDevice, g_RenderState.skyNoise3D);
    SDL_ReleaseWindowFromGPUDevice(g_GPUDevice, g_SDLWindow);

    SDL_DestroyGPUDevice(g_GPUDevice);
    DestroyGeometryHeaps();
    OSFreeAligned(gGFX.SkinnedVertexBuffer, sizeof(ASkinedVertex) * MAX_SKINNED_SOURCE_VERTEX);
    OSFreeAligned(gGFX.SurfaceVertexBuffer, sizeof(AVertex) * MAX_SURFACE_VERTEX);
    OSFreeAligned(gGFX.TerrainVertexBuffer, sizeof(TerrainVertex) * TERRAIN_MAX_VERTICES);
    OSFreeAligned(gGFX.TerrainIndexBuffer , sizeof(u32) * TERRAIN_MAX_INDICES);
    OSFreeAligned(gGFX.IndexBuffer        , sizeof(u32) * MAX_INDEX + 16);
    gGFX.SkinnedVertexBuffer = NULL;
    gGFX.SurfaceVertexBuffer = NULL;
    gGFX.TerrainVertexBuffer = NULL;
    gGFX.TerrainIndexBuffer = NULL;
    gGFX.IndexBuffer = NULL;
}



#endif
