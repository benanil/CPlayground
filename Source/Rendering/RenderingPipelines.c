#include "RenderingInternal.h"
#include "Source/Terrain/TerrainInternal.h"
#include "Include/Slug.h"

#if defined(PLATFORM_MACOSX)
#include "Shaders/msl/SurfaceForwardFrag.msl.h"
#include "Shaders/msl/SurfaceForwardVert.msl.h"
#include "Shaders/msl/SkinnedForwardFrag.msl.h"
#include "Shaders/msl/SkinnedForwardVert.msl.h"
#include "Shaders/msl/ReconstructNormalCompute.msl.h"
#include "Shaders/msl/PreProcessing/BuildLightGridCompute.msl.h"
#include "Shaders/msl/PreProcessing/CullDrawArgsCompute.msl.h"
#include "Shaders/msl/Animation/AnimationCompute.msl.h"
#include "Shaders/msl/Animation/AnimateVertices.msl.h"
#include "Shaders/msl/LineDebugVert.msl.h"
#include "Shaders/msl/LineDebugFrag.msl.h"
#include "Shaders/msl/OutlineVert.msl.h"
#include "Shaders/msl/OutlineFrag.msl.h"
#include "Shaders/msl/UI/SlugVert.msl.h"
#include "Shaders/msl/Slug2DVert.msl.h"
#include "Shaders/msl/UI/SlugFrag.msl.h"
#include "Shaders/msl/UI/UIShapeVert.msl.h"
#include "Shaders/msl/UI/UIShapeFrag.msl.h"
#include "Shaders/msl/UI/UIImageVert.msl.h"
#include "Shaders/msl/UI/UIImageFrag.msl.h"
#include "Shaders/msl/PostProcessing/TonemapCompute.msl.h"
#include "Shaders/msl/PostProcessing/BloomDownsampleSPDCompute.msl.h"
#include "Shaders/msl/PostProcessing/BloomUpsampleCompute.msl.h"
#include "Shaders/msl/PreProcessing/HiZBuildCompute.msl.h"
#include "Shaders/msl/PreProcessing/HiZDownscaleSPDCompute.msl.h"
#include "Shaders/msl/PostProcessing/HBAOCompute.msl.h"
#include "Shaders/msl/PostProcessing/HBAOBlurCompute.msl.h"
#include "Shaders/msl/PostProcessing/ContactShadowsCompute.msl.h"
#include "Shaders/msl/PostProcessing/MLAAEdgeMaskCompute.msl.h"
#include "Shaders/msl/PostProcessing/MLAALineLengthCompute.msl.h"
#include "Shaders/msl/PostProcessing/MLAABlendCompute.msl.h"
#include "Shaders/msl/SurfaceDepthOnlyVert.msl.h"
#include "Shaders/msl/SurfaceDepthOnlyFrag.msl.h"
#include "Shaders/msl/TerrainDepthOnlyFrag.msl.h"
#include "Shaders/msl/SkinnedDepthOnlyVert.msl.h"
#include "Shaders/msl/SkinnedDepthOnlyFrag.msl.h"
#include "Shaders/msl/Shadow/SurfaceShadowDepthOnlyVert.msl.h"
#include "Shaders/msl/Shadow/SurfaceShadowDepthOnlyFrag.msl.h"
#include "Shaders/msl/Shadow/SkinnedShadowDepthOnlyVert.msl.h"
#include "Shaders/msl/Shadow/SkinnedShadowDepthOnlyFrag.msl.h"
#include "Shaders/msl/Shadow/SurfacePointShadowDepthOnlyVert.msl.h"
#include "Shaders/msl/Shadow/SurfacePointShadowDepthOnlyFrag.msl.h"
#include "Shaders/msl/Shadow/SkinnedPointShadowDepthOnlyVert.msl.h"
#include "Shaders/msl/Shadow/SkinnedPointShadowDepthOnlyFrag.msl.h"
#include "Shaders/msl/Shadow/SurfaceSpotShadowDepthOnlyVert.msl.h"
#include "Shaders/msl/Shadow/SkinnedSpotShadowDepthOnlyVert.msl.h"

#define Shaders_AnimationCompute_spv Shaders_Animation_AnimationCompute_msl
#define Shaders_AnimateVertices_spv Shaders_Animation_AnimateVertices_msl
#define Shaders_CullDrawArgsCompute_spv Shaders_PreProcessing_CullDrawArgsCompute_msl
#define Shaders_BuildLightGridCompute_spv Shaders_PreProcessing_BuildLightGridCompute_msl
#define Shaders_TonemapCompute_spv Shaders_PostProcessing_TonemapCompute_msl
#define Shaders_BloomDownsampleSPDCompute_spv Shaders_PostProcessing_BloomDownsampleSPDCompute_msl
#define Shaders_BloomUpsampleCompute_spv Shaders_PostProcessing_BloomUpsampleCompute_msl
#define Shaders_HiZBuildCompute_spv Shaders_PreProcessing_HiZBuildCompute_msl
#define Shaders_HiZDownscaleSPDCompute_spv Shaders_PreProcessing_HiZDownscaleSPDCompute_msl
#define Shaders_HBAOCompute_spv Shaders_PostProcessing_HBAOCompute_msl
#define Shaders_HBAOBlurCompute_spv Shaders_PostProcessing_HBAOBlurCompute_msl
#define Shaders_ContactShadowsCompute_spv Shaders_PostProcessing_ContactShadowsCompute_msl
#define Shaders_MLAAEdgeMaskCompute_spv Shaders_PostProcessing_MLAAEdgeMaskCompute_msl
#define Shaders_MLAALineLengthCompute_spv Shaders_PostProcessing_MLAALineLengthCompute_msl
#define Shaders_MLAABlendCompute_spv Shaders_PostProcessing_MLAABlendCompute_msl
#define Shaders_LineDebugVert_spv Shaders_LineDebugVert_msl
#define Shaders_LineDebugFrag_spv Shaders_LineDebugFrag_msl
#define Shaders_OutlineVert_spv Shaders_OutlineVert_msl
#define Shaders_OutlineFrag_spv Shaders_OutlineFrag_msl
#define Shaders_SlugVert_spv Shaders_UI_SlugVert_msl
#define Shaders_Slug2DVert_spv Shaders_Slug2DVert_msl
#define Shaders_SlugFrag_spv Shaders_UI_SlugFrag_msl
#define Shaders_UIShapeVert_spv Shaders_UI_UIShapeVert_msl
#define Shaders_UIShapeFrag_spv Shaders_UI_UIShapeFrag_msl
#define Shaders_UIImageVert_spv Shaders_UI_UIImageVert_msl
#define Shaders_UIImageFrag_spv Shaders_UI_UIImageFrag_msl
#define Shaders_SurfaceDepthOnlyVert_spv Shaders_SurfaceDepthOnlyVert_msl
#define Shaders_SurfaceDepthOnlyFrag_spv Shaders_SurfaceDepthOnlyFrag_msl
#define Shaders_TerrainDepthOnlyFrag_spv Shaders_TerrainDepthOnlyFrag_msl
#define Shaders_TerrainDepthOnlyFrag_spv_size Shaders_TerrainDepthOnlyFrag_msl_size
extern const unsigned int Shaders_TerrainDepthOnlyFrag_msl_size;
extern const unsigned char Shaders_TerrainDepthOnlyFrag_msl[];
#define Shaders_SkinnedDepthOnlyVert_spv Shaders_SkinnedDepthOnlyVert_msl
#define Shaders_SkinnedDepthOnlyFrag_spv Shaders_SkinnedDepthOnlyFrag_msl
#define Shaders_SurfaceShadowDepthOnlyVert_spv Shaders_Shadow_SurfaceShadowDepthOnlyVert_msl
#define Shaders_SurfaceShadowDepthOnlyFrag_spv Shaders_Shadow_SurfaceShadowDepthOnlyFrag_msl
#define Shaders_SkinnedShadowDepthOnlyVert_spv Shaders_Shadow_SkinnedShadowDepthOnlyVert_msl
#define Shaders_SkinnedShadowDepthOnlyFrag_spv Shaders_Shadow_SkinnedShadowDepthOnlyFrag_msl
#define Shaders_SurfacePointShadowDepthOnlyVert_spv Shaders_Shadow_SurfacePointShadowDepthOnlyVert_msl
#define Shaders_SurfacePointShadowDepthOnlyFrag_spv Shaders_Shadow_SurfacePointShadowDepthOnlyFrag_msl
#define Shaders_SkinnedPointShadowDepthOnlyVert_spv Shaders_Shadow_SkinnedPointShadowDepthOnlyVert_msl
#define Shaders_SkinnedPointShadowDepthOnlyFrag_spv Shaders_Shadow_SkinnedPointShadowDepthOnlyFrag_msl
#define Shaders_Shadow_SurfaceSpotShadowDepthOnlyVert_spv Shaders_Shadow_SurfaceSpotShadowDepthOnlyVert_msl
#define Shaders_Shadow_SkinnedSpotShadowDepthOnlyVert_spv Shaders_Shadow_SkinnedSpotShadowDepthOnlyVert_msl
#elif defined(PLATFORM_WINDOWS)
#include "Shaders/spv/SurfaceForwardFrag.spv.h"
#include "Shaders/spv/SurfaceForwardVert.spv.h"
#include "Shaders/spv/SkinnedForwardFrag.spv.h"
#include "Shaders/spv/SkinnedForwardVert.spv.h"
#include "Shaders/spv/ReconstructNormalCompute.spv.h"
#include "Shaders/spv/PreProcessing/BuildLightGridCompute.spv.h"
#include "Shaders/spv/PreProcessing/CullDrawArgsCompute.spv.h"
#include "Shaders/spv/Animation/AnimationCompute.spv.h"
#include "Shaders/spv/Animation/AnimateVertices.spv.h"
#include "Shaders/spv/LineDebugVert.spv.h"
#include "Shaders/spv/LineDebugFrag.spv.h"
#include "Shaders/spv/TerrainChunkVert.spv.h"
#include "Shaders/spv/TerrainChunkFrag.spv.h"
#include "Shaders/spv/OutlineVert.spv.h"
#include "Shaders/spv/OutlineFrag.spv.h"
#include "Shaders/spv/UI/SlugVert.spv.h"
#include "Shaders/spv/Slug2DVert.spv.h"
#include "Shaders/spv/UI/SlugFrag.spv.h"
#include "Shaders/spv/UI/UIShapeVert.spv.h"
#include "Shaders/spv/UI/UIShapeFrag.spv.h"
#include "Shaders/spv/UI/UIImageVert.spv.h"
#include "Shaders/spv/UI/UIImageFrag.spv.h"
#include "Shaders/spv/PostProcessing/TonemapCompute.spv.h"
#include "Shaders/spv/PostProcessing/BloomDownsampleSPDCompute.spv.h"
#include "Shaders/spv/PostProcessing/BloomUpsampleCompute.spv.h"
#include "Shaders/spv/PreProcessing/HiZBuildCompute.spv.h"
#include "Shaders/spv/PreProcessing/HiZDownscaleSPDCompute.spv.h"
#include "Shaders/spv/PostProcessing/HBAOCompute.spv.h"
#include "Shaders/spv/PostProcessing/HBAOBlurCompute.spv.h"
#include "Shaders/spv/PostProcessing/ContactShadowsCompute.spv.h"
#include "Shaders/spv/PostProcessing/MLAAEdgeMaskCompute.spv.h"
#include "Shaders/spv/PostProcessing/MLAALineLengthCompute.spv.h"
#include "Shaders/spv/PostProcessing/MLAABlendCompute.spv.h"
#include "Shaders/spv/SurfaceDepthOnlyVert.spv.h"
#include "Shaders/spv/SurfaceDepthOnlyFrag.spv.h"
#include "Shaders/spv/TerrainDepthOnlyFrag.spv.h"
#include "Shaders/spv/SkinnedDepthOnlyVert.spv.h"
#include "Shaders/spv/SkinnedDepthOnlyFrag.spv.h"
#include "Shaders/spv/Shadow/SurfaceShadowDepthOnlyVert.spv.h"
#include "Shaders/spv/Shadow/SurfaceShadowDepthOnlyFrag.spv.h"
#include "Shaders/spv/Shadow/SkinnedShadowDepthOnlyVert.spv.h"
#include "Shaders/spv/Shadow/SkinnedShadowDepthOnlyFrag.spv.h"
#include "Shaders/spv/Shadow/SurfacePointShadowDepthOnlyVert.spv.h"
#include "Shaders/spv/Shadow/SurfacePointShadowDepthOnlyFrag.spv.h"
#include "Shaders/spv/Shadow/SkinnedPointShadowDepthOnlyVert.spv.h"
#include "Shaders/spv/Shadow/SkinnedPointShadowDepthOnlyFrag.spv.h"
#include "Shaders/spv/Shadow/SurfaceSpotShadowDepthOnlyVert.spv.h"
#include "Shaders/spv/Shadow/SkinnedSpotShadowDepthOnlyVert.spv.h"
#define Shaders_AnimationCompute_spv Shaders_Animation_AnimationCompute_spv
#define Shaders_AnimationCompute_spv_size Shaders_Animation_AnimationCompute_spv_size
#define Shaders_AnimateVertices_spv Shaders_Animation_AnimateVertices_spv
#define Shaders_AnimateVertices_spv_size Shaders_Animation_AnimateVertices_spv_size
#define Shaders_CullDrawArgsCompute_spv Shaders_PreProcessing_CullDrawArgsCompute_spv
#define Shaders_CullDrawArgsCompute_spv_size Shaders_PreProcessing_CullDrawArgsCompute_spv_size
#define Shaders_BuildLightGridCompute_spv Shaders_PreProcessing_BuildLightGridCompute_spv
#define Shaders_BuildLightGridCompute_spv_size Shaders_PreProcessing_BuildLightGridCompute_spv_size
#define Shaders_TonemapCompute_spv Shaders_PostProcessing_TonemapCompute_spv
#define Shaders_TonemapCompute_spv_size Shaders_PostProcessing_TonemapCompute_spv_size
#define Shaders_BloomDownsampleSPDCompute_spv Shaders_PostProcessing_BloomDownsampleSPDCompute_spv
#define Shaders_BloomDownsampleSPDCompute_spv_size Shaders_PostProcessing_BloomDownsampleSPDCompute_spv_size
#define Shaders_BloomUpsampleCompute_spv Shaders_PostProcessing_BloomUpsampleCompute_spv
#define Shaders_BloomUpsampleCompute_spv_size Shaders_PostProcessing_BloomUpsampleCompute_spv_size
#define Shaders_HiZBuildCompute_spv Shaders_PreProcessing_HiZBuildCompute_spv
#define Shaders_HiZBuildCompute_spv_size Shaders_PreProcessing_HiZBuildCompute_spv_size
#define Shaders_HiZDownscaleSPDCompute_spv Shaders_PreProcessing_HiZDownscaleSPDCompute_spv
#define Shaders_HiZDownscaleSPDCompute_spv_size Shaders_PreProcessing_HiZDownscaleSPDCompute_spv_size
#define Shaders_HBAOCompute_spv Shaders_PostProcessing_HBAOCompute_spv
#define Shaders_HBAOCompute_spv_size Shaders_PostProcessing_HBAOCompute_spv_size
#define Shaders_HBAOBlurCompute_spv Shaders_PostProcessing_HBAOBlurCompute_spv
#define Shaders_HBAOBlurCompute_spv_size Shaders_PostProcessing_HBAOBlurCompute_spv_size
#define Shaders_ContactShadowsCompute_spv Shaders_PostProcessing_ContactShadowsCompute_spv
#define Shaders_ContactShadowsCompute_spv_size Shaders_PostProcessing_ContactShadowsCompute_spv_size
#define Shaders_MLAAEdgeMaskCompute_spv Shaders_PostProcessing_MLAAEdgeMaskCompute_spv
#define Shaders_MLAAEdgeMaskCompute_spv_size Shaders_PostProcessing_MLAAEdgeMaskCompute_spv_size
#define Shaders_MLAALineLengthCompute_spv Shaders_PostProcessing_MLAALineLengthCompute_spv
#define Shaders_MLAALineLengthCompute_spv_size Shaders_PostProcessing_MLAALineLengthCompute_spv_size
#define Shaders_MLAABlendCompute_spv Shaders_PostProcessing_MLAABlendCompute_spv
#define Shaders_MLAABlendCompute_spv_size Shaders_PostProcessing_MLAABlendCompute_spv_size
#define Shaders_SlugVert_spv Shaders_UI_SlugVert_spv
#define Shaders_SlugVert_spv_size Shaders_UI_SlugVert_spv_size
#define Shaders_SlugFrag_spv Shaders_UI_SlugFrag_spv
#define Shaders_SlugFrag_spv_size Shaders_UI_SlugFrag_spv_size
#define Shaders_UIShapeVert_spv Shaders_UI_UIShapeVert_spv
#define Shaders_UIShapeVert_spv_size Shaders_UI_UIShapeVert_spv_size
#define Shaders_UIShapeFrag_spv Shaders_UI_UIShapeFrag_spv
#define Shaders_UIShapeFrag_spv_size Shaders_UI_UIShapeFrag_spv_size
#define Shaders_UIImageVert_spv Shaders_UI_UIImageVert_spv
#define Shaders_UIImageVert_spv_size Shaders_UI_UIImageVert_spv_size
#define Shaders_UIImageFrag_spv Shaders_UI_UIImageFrag_spv
#define Shaders_UIImageFrag_spv_size Shaders_UI_UIImageFrag_spv_size
#define Shaders_SurfaceShadowDepthOnlyVert_spv Shaders_Shadow_SurfaceShadowDepthOnlyVert_spv
#define Shaders_SurfaceShadowDepthOnlyVert_spv_size Shaders_Shadow_SurfaceShadowDepthOnlyVert_spv_size
#define Shaders_SurfaceShadowDepthOnlyFrag_spv Shaders_Shadow_SurfaceShadowDepthOnlyFrag_spv
#define Shaders_SurfaceShadowDepthOnlyFrag_spv_size Shaders_Shadow_SurfaceShadowDepthOnlyFrag_spv_size
#define Shaders_SkinnedShadowDepthOnlyVert_spv Shaders_Shadow_SkinnedShadowDepthOnlyVert_spv
#define Shaders_SkinnedShadowDepthOnlyVert_spv_size Shaders_Shadow_SkinnedShadowDepthOnlyVert_spv_size
#define Shaders_SkinnedShadowDepthOnlyFrag_spv Shaders_Shadow_SkinnedShadowDepthOnlyFrag_spv
#define Shaders_SkinnedShadowDepthOnlyFrag_spv_size Shaders_Shadow_SkinnedShadowDepthOnlyFrag_spv_size
#define Shaders_SurfacePointShadowDepthOnlyVert_spv Shaders_Shadow_SurfacePointShadowDepthOnlyVert_spv
#define Shaders_SurfacePointShadowDepthOnlyVert_spv_size Shaders_Shadow_SurfacePointShadowDepthOnlyVert_spv_size
#define Shaders_SurfacePointShadowDepthOnlyFrag_spv Shaders_Shadow_SurfacePointShadowDepthOnlyFrag_spv
#define Shaders_SurfacePointShadowDepthOnlyFrag_spv_size Shaders_Shadow_SurfacePointShadowDepthOnlyFrag_spv_size
#define Shaders_SkinnedPointShadowDepthOnlyVert_spv Shaders_Shadow_SkinnedPointShadowDepthOnlyVert_spv
#define Shaders_SkinnedPointShadowDepthOnlyVert_spv_size Shaders_Shadow_SkinnedPointShadowDepthOnlyVert_spv_size
#define Shaders_SkinnedPointShadowDepthOnlyFrag_spv Shaders_Shadow_SkinnedPointShadowDepthOnlyFrag_spv
#define Shaders_SkinnedPointShadowDepthOnlyFrag_spv_size Shaders_Shadow_SkinnedPointShadowDepthOnlyFrag_spv_size
extern const unsigned int Shaders_TerrainDepthOnlyFrag_spv_size;
extern const unsigned char Shaders_TerrainDepthOnlyFrag_spv[];
#endif

#define PIPELINE_VERT_DEF(xbuffer) \
SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){\
.code = xbuffer, .code_size = sizeof(xbuffer),\
.format              = shaderformat,\
.stage               = SDL_GPU_SHADERSTAGE_VERTEX,\
.entrypoint          = "vert"

#define PIPELINE_FRAG_DEF(xbuffer) \
SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){\
.code = xbuffer, .code_size = sizeof(xbuffer),\
.format              = shaderformat,\
.stage               = SDL_GPU_SHADERSTAGE_FRAGMENT,\
.entrypoint          = "frag"

#define COMPUTE_DEF(xbuffer) SDL_CreateGPUComputePipeline(g_GPUDevice, &(SDL_GPUComputePipelineCreateInfo){ \
.code = xbuffer, .code_size = sizeof(xbuffer), .entrypoint = AX_GPU_COMPUTE_ENTRYPOINT, .format = shaderformat

#define THREAD_COUNT_XYZ(_X, _Y, _Z)\
.threadcount_x                 = _X,\
.threadcount_y                 = _Y,\
.threadcount_z                 = _Z

SDL_GPUComputePipeline* g_AnimComputePipeline            = NULL;
SDL_GPUComputePipeline* g_AnimVerticesPipeline           = NULL;
SDL_GPUComputePipeline* g_CullDrawArgsComputePipeline    = NULL;
SDL_GPUComputePipeline* g_BuildLightGridComputePipeline  = NULL;
SDL_GPUComputePipeline* g_ReconstructNormalComputePipeline = NULL;
SDL_GPUComputePipeline* g_TonemapComputePipeline         = NULL;
SDL_GPUComputePipeline* g_BloomDownsampleSPDComputePipeline = NULL;
SDL_GPUComputePipeline* g_BloomUpsampleComputePipeline   = NULL;
SDL_GPUComputePipeline* g_HiZBuildComputePipeline        = NULL;
SDL_GPUComputePipeline* g_HiZDownscaleSPDComputePipeline = NULL;
SDL_GPUComputePipeline* g_HBAOComputePipeline            = NULL;
SDL_GPUComputePipeline* g_HBAOBlurComputePipeline        = NULL;
SDL_GPUComputePipeline* g_ContactShadowsComputePipeline  = NULL;
SDL_GPUComputePipeline* g_MLAAEdgeMaskComputePipeline    = NULL;
SDL_GPUComputePipeline* g_MLAALineLengthComputePipeline  = NULL;
SDL_GPUComputePipeline* g_MLAABlendComputePipeline       = NULL;

static void InitComputePipelines(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    g_AnimComputePipeline = COMPUTE_DEF(Shaders_AnimationCompute_spv),
        .num_uniform_buffers           = 1, .num_readonly_storage_buffers  = 7, .num_readwrite_storage_buffers = 1,
        THREAD_COUNT_XYZ(32, 1, 1)
    }); CHECK_CREATE(g_AnimComputePipeline, "Animation Compute Pipeline");

    g_AnimVerticesPipeline = COMPUTE_DEF(Shaders_AnimateVertices_spv),
        .num_uniform_buffers           = 1, .num_readonly_storage_buffers  = 8, .num_readwrite_storage_buffers = 1,
        THREAD_COUNT_XYZ(1, 32, 1)
    }); CHECK_CREATE(g_AnimVerticesPipeline, "Animation vertices Pipeline");

    g_CullDrawArgsComputePipeline = COMPUTE_DEF(Shaders_CullDrawArgsCompute_spv),
        .num_readonly_storage_textures = 1, .num_uniform_buffers = 1, .num_readonly_storage_buffers  = 3, .num_readwrite_storage_buffers = 8,
        THREAD_COUNT_XYZ(64, 1, 1)
    }); CHECK_CREATE(g_CullDrawArgsComputePipeline, "Cull Draw Args Compute Pipeline");

    // Forward+ tiled light culling: 1 sampled depth + light buffer in, grid/index/counter/visibility out.
    g_BuildLightGridComputePipeline = COMPUTE_DEF(Shaders_BuildLightGridCompute_spv),
        .num_samplers = 1, .num_uniform_buffers = 1, .num_readonly_storage_buffers = 1, .num_readwrite_storage_buffers = 4,
        THREAD_COUNT_XYZ(16, 16, 1)
    }); CHECK_CREATE(g_BuildLightGridComputePipeline, "Build Light Grid Compute Pipeline");

    // Forward+ AO helper: reconstructs world normals from the prepass depth for HBAO.
    g_ReconstructNormalComputePipeline = COMPUTE_DEF(Shaders_ReconstructNormalCompute_spv),
        .num_samplers = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_ReconstructNormalComputePipeline, "Reconstruct Normal Compute Pipeline");

    g_HiZBuildComputePipeline = COMPUTE_DEF(Shaders_HiZBuildCompute_spv),
        .num_samplers = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_HiZBuildComputePipeline, "Hi-Z Build Compute Pipeline");
    
    // AMD FFX SPD: whole reduction chain (tex_hiz mip1..mipN) in one dispatch, wave/LDS min-reduction instead of a per-mip loop.
    g_HiZDownscaleSPDComputePipeline = COMPUTE_DEF(Shaders_HiZDownscaleSPDCompute_spv),
        .num_readonly_storage_textures = 1, .num_readwrite_storage_textures = 8, .num_readwrite_storage_buffers = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(256, 1, 1)
    }); CHECK_CREATE(g_HiZDownscaleSPDComputePipeline, "Hi-Z Downscale SPD Compute Pipeline");

    g_TonemapComputePipeline = COMPUTE_DEF(Shaders_TonemapCompute_spv),
        .num_samplers = 4, .num_readonly_storage_buffers = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_TonemapComputePipeline, "Tonemap Compute Pipeline");

    // AMD FFX SPD: whole downsample mip chain (mip0..mipN) in one dispatch, wave/LDS reduction instead of a per-mip loop.
    g_BloomDownsampleSPDComputePipeline = COMPUTE_DEF(Shaders_BloomDownsampleSPDCompute_spv),
        .num_samplers = 1, .num_readwrite_storage_textures = 8, .num_readwrite_storage_buffers = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(256, 1, 1)
    }); CHECK_CREATE(g_BloomDownsampleSPDComputePipeline, "Bloom Downsample SPD Compute Pipeline");

    g_BloomUpsampleComputePipeline = COMPUTE_DEF(Shaders_BloomUpsampleCompute_spv),
        .num_samplers = 2, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_BloomUpsampleComputePipeline, "Bloom Upsample Compute Pipeline");

    g_HBAOComputePipeline = COMPUTE_DEF(Shaders_HBAOCompute_spv),
        .num_samplers = 2, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_HBAOComputePipeline, "HBAO Compute Pipeline");

    g_HBAOBlurComputePipeline = COMPUTE_DEF(Shaders_HBAOBlurCompute_spv),
        .num_samplers = 2, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_HBAOBlurComputePipeline, "HBAO Blur Compute Pipeline");

    g_ContactShadowsComputePipeline = COMPUTE_DEF(Shaders_ContactShadowsCompute_spv),
        .num_samplers = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(64, 1, 1)
    }); CHECK_CREATE(g_ContactShadowsComputePipeline, "Contact Shadows Compute Pipeline");

    g_MLAAEdgeMaskComputePipeline = COMPUTE_DEF(Shaders_MLAAEdgeMaskCompute_spv),
        .num_samplers = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_MLAAEdgeMaskComputePipeline, "MLAA Edge Mask Compute Pipeline");

    g_MLAALineLengthComputePipeline = COMPUTE_DEF(Shaders_MLAALineLengthCompute_spv),
        .num_readonly_storage_textures  = 1, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_MLAALineLengthComputePipeline, "MLAA Line Length Compute Pipeline");

    g_MLAABlendComputePipeline = COMPUTE_DEF(Shaders_MLAABlendCompute_spv),
        .num_samplers = 1, .num_readonly_storage_textures  = 2, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
        THREAD_COUNT_XYZ(8, 8, 1)
    }); CHECK_CREATE(g_MLAABlendComputePipeline, "MLAA Blend Compute Pipeline");
}

static void InitSamplers(void)
{
    g_RenderState.sampler = SDL_CreateGPUSampler(g_GPUDevice, &(SDL_GPUSamplerCreateInfo){
        .min_filter      = SDL_GPU_FILTER_LINEAR,
        .mag_filter      = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        .address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
        .address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
        .address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
        .min_lod         = 0.0f,
        .max_lod         = 12.0f
    }); CHECK_CREATE(g_RenderState.sampler, "Linear Sampler")

    g_RenderState.hiZSampler = SDL_CreateGPUSampler(g_GPUDevice, &(SDL_GPUSamplerCreateInfo){
        .min_filter      = SDL_GPU_FILTER_NEAREST,
        .mag_filter      = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .min_lod         = 0.0f,
        .max_lod         = 32.0f
    }); CHECK_CREATE(g_RenderState.hiZSampler, "Hi-Z Sampler")

    g_RenderState.shadowSampler = SDL_CreateGPUSampler(g_GPUDevice, &(SDL_GPUSamplerCreateInfo){
        .min_filter      = SDL_GPU_FILTER_NEAREST,
        .mag_filter      = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode     = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w  = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .min_lod         = 0.0f,
        .max_lod         = SHADOW_CASCADE_COUNT
    }); CHECK_CREATE(g_RenderState.shadowSampler, "Shadow Sampler")
}

static void InitLinePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_LineDebugVert_spv), .num_uniform_buffers = 1 }); CHECK_CREATE(vertex_shader, "Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_LineDebugFrag_spv)});                            CHECK_CREATE(fragment_shader, "Fragment Shader")

    const SDL_GPUVertexAttribute vertex_attributes[2] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_FLOAT3, .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT, .offset = sizeof(f32) * 3 }
    };

    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    SDL_GPUGraphicsPipelineCreateInfo pipelinedesc = {
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_LINELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = colorFormat },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = true,
            .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
        },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(ALineVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
            },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    };

    g_RenderState.linePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &pipelinedesc);
    CHECK_CREATE(g_RenderState.linePipeline, "Render Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

// gizmo overlay: same shaders and vertex format as the debug lines, but triangles
// (the gizmo builds camera facing quads for thick lines) and no depth target so it
// always draws on top
static void InitGizmoLinePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_LineDebugVert_spv), .num_uniform_buffers = 1 }); CHECK_CREATE(vertex_shader, "Gizmo Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_LineDebugFrag_spv)});                            CHECK_CREATE(fragment_shader, "Gizmo Fragment Shader")

    const SDL_GPUVertexAttribute vertex_attributes[2] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_FLOAT3, .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT, .offset = sizeof(f32) * 3 }
    };

    g_GizmoLinePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM }
        },
        .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(ALineVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
            },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    });
    CHECK_CREATE(g_GizmoLinePipeline, "Gizmo Line Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

SDL_GPUGraphicsPipeline* g_OutlinePipeline;
SDL_GPUGraphicsPipeline* g_GizmoLinePipeline;
SDL_GPUGraphicsPipeline* g_TerrainTrianglePipeline;
SDL_GPUGraphicsPipeline* g_TerrainTriangleDepthPipeline;

static void InitTerrainTrianglePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_TerrainChunkVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 1 }); CHECK_CREATE(vertex_shader, "Terrain Triangle Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_TerrainChunkFrag_spv), .num_uniform_buffers = 1, .num_samplers = 3 }); CHECK_CREATE(fragment_shader, "Terrain Triangle Fragment Shader")

    const SDL_GPUVertexAttribute vertex_attributes[3] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_UINT2,  .offset = offsetof(tVertexData, position) },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(tVertexData, normal) },
        { .location = 2, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(tVertexData, materials) },
    };

    g_TerrainTrianglePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .rasterizer_state = (SDL_GPURasterizerState){ .cull_mode = SDL_GPU_CULLMODE_NONE },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = true,
            .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = g_RenderState.sceneSampleCount },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(tVertexData), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
            },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    });
    CHECK_CREATE(g_TerrainTrianglePipeline, "Terrain Triangle Pipeline")

    SDL_GPUShader* depth_fragment_shader = SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){
        .code = Shaders_TerrainDepthOnlyFrag_spv,
        .code_size = Shaders_TerrainDepthOnlyFrag_spv_size,
        .format = shaderformat,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .entrypoint = "frag",
        .num_uniform_buffers = 0
    });
    CHECK_CREATE(depth_fragment_shader, "Terrain Triangle Depth Fragment Shader")
    g_TerrainTriangleDepthPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = depth_fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .rasterizer_state = (SDL_GPURasterizerState){ .cull_mode = SDL_GPU_CULLMODE_NONE },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = true,
            .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(tVertexData), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
            },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    });
    CHECK_CREATE(g_TerrainTriangleDepthPipeline, "Terrain Triangle Depth Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, depth_fragment_shader);
}

// editor selection outline: the selected primitive re-draws as an inverted hull, the
// vertex shader grows it along the normals (ported from the old engine's outline)
static void InitOutlinePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_OutlineVert_spv), .num_uniform_buffers = 1 }); CHECK_CREATE(vertex_shader, "Outline Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_OutlineFrag_spv)});                            CHECK_CREATE(fragment_shader, "Outline Fragment Shader")

    const SDL_GPUVertexAttribute vertex_attributes[3] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_UINT2,  .offset = offsetof(AVertex, position) },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(AVertex, octTbn)   },
        { .location = 2, .buffer_slot = 0, .format = VFORMAT_HALF2,  .offset = offsetof(AVertex, texCoord) }
    };

    g_OutlinePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        // front face culling keeps only the back of the grown hull, the scene depth hides
        // the interior so a silhouette ring remains (replaces the old stencil masking)
        .rasterizer_state = (SDL_GPURasterizerState){
            .cull_mode  = SDL_GPU_CULLMODE_FRONT,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE
        },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = false,
            .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
        },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){
                0, sizeof(AVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0
            },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    });
    CHECK_CREATE(g_OutlinePipeline, "Outline Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

static void InitSlugPipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader     = PIPELINE_VERT_DEF(Shaders_SlugVert_spv), .num_uniform_buffers = 1 }); CHECK_CREATE(vertex_shader, "Slug Vertex Shader");
    SDL_GPUShader* fragment_shader   = PIPELINE_FRAG_DEF(Shaders_SlugFrag_spv), .num_storage_buffers = 2 }); CHECK_CREATE(fragment_shader, "Slug Fragment Shader");
    
    SDL_GPUShader* vertex_2d_shader = SDL_CreateGPUShader(g_GPUDevice, &(SDL_GPUShaderCreateInfo){
        .num_uniform_buffers = 1,
        .format              = AX_GPU_SHADER_FORMAT,
        .code                = Shaders_Slug2DVert_spv,
        .code_size           = sizeof(Shaders_Slug2DVert_spv),
        .num_samplers        = 0,
        .num_storage_buffers = 0,
        .stage               = SDL_GPU_SHADERSTAGE_VERTEX,
        .entrypoint          = "vert2d"
    }); CHECK_CREATE(vertex_2d_shader, "Slug 2D Vertex Shader")

    const SDL_GPUVertexAttribute vertex_attributes[6] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_FLOAT4, .offset = offsetof(SlugVertex, pos)   },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_FLOAT4, .offset = offsetof(SlugVertex, tex)   },
        { .location = 2, .buffer_slot = 0, .format = VFORMAT_FLOAT4, .offset = offsetof(SlugVertex, jac)   },
        { .location = 3, .buffer_slot = 0, .format = VFORMAT_FLOAT4, .offset = offsetof(SlugVertex, band)  },
        { .location = 4, .buffer_slot = 0, .format = VFORMAT_UINT  , .offset = offsetof(SlugVertex, color) },
        { .location = 5, .buffer_slot = 0, .format = VFORMAT_F32   , .offset = offsetof(SlugVertex, z)     }
    };
    SDL_GPUColorTargetDescription colorTarget = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = {
            .enable_blend = true,
            .color_write_mask = 0xF,
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        }
    };

    SDL_GPUGraphicsPipelineCreateInfo pipelinedesc = {
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &colorTarget
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(SlugVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = vertex_attributes,
            .num_vertex_attributes = ARRAY_SIZE(vertex_attributes)
        }
    };

    g_RenderState.slugPipeline   = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &pipelinedesc); CHECK_CREATE(g_RenderState.slugPipeline, "Slug Render Pipeline")
    // 2d text draws over the swapchain after the scene upscale blit
    colorTarget.format           = SDL_GetGPUSwapchainTextureFormat(g_GPUDevice, g_SDLWindow);
    pipelinedesc.vertex_shader   = vertex_2d_shader;
    g_RenderState.slug2DPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &pipelinedesc); CHECK_CREATE(g_RenderState.slug2DPipeline, "Slug 2D Render Pipeline")
    colorTarget.format           = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pipelinedesc.vertex_shader   = vertex_shader;

    pipelinedesc.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    pipelinedesc.target_info.has_depth_stencil_target = true;
    pipelinedesc.depth_stencil_state = (SDL_GPUDepthStencilState){
        .enable_depth_test  = true,
        .enable_depth_write = false,
        .compare_op         = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
    };
    g_RenderState.slugDepthPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &pipelinedesc);
    CHECK_CREATE(g_RenderState.slugDepthPipeline, "Slug Depth Render Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_2d_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

static void InitUIShapePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_UIShapeVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 1 }); CHECK_CREATE(vertex_shader, "UI Shape Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_UIShapeFrag_spv), .num_uniform_buffers = 1 });                           CHECK_CREATE(fragment_shader, "UI Shape Fragment Shader")

    SDL_GPUColorTargetDescription colorTarget = {
        .format = SDL_GetGPUSwapchainTextureFormat(g_GPUDevice, g_SDLWindow), // ui renders to the swapchain at native resolution
        .blend_state = {
            .enable_blend = true,
            .color_write_mask = 0xF,
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        }
    };

    SDL_GPUGraphicsPipelineCreateInfo pipelinedesc = {
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &colorTarget
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 }
    };

    g_RenderState.uiShapePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &pipelinedesc);
    CHECK_CREATE(g_RenderState.uiShapePipeline, "UI Shape Render Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

static void InitUIImagePipeline(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* vertex_shader   = PIPELINE_VERT_DEF(Shaders_UIImageVert_spv), .num_uniform_buffers = 1 });                    CHECK_CREATE(vertex_shader, "UI Image Vertex Shader")
    SDL_GPUShader* fragment_shader = PIPELINE_FRAG_DEF(Shaders_UIImageFrag_spv), .num_uniform_buffers = 1, .num_samplers = 1 }); CHECK_CREATE(fragment_shader, "UI Image Fragment Shader")

    SDL_GPUColorTargetDescription colorTarget = {
        .format = SDL_GetGPUSwapchainTextureFormat(g_GPUDevice, g_SDLWindow), // ui renders to the swapchain at native resolution
        .blend_state = {
            .enable_blend = true,
            .color_write_mask = 0xF,
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        }
    };

    g_RenderState.uiImagePipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &colorTarget
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 }
    });
    CHECK_CREATE(g_RenderState.uiImagePipeline, "UI Image Render Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertex_shader);
    SDL_ReleaseGPUShader(g_GPUDevice, fragment_shader);
}

// Forward+ opaque pipelines. Single HDR color target,
// depth EQUAL for prepass reuse, or LESS_OR_EQUAL when MSAA uses a separate depth target. The fragment
// stage binds 8 sampled textures (material pages + shadow atlases + AO/contact) and 7 storage
// buffers (materials, descriptors, lights, light grid/index, point/spot shadow matrices).
static void InitForwardPipelines(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* sur_vert = PIPELINE_VERT_DEF(Shaders_SurfaceForwardVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 4 }); CHECK_CREATE(sur_vert, "Surface Forward Vertex Shader")
    SDL_GPUShader* sur_frag = PIPELINE_FRAG_DEF(Shaders_SurfaceForwardFrag_spv), .num_uniform_buffers = 1, .num_samplers = 8, .num_storage_buffers = 7 }); CHECK_CREATE(sur_frag, "Surface Forward Fragment Shader")
    SDL_GPUShader* ski_vert = PIPELINE_VERT_DEF(Shaders_SkinnedForwardVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 7 }); CHECK_CREATE(ski_vert, "Skinned Forward Vertex Shader")
    SDL_GPUShader* ski_frag = PIPELINE_FRAG_DEF(Shaders_SkinnedForwardFrag_spv), .num_uniform_buffers = 1, .num_samplers = 8, .num_storage_buffers = 7 }); CHECK_CREATE(ski_frag, "Skinned Forward Fragment Shader")

    const SDL_GPUVertexAttribute sur_attributes[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT2, .offset = offsetof(AVertex, position) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT,  .offset = offsetof(AVertex, octTbn)   },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF2, .offset = offsetof(AVertex, texCoord) }
    };
    const SDL_GPUVertexAttribute ski_attributes[5] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_HALF4,  .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(ASkinedVertex, octTbn)   },
        { .location = 2, .buffer_slot = 0, .format = VFORMAT_HALF2,  .offset = offsetof(ASkinedVertex, texCoord) },
        { .location = 3, .buffer_slot = 0, .format = VFORMAT_UBYTE4, .offset = offsetof(ASkinedVertex, joints)   },
        { .location = 4, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(ASkinedVertex, weights)  }
    };

    SDL_GPUCompareOp opaqueCompareOp = g_RenderState.sceneSampleCount == SDL_GPU_SAMPLECOUNT_1 ?
                                      SDL_GPU_COMPAREOP_EQUAL : SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;

    SDL_GPUGraphicsPipelineCreateInfo desc = {
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = g_RenderState.sceneSampleCount != SDL_GPU_SAMPLECOUNT_1,
            .compare_op         = opaqueCompareOp
        },
        .multisample_state  = (SDL_GPUMultisampleState){ .sample_count = g_RenderState.sceneSampleCount },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(AVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
            .num_vertex_buffers    = 1,
            .vertex_attributes     = sur_attributes,
            .num_vertex_attributes = ARRAY_SIZE(sur_attributes)
        }
    };

    desc.vertex_shader   = sur_vert;
    desc.fragment_shader = sur_frag;
    g_RenderState.surface.forwardPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &desc);
    CHECK_CREATE(g_RenderState.surface.forwardPipeline, "Surface Forward Pipeline")

    SDL_GPUColorTargetDescription transparentColorTarget = {
        .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        .blend_state = {
            .enable_blend = true,
            .color_write_mask = 0xF,
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        }
    };
    desc.target_info.color_target_descriptions = &transparentColorTarget;
    desc.depth_stencil_state.enable_depth_write = false;
    desc.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    g_RenderState.transparentForwardPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &desc);
    CHECK_CREATE(g_RenderState.transparentForwardPipeline, "Transparent Surface Forward Pipeline")

    desc.vertex_shader   = ski_vert;
    desc.fragment_shader = ski_frag;
    desc.target_info.color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };
    desc.depth_stencil_state.enable_depth_write = g_RenderState.sceneSampleCount != SDL_GPU_SAMPLECOUNT_1;
    desc.depth_stencil_state.compare_op = opaqueCompareOp;
    desc.vertex_input_state = (SDL_GPUVertexInputState){
        .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(ASkinedVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
        .num_vertex_buffers    = 1,
        .vertex_attributes     = ski_attributes,
        .num_vertex_attributes = ARRAY_SIZE(ski_attributes)
    };
    g_RenderState.skinned.forwardPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, &desc);
    CHECK_CREATE(g_RenderState.skinned.forwardPipeline, "Skinned Forward Pipeline")

    SDL_ReleaseGPUShader(g_GPUDevice, sur_vert); SDL_ReleaseGPUShader(g_GPUDevice, sur_frag);
    SDL_ReleaseGPUShader(g_GPUDevice, ski_vert); SDL_ReleaseGPUShader(g_GPUDevice, ski_frag);
}

static void CreatePipelineWithDesc(RenderSetShared* buffers, SDL_GPUGraphicsPipelineCreateInfo* desc,
                                   SDL_GPUShader* vertexDepth, SDL_GPUShader* fragmentDepth,
                                   SDL_GPUShader* vertexShadow, SDL_GPUShader* fragmentShadow,
                                   SDL_GPUShader* pointShadowVertex, SDL_GPUShader* pointShadowFragment,
                                   SDL_GPUShader* spotShadowVertex, SDL_GPUShader* spotShadowFragment)
{
    // The camera depth pre-pass uses reversed-Z (GREATER_OR_EQUAL); the shadow passes below
    // render to standard-Z shadow maps and keep LESS_OR_EQUAL (the desc's incoming value).
    desc->vertex_shader     = vertexDepth; desc->fragment_shader = fragmentDepth;
    desc->depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    buffers->depthPipeline  = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, desc);
    desc->depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    desc->vertex_shader     = vertexShadow; desc->fragment_shader = fragmentShadow;
    buffers->shadowPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, desc);
    desc->vertex_shader     = pointShadowVertex; desc->fragment_shader = pointShadowFragment;
    buffers->pointShadowPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, desc);
    desc->vertex_shader     = spotShadowVertex; desc->fragment_shader = spotShadowFragment;
    buffers->spotShadowPipeline = SDL_CreateGPUGraphicsPipeline(g_GPUDevice, desc);
    CHECK_CREATE(buffers->depthPipeline, "Depth Pipeline") CHECK_CREATE(buffers->shadowPipeline, "Shadow Pipeline") CHECK_CREATE(buffers->pointShadowPipeline, "Point Shadow Pipeline") CHECK_CREATE(buffers->spotShadowPipeline, "Spot Shadow Pipeline")
    SDL_ReleaseGPUShader(g_GPUDevice, vertexDepth);       SDL_ReleaseGPUShader(g_GPUDevice, fragmentDepth);
    SDL_ReleaseGPUShader(g_GPUDevice, vertexShadow);      SDL_ReleaseGPUShader(g_GPUDevice, fragmentShadow);
    SDL_ReleaseGPUShader(g_GPUDevice, pointShadowVertex); SDL_ReleaseGPUShader(g_GPUDevice, pointShadowFragment);
    SDL_ReleaseGPUShader(g_GPUDevice, spotShadowVertex);  SDL_ReleaseGPUShader(g_GPUDevice, spotShadowFragment);
}

static void InitDepthOnlyPipelines(void)
{
    SDL_GPUShaderFormat shaderformat = AX_GPU_SHADER_FORMAT;
    SDL_GPUShader* sur_ver  = PIPELINE_VERT_DEF(Shaders_SurfaceDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 3 });
    SDL_GPUShader* ski_ver  = PIPELINE_VERT_DEF(Shaders_SkinnedDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 5 });
    SDL_GPUShader* sur_frag = PIPELINE_FRAG_DEF(Shaders_SurfaceDepthOnlyFrag_spv), .num_uniform_buffers = 0, .num_samplers = 1, .num_storage_buffers = 2 });
    SDL_GPUShader* ski_frag = PIPELINE_FRAG_DEF(Shaders_SkinnedDepthOnlyFrag_spv), .num_storage_buffers = 2 ,.num_samplers = 1 });
    SDL_GPUShader* sur_shadow_ver        = PIPELINE_VERT_DEF(Shaders_SurfaceShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 4 });
    SDL_GPUShader* ski_shadow_ver        = PIPELINE_VERT_DEF(Shaders_SkinnedShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 6 });
    SDL_GPUShader* sur_shadow_frag       = PIPELINE_FRAG_DEF(Shaders_SurfaceShadowDepthOnlyFrag_spv) });
    SDL_GPUShader* ski_shadow_frag       = PIPELINE_FRAG_DEF(Shaders_SkinnedShadowDepthOnlyFrag_spv) });
    SDL_GPUShader* sur_point_shadow_ver  = PIPELINE_VERT_DEF(Shaders_SurfacePointShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 4 });
    SDL_GPUShader* ski_point_shadow_ver  = PIPELINE_VERT_DEF(Shaders_SkinnedPointShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 6 });
    SDL_GPUShader* sur_spot_shadow_ver   = PIPELINE_VERT_DEF(Shaders_Shadow_SurfaceSpotShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 4 });
    SDL_GPUShader* ski_spot_shadow_ver   = PIPELINE_VERT_DEF(Shaders_Shadow_SkinnedSpotShadowDepthOnlyVert_spv), .num_uniform_buffers = 1, .num_storage_buffers = 6 });
    SDL_GPUShader* sur_point_shadow_frag = PIPELINE_FRAG_DEF(Shaders_SurfacePointShadowDepthOnlyFrag_spv) });
    SDL_GPUShader* ski_point_shadow_frag = PIPELINE_FRAG_DEF(Shaders_SkinnedPointShadowDepthOnlyFrag_spv) });
    SDL_GPUShader* sur_spot_shadow_frag  = PIPELINE_FRAG_DEF(Shaders_SurfacePointShadowDepthOnlyFrag_spv) });
    SDL_GPUShader* ski_spot_shadow_frag  = PIPELINE_FRAG_DEF(Shaders_SkinnedPointShadowDepthOnlyFrag_spv) });
    CHECK_CREATE(sur_ver, "Surface Depth Vertex Shader") CHECK_CREATE(sur_frag, "Surface Depth Fragment Shader") CHECK_CREATE(ski_ver, "Skinned Depth Vertex Shader") CHECK_CREATE(ski_frag, "Skinned Depth Fragment Shader") CHECK_CREATE(sur_shadow_ver, "Surface Shadow Vertex Shader") CHECK_CREATE(sur_shadow_frag, "Surface Shadow Fragment Shader") CHECK_CREATE(ski_shadow_ver, "Skinned Shadow Vertex Shader") CHECK_CREATE(ski_shadow_frag, "Skinned Shadow Fragment Shader") CHECK_CREATE(sur_point_shadow_ver, "Surface Point Shadow Vertex Shader")
    CHECK_CREATE(sur_point_shadow_frag , "Surface Point Shadow Fragment Shader") CHECK_CREATE(ski_point_shadow_ver, "Skinned Point Shadow Vertex Shader") CHECK_CREATE(ski_point_shadow_frag, "Skinned Point Shadow Fragment Shader") CHECK_CREATE(sur_spot_shadow_ver, "Surface Spot Shadow Vertex Shader") CHECK_CREATE(ski_spot_shadow_ver, "Skinned Spot Shadow Vertex Shader") CHECK_CREATE(sur_spot_shadow_frag, "Surface Spot Shadow Fragment Shader") CHECK_CREATE(ski_spot_shadow_frag, "Skinned Spot Shadow Fragment Shader")

    const SDL_GPUVertexAttribute sur_attributes[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT2,  .offset = offsetof(AVertex, position) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT,   .offset = offsetof(AVertex, octTbn)   },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF2,  .offset = offsetof(AVertex, texCoord) }
    };
    const SDL_GPUVertexAttribute ski_attributes[5] = {
        { .location = 0, .buffer_slot = 0, .format = VFORMAT_HALF4,  .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(ASkinedVertex, octTbn)   },
        { .location = 2, .buffer_slot = 0, .format = VFORMAT_HALF2,  .offset = offsetof(ASkinedVertex, texCoord) },
        { .location = 3, .buffer_slot = 0, .format = VFORMAT_UBYTE4, .offset = offsetof(ASkinedVertex, joints)   },
        { .location = 4, .buffer_slot = 0, .format = VFORMAT_UINT,   .offset = offsetof(ASkinedVertex, weights)  }
    };

    SDL_GPUGraphicsPipelineCreateInfo sur_desc = {
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info     = (SDL_GPUGraphicsPipelineTargetInfo){
            .num_color_targets         = 1,
            .color_target_descriptions = &(SDL_GPUColorTargetDescription){ .format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT },
            .depth_stencil_format      = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            .has_depth_stencil_target  = true
        },
        .depth_stencil_state = (SDL_GPUDepthStencilState){
            .enable_depth_test  = true,
            .enable_depth_write = true,
            .compare_op         = SDL_GPU_COMPAREOP_LESS_OR_EQUAL
        },
        .multisample_state = (SDL_GPUMultisampleState){ .sample_count = SDL_GPU_SAMPLECOUNT_1 },
        .vertex_input_state = (SDL_GPUVertexInputState){
            .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(AVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
            .num_vertex_buffers = 1,
            .vertex_attributes = sur_attributes,
            .num_vertex_attributes = ARRAY_SIZE(sur_attributes)
        }
    };
    SDL_GPUGraphicsPipelineCreateInfo ski_desc = sur_desc;
    ski_desc.vertex_input_state = (SDL_GPUVertexInputState){
        .vertex_buffer_descriptions = &(SDL_GPUVertexBufferDescription){ 0, sizeof(ASkinedVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 },
        .num_vertex_buffers = 1,
        .vertex_attributes = ski_attributes,
        .num_vertex_attributes = ARRAY_SIZE(ski_attributes)
    };

    CreatePipelineWithDesc(&g_RenderState.surface, &sur_desc, sur_ver, sur_frag, sur_shadow_ver, sur_shadow_frag, sur_point_shadow_ver, sur_point_shadow_frag, sur_spot_shadow_ver, sur_spot_shadow_frag);
    CreatePipelineWithDesc(&g_RenderState.skinned, &ski_desc, ski_ver, ski_frag, ski_shadow_ver, ski_shadow_frag, ski_point_shadow_ver, ski_point_shadow_frag, ski_spot_shadow_ver, ski_spot_shadow_frag);
}
extern void InitShadows();

void InitRenderPipelines(void)
{
    InitSamplers();
    InitForwardPipelines();
    InitDepthOnlyPipelines();
    InitShadows();
    InitLinePipeline();
    InitGizmoLinePipeline();
    InitTerrainTrianglePipeline();
    InitOutlinePipeline();
    InitSlugPipeline();
    InitUIShapePipeline();
    InitUIImagePipeline();
    InitComputePipelines();
}

extern void DestroyShadows();

static void DestroyRenderSetBufferPipelines(RenderSetShared buffer)
{
    if (buffer.forwardPipeline)     SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, buffer.forwardPipeline);
    if (buffer.shadowPipeline)      SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, buffer.shadowPipeline);
    if (buffer.depthPipeline)       SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, buffer.depthPipeline);
    if (buffer.pointShadowPipeline) SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, buffer.pointShadowPipeline);
    if (buffer.spotShadowPipeline)  SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, buffer.spotShadowPipeline);
}

void DestroyRenderPipelines(void)
{
    DestroyShadows();
    DestroyRenderSetBufferPipelines(g_RenderState.surface);
    DestroyRenderSetBufferPipelines(g_RenderState.skinned);
    
	if (g_RenderState.linePipeline)          SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.linePipeline);
    if (g_OutlinePipeline)                   SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_OutlinePipeline);
    if (g_GizmoLinePipeline)                 SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_GizmoLinePipeline);
    if (g_TerrainTrianglePipeline)           SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_TerrainTrianglePipeline);
    if (g_TerrainTriangleDepthPipeline)      SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_TerrainTriangleDepthPipeline);
    if (g_RenderState.slugPipeline)          SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.slugPipeline);
    if (g_RenderState.slug2DPipeline)        SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.slug2DPipeline);
    if (g_RenderState.slugDepthPipeline)     SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.slugDepthPipeline);
    if (g_RenderState.uiShapePipeline)       SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.uiShapePipeline);
    if (g_RenderState.uiImagePipeline)       SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.uiImagePipeline);
    if (g_RenderState.transparentForwardPipeline) SDL_ReleaseGPUGraphicsPipeline(g_GPUDevice, g_RenderState.transparentForwardPipeline);
    
    if (g_AnimComputePipeline)             SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_AnimComputePipeline);
    if (g_AnimVerticesPipeline)            SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_AnimVerticesPipeline);
    if (g_CullDrawArgsComputePipeline)     SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_CullDrawArgsComputePipeline);
    if (g_BuildLightGridComputePipeline)   SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_BuildLightGridComputePipeline);
    if (g_ReconstructNormalComputePipeline) SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_ReconstructNormalComputePipeline);
    if (g_TonemapComputePipeline)          SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_TonemapComputePipeline);
    if (g_BloomDownsampleSPDComputePipeline) SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_BloomDownsampleSPDComputePipeline);
    if (g_BloomUpsampleComputePipeline)    SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_BloomUpsampleComputePipeline);
    if (g_HiZBuildComputePipeline)         SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_HiZBuildComputePipeline);
    if (g_HiZDownscaleSPDComputePipeline)  SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_HiZDownscaleSPDComputePipeline);
    if (g_HBAOComputePipeline)             SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_HBAOComputePipeline);
    if (g_HBAOBlurComputePipeline)         SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_HBAOBlurComputePipeline);
    if (g_ContactShadowsComputePipeline)   SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_ContactShadowsComputePipeline);
    if (g_MLAAEdgeMaskComputePipeline)     SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_MLAAEdgeMaskComputePipeline);
    if (g_MLAALineLengthComputePipeline)   SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_MLAALineLengthComputePipeline);
    if (g_MLAABlendComputePipeline)        SDL_ReleaseGPUComputePipeline(g_GPUDevice, g_MLAABlendComputePipeline);
}
