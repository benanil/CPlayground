#ifndef _H_GRAPHICS_
#define _H_GRAPHICS_

#include "Math/Half.h"
#include "GLTFParser.h"

enum TexFlags_
{
    TexFlags_None        = 0,
    TexFlags_MipMap      = 1,
    TexFlags_Compressed  = 2,
    TexFlags_ClampToEdge = 4,
    TexFlags_Nearest     = 8,
    TexFlags_Linear      = 16, // default linear in desktop platforms
    TexFlags_DontDeleteCPUBuffer = 32,
    TexFlags_DynamicUpdate = 64, // the image content is updated infrequently by the CPU
    TexFlags_StreamUpdate = 128, // the image content is updated each frame by the CPU via
    TexFlags_RenderAttachment = 256,
    TexFlags_StorageAttachment = 512,
    // no filtering or wrapping
    TexFlags_RawData     = TexFlags_Nearest | TexFlags_ClampToEdge
};
typedef int TexFlags;

enum GraphicType_
{
    GraphicType_Byte, // -> 0x1400 in opengl 
    GraphicType_UnsignedByte, 
    GraphicType_Short,
    GraphicType_UnsignedShort, 
    GraphicType_Int,
    GraphicType_UnsignedInt,
    GraphicType_Float,
    GraphicType_TwoByte,
    GraphicType_ThreeByte,
    GraphicType_FourByte,
    GraphicType_Double,
    GraphicType_Half, // -> 0x140B in opengl
    GraphicType_XYZ10W2, // GL_INT_2_10_10_10_REV

    GraphicType_Vector2f,
    GraphicType_Vector3f,
    GraphicType_Vector4f,

    GraphicType_Vector2i,
    GraphicType_Vector3i,
    GraphicType_Vector4i,

    GraphicType_Matrix2,
    GraphicType_Matrix3,
    GraphicType_Matrix4,

    GraphicType_NormalizeBit = 1 << 31
};
typedef int GraphicType;

// https://www.yosoygames.com.ar/wp/2018/03/vertex-formats-part-1-compression/
typedef struct AVertex_
{
    Vec3f    position;
    uint32_t normal;
    uint32_t tangent;
    uint32_t texCoord; // half2
} AVertex;

typedef struct ASkinedVertex_
{
    Vec3f    position;
    uint32_t normal;
    uint32_t tangent;
    uint32_t texCoord; // half2
    uint32_t joints;  // rgb8u
    uint32_t weights; // rgb8u
} ASkinedVertex;


typedef struct GPUMesh_
{
    int numVertex, numIndex;
    // unsigned because opengl accepts unsigned
    unsigned int vertexLayoutHandle;
    unsigned int indexHandle;
    unsigned int indexType;  // uint32, uint64. GL_BYTE + indexType
    unsigned int vertexHandle; // opengl handles for, POSITION, TexCoord...
    // usefull for knowing which attributes are there
    // POSITION, TexCoord... AAttribType_ bitmask
    int attributes;
    int stride; // size of an vertex of the mesh
    
    void* vertices;
    void* indices;
} GPUMesh;

typedef struct Texture_
{
    int width, height;
    sg_image handle;
    sg_pixel_format format;
    void* buffer;
} Texture;

typedef struct Prefab_
{
    /* Union to contain all scene data directly in the struct */
    union {
        SceneBundle sceneBundle;
        struct {
            unsigned short numMeshes;
            unsigned short numNodes;
            unsigned short numMaterials;
            unsigned short numTextures;
            unsigned short numImages;
            unsigned short numSamplers;
            unsigned short numCameras;
            unsigned short numScenes;
            unsigned short defaultSceneIndex;
            unsigned short numBuffers;
            unsigned short numAnimations;
            unsigned short numSkins;
            AErrorType error;
            void* stringAllocator;
            void* intAllocator;
            void* allVertices;
            void* allIndices;
            int totalVertices;
            int totalIndices;
            float scale;
            GLTFBuffer* buffers;
            AMesh*      meshes;
            ANode*      nodes;
            AMaterial*  materials;
            ATexture*   textures;
            AImage*     images;
            ASampler*   samplers;
            ACamera*    cameras;
            AScene*     scenes;
            AAnimation* animations;
            ASkin*      skins;
        };
    };
    
    /* Prefab-specific members */
    Texture* gpuTextures;
    GPUMesh  bigMesh;           /* contains all of the vertices and indices of an prefab */
    Matrix4* globalNodeTransforms; /* pre calculated global transforms, accumulated with parents */
    // struct TLAS* tlas;
    int firstTimeRender;        /* starts with 4 and decreases until its 0 we draw first time and set this to-1 */
    char path[256];             /* relative path */
} Prefab;

Texture GetGPUTexture(Prefab* prefab, int index)
{
    return prefab->gpuTextures[prefab->textures[index].source];
}

ANode* GetNodePtr(Prefab* prefab, int index)
{
    return &prefab->nodes[index];
}

int GetRootNodeIdx(SceneBundle* bundle)
{
    int node = 0;
    if (bundle->numScenes > 0) {
        AScene defaultScene = bundle->scenes[bundle->defaultSceneIndex];
        node = defaultScene.nodes[0];
    }
    return node;
}

// https://copyprogramming.com/howto/how-to-pack-normals-into-gl-int-2-10-10-10-rev
static inline uint32_t Pack_INT_2_10_10_10_REV(Vec3f v) {
    const uint32_t xs = v.x < 0.0f, ys = v.y < 0.0f, zs = v.z < 0.0f;   
    return zs << 29 | ((uint32_t)(v.z * 511 + (zs << 9)) & 511) << 20 |
        ys << 19 | ((uint32_t)(v.y * 511 + (ys << 9)) & 511) << 10 |
        xs << 9  | ((uint32_t)(v.x * 511 + (xs << 9)) & 511);
}

static inline uint32_t Pack_INT_2_10_10_10_REV_VEC(Vector4x32f v)
{
	float x = VecGetX(v), y = VecGetY(v), z = VecGetZ(v), w = VecGetW(v); 

    const uint32_t xs = x < 0.0f, ys = y < 0.0f, zs = z < 0.0f, ws = w < 0.0f;
    return ws << 31 | ((uint32_t)(w       + (ws << 1)) & 1) << 30 |
        zs << 29 | ((uint32_t)(z * 511 + (zs << 9)) & 511) << 20 |
        ys << 19 | ((uint32_t)(y * 511 + (ys << 9)) & 511) << 10 |
        xs << 9  | ((uint32_t)(x * 511 + (xs << 9)) & 511);
}

static inline Vec3f Unpack_INT_2_10_10_10_REV(uint32_t p) 
{
    Vec3f result;
    const uint32_t tenMask = (1 << 10)-1;
    result.x = 255.0f / ((p >>  0) & tenMask);
    result.y = 255.0f / ((p >> 10) & tenMask);
    result.z = 255.0f / ((p >> 20) & tenMask);
    return result;
}

// w value is undefined, it could be anything or trash data
static inline Vector4x32f GetPosition(GPUMesh* gpu, int index)
{
    const char* bytePtr = (const char*)gpu->vertices;
    bytePtr += gpu->stride * index;
    return VecLoad((const float*)bytePtr);
}

// todo GPUMesh GetNormal
static inline Vector4x32f GetNormal(GPUMesh* gpu, int index)
{
    const char* bytePtr = (const char*)gpu->vertices;
    bytePtr += gpu->stride * index + sizeof(Vec3f); // skip position
    uint32_t normalPacked = *(uint32_t *)bytePtr;
    typedef union S_ { Vector4x32f v; Vec3f s; } S;
    S s = (S){ .s = Unpack_INT_2_10_10_10_REV(normalPacked) };
    return s.v; // VecLoad(bytePtr);
}

// todo GPUMesh GetNormal
static inline Vec2f GetUV(GPUMesh* gpu, int index)
{
    const char* bytePtr = (const char*)gpu->vertices;
    bytePtr += gpu->stride * index + sizeof(Vec3f) + sizeof(int) + sizeof(int); // skip normal and tangent
    uint32_t uvPacked = *(uint32_t *)bytePtr;
    Vec2f result;
    ConvertHalf2ToFloat2(&result.x, uvPacked); // VecLoad(bytePtr);
    return result;
}


uint8_t rTextureTypeToBytesPerPixel(sg_pixel_format type);

void rInit();

void rDestroy();

int rGetMipmapImageData(sg_image_data* img_data, void* data, int width, int height);

Texture rImportTexture(const char* path, TexFlags flags, const char* label);

void rDeleteMipData(sg_image_data* img_data, int numMips);

Texture rCreateTexture(int width, int height, void* data, sg_pixel_format format, TexFlags flags, const char* label);

void rDeleteTexture(Texture texture);

#endif