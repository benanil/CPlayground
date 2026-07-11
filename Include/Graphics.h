#ifndef _H_GRAPHICS_
#define _H_GRAPHICS_

#include <SDL3/SDL_gpu.h>
#include "Math/Half.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "GLTFParser.h"
#include "RenderLimits.h"

#define CHECK_CREATE(var, thing) { if (!(var)) { AX_ERROR("Failed to create %s: %s", thing, SDL_GetError()); /*Quit(2);*/ } }

#define TEXTURE_PAGE_SIZE       4096
#define TEXTURE_PAGE_LAYERS     64
#define MAX_SCENE_TEXTURES      1024
#define MAX_TEXTURE_DESCRIPTORS 2048
#define MAX_GPU_MATERIALS       2048
#define BLOOM_MAX_MIPS          16

#define BReadRasterBit   SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
#define BWriteComputeBit SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
#define BReadCompute     SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
#define BIndirectBit     SDL_GPU_BUFFERUSAGE_INDIRECT
#define BVertexBit       SDL_GPU_BUFFERUSAGE_VERTEX

#define VFORMAT_F32    SDL_GPU_VERTEXELEMENTFORMAT_FLOAT
#define VFORMAT_FLOAT3 SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3
#define VFORMAT_FLOAT4 SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4
#define VFORMAT_UINT   SDL_GPU_VERTEXELEMENTFORMAT_UINT
#define VFORMAT_UINT2  SDL_GPU_VERTEXELEMENTFORMAT_UINT2
#define VFORMAT_HALF2  SDL_GPU_VERTEXELEMENTFORMAT_HALF2
#define VFORMAT_HALF4  SDL_GPU_VERTEXELEMENTFORMAT_HALF4
#define VFORMAT_UBYTE4 SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4
#define VFORMAT_NBYTE4 SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM

#define TEX_DEPTH_STENCIL  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
#define TEX_COMP_READ      SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ
#define TEX_COMP_WRITE     SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE

#define TEX_COLOR_TARGET   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
#define TEX_SAMPLER        SDL_GPU_TEXTUREUSAGE_SAMPLER
#define TEX_SMP_CNT1       SDL_GPU_SAMPLECOUNT_1

#define TEX_FMT_D32_FLT  SDL_GPU_TEXTUREFORMAT_D32_FLOAT
#define TEX_FMT_D32_FLT2 SDL_GPU_TEXTUREFORMAT_R32G32_UINT
#define TEX_FMT_R32_FLT  SDL_GPU_TEXTUREFORMAT_R32_FLOAT
#define TEX_FMT_HALF4      SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
#define TEX_FMT_R32_UINT   SDL_GPU_TEXTUREFORMAT_R32_UINT
#define TEX_FMT_8UNORM4    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
#define TEX_FMT_8UNORM2    SDL_GPU_TEXTUREFORMAT_R8G8_UNORM
#define TEX_FMT_8UNORM1    SDL_GPU_TEXTUREFORMAT_R8_UNORM


#if defined(__cplusplus)
extern "C" {
#endif

enum TexFlags_
{
    TexFlags_None                = 0,
    TexFlags_MipMap              = 1,
    TexFlags_DontDeleteCPUBuffer = 2,

};
typedef s32 TexFlags;

enum GraphicType_
{
    GraphicType_Byte, // -> 0x1400 in opengl 
    GraphicType_UnsignedByte, 
    GraphicType_Short,
    GraphicType_UnsignedShort, 
    GraphicType_i32,
    GraphicType_Unsignedi32,
    GraphicType_Float,
    GraphicType_TwoByte,
    GraphicType_ThreeByte,
    GraphicType_FourByte,
    GraphicType_Double,
    GraphicType_Half, // -> 0x140B in opengl
    GraphicType_XYZ10W2, // GL_i32_2_10_10_10_REV

    GraphicType_Vector2f,
    GraphicType_Vector3f,
    GraphicType_Vector4f,

    GraphicType_Vector2i,
    GraphicType_Vector3i,
    GraphicType_Vector4i,
    // matrix types
    GraphicType_M22,
    GraphicType_M33,
    GraphicType_M44,

    GraphicType_NormalizeBit = 1 << 31
};
typedef s32 GraphicType;

// https://www.yosoygames.com.ar/wp/2018/03/vertex-formats-part-1-compression/
typedef struct AVertex_
{
    // xyz unorm16 normalized to the primitive AABB, w stores RGBA5551 vertex color
    u64 position; 
    u32 octTbn;
    u32 texCoord; // half2
} AVertex;

typedef struct ASkinedVertex_
{
    u32 positionXY;
    u32 positionZW;
    u32 octTbn;   // XY11Z10
    u32 texCoord; // half2
    u32 joints;   // rgb8u
    u32 weights;  // rgb8u
} ASkinedVertex;

// for sizeof only
typedef struct ALineVertex_
{
    f32 x, y, z;
    u32 color;
} ALineVertex;

enum LightType_
{
    LightType_Point = 0,
    LightType_Spot  = 1,
    LightType_Rect  = 2,
};
typedef u32 LightType;

#define LIGHT_FLAG_SHADOWED (1u << 0)
#define LIGHT_SHADOW_INDEX_INVALID 0xffffffffu

typedef struct LightGPU_
{
    f32 positionRadius[4];
    f32 directionCone[4];
    f32 colorIntensity[4];
    u32 type;
    u32 flags;
    u32 shadowIndex;
    u32 padding;
} LightGPU;

typedef struct LightDrawInfo_
{
    f32 uvRect[4];
    u32 lightIndex;
    u32 flags;
    u32 padding[2];
} LightDrawInfo;

typedef struct GPUMesh_
{
    s32 numVertex, numIndex;
    // unsigned because opengl accepts unsigned
    u32 vertexLayoutHandle;
    u32 indexHandle;
    u32 indexType;  // ui3232, ui3264. GL_BYTE + indexType
    u32 vertexHandle; // opengl handles for, POSITION, TexCoord...
    // usefull for knowing which attributes are there
    // POSITION, TexCoord... AAttribType_ bitmask
    s32 attributes;
    s32 stride; // size of an vertex of the mesh
    
    void* vertices;
    void* indices;
} GPUMesh;

typedef struct Texture_
{
    s32 width, height;
    SDL_GPUTexture* handle;
    SDL_GPUTextureFormat format;
    void* buffer;
    u64 bufferSize;
    u32 channels;
    u32 type;
    u32 mipLevels;
} Texture;

typedef struct TextureDescriptor_
{
    float2 uvScale;
    float2 uvBias;
    u32 pageIndex;
    u32 flags;
    u32 padding[2];
} TextureDescriptor;

#define MATERIAL_FLAG_ALPHA_MASK       (1u << 0)
#define MATERIAL_ALPHA_CUTOFF_SHIFT    8u
#define MATERIAL_ALPHA_CUTOFF_MASK     (0xffu << MATERIAL_ALPHA_CUTOFF_SHIFT)

typedef struct MaterialGPU_
{
    u32 albedoDescriptor;
    u32 normalDescriptor;
    u32 metallicRoughnessDescriptor;
    u32 flags;
    u32 baseColorFactor;
    u32 metallicRoughnessFactor;
    u32 padding[2];
} MaterialGPU;

typedef struct WindowState
{
    SDL_GPUTexture* tex_depth, *tex_hiz_depth, *tex_color, *tex_color_msaa, *tex_depth_msaa, *tex_post, *tex_hiz;
    SDL_GPUTexture* tex_hbao, *tex_hbao_blur, *tex_hbao_normal, *tex_contact_shadow;
    SDL_GPUTexture* tex_bloom_ping, *tex_bloom_pong;
    SDL_GPUTexture* tex_bloom_downsample[BLOOM_MAX_MIPS];
    SDL_GPUTexture* tex_bloom_upsample[BLOOM_MAX_MIPS];
    SDL_GPUTexture* tex_mlaa_edge_mask, *tex_mlaa_edge_count, *tex_mlaa_output;
    SDL_GPUTexture* tex_shadow_depth, *tex_shadow_color;
    SDL_GPUTexture* tex_point_shadow_depth, *tex_point_shadow_color;
    SDL_GPUTexture* tex_spot_shadow_depth, *tex_spot_shadow_color;
    u32 prev_width, prev_height;     // swapchain size, the ui renders at this resolution
    u32 render_width, render_height; // scene texture size, prev_* scaled by renderScale
    u32 bloom_width, bloom_height, bloom_mip_count;
    u32 hiz_width, hiz_height, hiz_mip_count;
    mat4x4 hiz_view_proj;
    bool hiz_valid;
} WindowState;

// per scene gpu mirrors of one render set, owned by Scene
typedef struct RenderSetBuffers_
{
    SDL_GPUBuffer* primitiveGroup;
    SDL_GPUBuffer* drawSparseIndices;
    SDL_GPUBuffer* drawArgs;
    SDL_GPUBuffer* sparseToDense;
    SDL_GPUBuffer* entity;
    SDL_GPUBuffer* visibleSparseIndices;
    SDL_GPUBuffer* visibilityMask;
    SDL_GPUBuffer* visibleCount;
    SDL_GPUBuffer* dispatchArgs;
} RenderSetBuffers;

// shared per set type: pipelines and the vertex pools every scene draws from
typedef struct RenderSetShared_
{
    SDL_GPUGraphicsPipeline* forwardPipeline; // Forward+ opaque pass (gated by FORWARD_PLUS)
    SDL_GPUGraphicsPipeline* transparentForwardPipeline;
    SDL_GPUGraphicsPipeline* depthPipeline;
    SDL_GPUGraphicsPipeline* shadowPipeline;
    SDL_GPUGraphicsPipeline* pointShadowPipeline;
    SDL_GPUGraphicsPipeline* spotShadowPipeline;
    SDL_GPUBuffer*           vertexBuffer;
    SDL_GPUBuffer*           animatedVertices; // skinned only
} RenderSetShared;

typedef struct RenderState
{
    SDL_GPUGraphicsPipeline* linePipeline;
    SDL_GPUGraphicsPipeline* slugPipeline;
    SDL_GPUGraphicsPipeline* slug2DPipeline;
    SDL_GPUGraphicsPipeline* slugDepthPipeline;
    SDL_GPUGraphicsPipeline* uiShapePipeline;
    SDL_GPUGraphicsPipeline* uiImagePipeline;
    SDL_GPUSampler*          sampler;
    SDL_GPUSampler*          hiZSampler;
    SDL_GPUSampler*          shadowSampler;
    SDL_GPUBuffer*           indexBuffer;
    SDL_GPUBuffer*           lineBuffer;
    SDL_GPUBuffer*           lineDrawArgsBuffer;
    SDL_GPUBuffer*           gizmoLineBuffer;
    SDL_GPUBuffer*           terrainChunkVertexBuffer; // GPU mirror of the tVertexData geometry heap
    SDL_GPUBuffer*           terrainChunkIndexBuffer;  // GPU mirror of the terrain index heap (TerrainIndexBuffer2)
    SDL_GPUBuffer*           terrainDrawArgsBuffer;    // SDL_GPUIndexedIndirectDrawCommand per visible chunk
    SDL_GPUBuffer*           lightBuffer;
    SDL_GPUBuffer*           pointShadowMatrixBuffer;
    SDL_GPUBuffer*           spotShadowMatrixBuffer;
    SDL_GPUBuffer*           lightVisibilityBuffer;
    SDL_GPUBuffer*           lightGridBuffer;     // Forward+: per-tile {offset,count}
    SDL_GPUBuffer*           lightIndexBuffer;    // Forward+: flat per-tile light index list
    SDL_GPUBuffer*           lightIndexCounter;   // Forward+: global allocator for lightIndexBuffer
    SDL_GPUBuffer*           uiShapeBuffer;
    SDL_GPUBuffer*           uiShapeDrawArgsBuffer;
    SDL_GPUBuffer*           shadowCascadeBuffer;
    RenderSetShared          skinned;
    RenderSetShared          surface;
    SDL_GPUTexture*          skyNoise3D;
    SDL_GPUSampleCount       sceneSampleCount;
    u32                      numLights;
} RenderState;


typedef struct Graphics_
{
    ASkinedVertex* SkinnedVertexBuffer;
    AVertex*       SurfaceVertexBuffer;
    void*          TerrainGrassBuffer;
    void*          TerrainVertNewBuffer;
    void*          TerrainSecondBuffer;
    void*          TerrainVertexBuffer;
    u32*           TerrainIndexBuffer;
    u32*           TerrainIndexBuffer2;
    u32*           IndexBuffer;
    u32            NumIndices;          // in use stats, not cursors
    u32            NumSkinnedVertices;
    u32            NumSurfaceVertices;
} Graphics;

static inline s32 GetRootNodeIdx(SceneBundle* bundle)
{
    s32 node = 0;
    if (bundle->numScenes > 0) {
        AScene defaultScene = bundle->scenes[bundle->defaultSceneIndex];
        node = defaultScene.nodes[0];
    }
    return node;
}

// per scene render set gpu buffers, implemented in Rendering.c
void CreateRenderSetBuffers(RenderSetBuffers* buffers, u32 maxEntities, u32 maxGroups);
void DestroyRenderSetBuffers(RenderSetBuffers* buffers);

enum GeometryBufferKind_
{
    GeometryBuffer_SkinnedVertex,
    GeometryBuffer_SurfaceVertex,
    GeometryBuffer_Index,
    GeometryBuffer_TerrainVertex,
    GeometryBuffer_TerrainIndex,
    GeometryBuffer_GrassInstance,
    GeometryBuffer_TerrainVertNew,
    GeometryBuffer_TerrainSecond, // secondary vertex
    GeometryBuffer_TerrainIndex2,
    GeometryBuffer_Count
};
typedef s32 GeometryBufferKind;

// sub allocation of the cpu/gpu mega buffers. tlsf runs directly over the cpu
// mirrors, allocations are over sized and rounded up to the element stride.
// raw is the heap pointer, keep it to shrink or free
#define GEOMETRY_ALLOC_FAIL 0xFFFFFFFFu

// out: element offset of count free elements, GEOMETRY_ALLOC_FAIL when full
u32  GeometryHeapAlloc(GeometryBufferKind kind, u32 count, void** raw);
// shrinks an allocation in place down to newCount elements past offset
void GeometryHeapShrink(GeometryBufferKind kind, void* raw, u32 offset, u32 newCount);
void GeometryHeapFree(GeometryBufferKind kind, void* raw);

// queues an element range of a cpu mega buffer for upload to its gpu mirror,
// flushed by the renderer once per frame (and on init, ranges queued before the
// gpu buffers exist are kept). implemented in Rendering.c
void Rendering_QueueGeometryUpload(GeometryBufferKind kind, u32 begin, u32 end);

void GraphicsInit(bool msaa);

// Applies g_RenderSettings.msaaSamples to the scene raster targets. Returns true when
// the active sample count changed and scene pipelines/textures must be recreated.
bool GraphicsApplyMSAASettings(void);
u32  GraphicsGetActiveMSAASamples(void);

void CreateWindowBuffers();

// scene texture resolution: the scene view size (window size when no scene view is
// active) scaled by g_RenderSettings.renderScale (clamped)
void GetRenderResolution(u32 windowW, u32 windowH, u32* outW, u32* outH);

// while a scene view window is open the 3d scene renders at its content size and the
// editor shows the texture inside the window instead of the fullscreen blit. 0 0 disables
void SetSceneViewSize(u32 width, u32 height);
// overrides w/h with the scene view size, returns true while one is active
bool GetSceneViewSize(u32* w, u32* h);

void GraphicsDestroy();

Texture rImportTexture(const char* path, TexFlags flags, const char* label);

Texture rCreateTexture(int width, int height, void* data, SDL_GPUTextureFormat format,
                       TexFlags flags, SDL_GPUTextureUsageFlags usage, const char* label);

Texture rCreateTexture2DArray(int width, int height, int layers, void* data, SDL_GPUTextureFormat format, 
                              TexFlags flags, SDL_GPUTextureUsageFlags usage, const char* label);

SDL_GPUTexture* CreateSceneColorTexture(u32 drawablew, u32 drawableh, SDL_GPUSampleCount sampleCount);

SDL_GPUTexture* CreateTexture2D(u32 width, u32 height,
                                SDL_GPUTextureFormat format,
                                SDL_GPUTextureUsageFlags usage,
                                SDL_GPUSampleCount sampleCount,
                                u32 mipLevels,
                                const char* label);

SDL_GPUTexture* CreateTexture2DArray(u32 width, u32 height, u32 layers,
                                     SDL_GPUTextureFormat format,
                                     SDL_GPUTextureUsageFlags usage,
                                     const char* label);

void rDeleteTexture(Texture texture);

void UploadTextureRegion(Texture texture, u32 layer, u32 x, u32 y, u32 width, u32 height, u32 srcWidth, u32 srcHeight, const void* data);

void GenerateTextureMips(Texture texture);

void ReleaseTexture(Texture* texture);

s32 GraphicsTypeToSize(GraphicType type);

SDL_GPUBuffer* CreateBuffer(const void* buffer, size_t bufferSize, SDL_GPUBufferUsageFlags bufferUsage, const char* debugName);

void UpdateGPUBuffer(SDL_GPUBuffer* buffer, const void* data, size_t bufferSize, size_t offset);

void UpdateGPUBufferCycle(SDL_GPUBuffer* buffer, const void* data, size_t bufferSize, size_t offset, bool cycle);

SDL_GPUTexture* CreateHiZDepthTexture(u32 drawablew, u32 drawableh);

SDL_GPUTexture* CreateHBAOTexture(u32 hbaoWidth, u32 hbaoHeight);

SDL_GPUTexture* CreateHiZTexture(u32 drawablew, u32 drawableh, u32* mipCount);

SDL_GPUTexture* Create3DNoise3DTexture(u32 size);

#if defined(__cplusplus)
}
#endif

#endif
