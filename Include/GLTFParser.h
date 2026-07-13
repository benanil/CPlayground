
#ifndef ASTL_GLTF_PARSER
#define ASTL_GLTF_PARSER

#include "Common.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Warning! order is important, dependent in functions: ParseMeshes, ParseAttributes, ParseGLTF... 
enum AAttribType_
{
    AAttribType_POSITION   = 1 << 0,
    AAttribType_TEXCOORD_0 = 1 << 1,
    AAttribType_NORMAL     = 1 << 2,
    AAttribType_TANGENT    = 1 << 3,
    AAttribType_TEXCOORD_1 = 1 << 4,
    AAttribType_JOINTS     = 1 << 5,
    AAttribType_WEIGHTS    = 1 << 6,
    AAttribType_COLOR_0    = 1 << 7,
    AAttribType_Count      = 8 ,
    AAttribType_MAKE32BIT  = 1 << 31
};
typedef int AAttribType;

enum AAttribIdx_
{
    AAttribIdx_POSITION   = 0,
    AAttribIdx_TEXCOORD_0 = 1,
    AAttribIdx_NORMAL     = 2,
    AAttribIdx_TANGENT    = 3,
    AAttribIdx_TEXCOORD_1 = 4,
    AAttribIdx_JOINTS     = 5,
    AAttribIdx_WEIGHTS    = 6,
    AAttribIdx_COLOR_0    = 7,
    AAttribIdx_Count      = 8,
};
typedef int AAttribIdx;

enum AErrorType_
{
    AError_NONE,
    AError_UNKNOWN,
    AError_UNKNOWN_ATTRIB,
    AError_UNKNOWN_MATERIAL_VAR,
    AError_UNKNOWN_PBR_VAR,
    AError_UNKNOWN_NODE_VAR,
    AError_UNKNOWN_TEXTURE_VAR,
    AError_UNKNOWN_ACCESSOR_VAR,
    AError_UNKNOWN_BUFFER_VIEW_VAR,
    AError_UNKNOWN_MESH_VAR,
    AError_UNKNOWN_CAMERA_VAR,
    AError_UNKNOWN_MESH_PRIMITIVE_VAR,
    AError_BUFFER_PARSE_FAIL,
    AError_BIN_NOT_EXIST,
    AError_FILE_NOT_FOUND,
    AError_UNKNOWN_DESCRIPTOR,
    AError_HASH_COLISSION,
    AError_NON_UTF8,
    AError_EXT_NOT_SUPPORTED, // scenes other than GLTF, OBJ or Fbx
    AError_CloseBrackets,
    AError_GLB_PARSING_FAILED,
    AError_MAX
};
typedef int AErrorType;

enum AComponentType_
{
    // GL_TYPE starts from 0x1400
    AComponentType_BYTE          ,
    AComponentType_UNSIGNED_BYTE ,
    AComponentType_SHORT         ,
    AComponentType_UNSIGNED_SHORT,
    AComponentType_INT           ,
    AComponentType_UNSIGNED_INT  ,
    AComponentType_FLOAT         
};
typedef int AComponentType;

enum AMaterialAlphaMode_
{
    AMaterialAlphaMode_Opaque, AMaterialAlphaMode_Blend, AMaterialAlphaMode_Mask
};
typedef int AMaterialAlphaMode;

// original value multiplied by 400 to get real value: float scale = ((float)scale) / 400.0f; 
typedef short float16;

typedef struct GLTFTexture_
{
    float16 scale;
    float16 strength;
    unsigned short index; // index to texture path. -1 if is not exist
    unsigned short texCoord;
} GLTFTexture; // normalTexture, occlusionTexture, emissiveTexture

static inline float16 MakeFloat16(float x) { return (float16)(x * 400.0f); }

typedef struct AMaterial_
{
    // pbrMetallicRoughness in gltf. but baseColorFactor is below
    GLTFTexture baseColorTexture;
    GLTFTexture specularTexture;
    GLTFTexture metallicRoughnessTexture;
    unsigned short metallicFactor;  // (float)(metallicFactor) / UINT16_MAX to get value
    unsigned short roughnessFactor; // (float)(roughnessFactor) / UINT16_MAX to get value

    char* name;
    float16 emissiveFactor[3];
    float16 specularFactor;
    unsigned diffuseColor, specularColor, baseColorFactor;
    float alphaCutoff;
    float ior;
    bool doubleSided;
    AMaterialAlphaMode alphaMode;
    GLTFTexture textures[3];

    #ifdef __cplusplus
    const GLTFTexture& GetNormalTexture()    const { return textures[0]; }
    const GLTFTexture& GetOcclusionTexture() const { return textures[1]; }
    const GLTFTexture& GetEmissiveTexture()  const { return textures[2]; }
    #endif
} AMaterial;

typedef struct AImage_
{
    char* path;
    // -1, or index
    int bufferViewIndex;
} AImage;

typedef struct ANode_
{
    // Warning! order is important, we copy memory in assetmanager.cpp from fbx transform(pos,rot, scale)
    AX_ALIGN(16) float translation[4];
    AX_ALIGN(16) float rotation[4];
    AX_ALIGN(16) float scale[4];

    int   type;  // 0 mesh or 1 camera
    int   index; // index of mesh or camera, -1 if node doesn't have mesh or camera
    int   skin; 
    int   numChildren;
    int   parent;
    char* name;
    int*  children;
} ANode;

typedef struct AMorphTarget_
{
    AAttribType attributes;
    // accessor indices of: position, texcoord, tangent respectively
    unsigned short indexes[4]; 
} AMorphTarget;

typedef struct APrimitive_
{
    // pointers to binary file to lookup position, texture, normal..
    void* indices; 
    void* vertices;
    int lodIndexOffset[4];
    int lodNumIndices[4];
    int lodVertexOffset[4];
    int lodNumVertices[4];
    
    unsigned bvhNodeIndex;

    unsigned attributes; // AAttribType Position, Normal, TexCoord, Tangent, masks
    int indexType; // GraphicType_UnsignedInt, GraphicType_UnsignedShort.. 
    int numIndices;
    int numVertices;
    int indexOffset;
    int lodPadding;
    
    short jointType;   // GraphicType_UnsignedInt, GraphicType_UnsignedShort.. 
    short jointCount;  // per vertex bone count (joint), 1-4
    short jointStride; // lets say index data is rgba16u  [r, g, b, a, .......] stride might be bigger than joint

    // weight count is equal to joint count
    short weightType;   // GraphicType_UnsignedInt, GraphicType_UnsignedShort.. 
    short weightStride; // lets say index data is rgba16u  [r, g, b, a, .......] stride might be bigger than joint

    short colorType;
    short colorCount;
    short colorStride;
    
    // internal use only. after parsing this is useless
    unsigned short indiceIndex; // indice index to accessor
    unsigned short material;    // material index
    unsigned short mode;        // 4 is triangle
    bool hasOutline;

    // when we are parsing we use this as an indicator to accessor.
    // after parsing, this will become vertex pointers AAttribType_Position, AAttribType_TexCoord...
    // positions = (Vector3f*)vertexAttribs[0];
    // texCoords = (Vec2f*)vertexAttribs[1]; // note that tangent is vec4
    // ...
    void* vertexAttribs[AAttribType_Count]; 
    
    // AABB min and max
    AX_ALIGN(16) float min[4];
    AX_ALIGN(16) float max[4];
    
    AMorphTarget* morphTargets; // num morph targets is equal to mesh.numMorphWeights
} APrimitive;

typedef struct AMesh_
{
    char* name;
    APrimitive* primitives;
    int numPrimitives;
    int numMorphWeights;
    int primitiveOffset; // scenebundle relative primitiveIdx
    float* morphWeights;
} AMesh;

typedef struct ATexture_
{
    int   sampler; // sampler index
    int   source; // image index
    char* name;
} ATexture;

typedef struct ACamera_
{
    union {
        struct { float aspectRatio, yFov; };
        struct { float xmag, ymag; };
    };
    float zFar, zNear;
    int type; // 0 orthographic, 1 perspective
    char* name;
} ACamera;

typedef struct ASampler_
{
    char magFilter; // 0 = GL_NEAREST = 0x2600 = 9728, or 1 = 0x2601 = GL_LINEAR = 9729
    char minFilter; // 0 or 1 like above
    char wrapS; // 0 GL_REPEAT, 1 GL_CLAMP_TO_EDGE, 2 GL_CLAMP_TO_BORDER, 3 GL_MIRRORED_REPEAT
    char wrapT; //       10497               33071                 33069                 33648
} ASampler;


typedef struct AScene_
{
    char* name;
    int   numNodes;
    int*  nodes;
} AScene;

typedef struct GLTFBuffer_
{
    void* uri;
    int byteLength;
} GLTFBuffer;

typedef struct ASkin_
{
    int skeleton; // < index of the root node
    int numJoints;
    float *inverseBindMatrices; // matrix4x4 * numJoints
    int   *joints; // < indices of bone nodes 
    char  *name;
} ASkin;

enum AAnimTargetPath_ {
    AAnimTargetPath_Translation, 
    AAnimTargetPath_Rotation, 
    AAnimTargetPath_Scale,
    AAnimTargetPath_Weight,
    AAnimTargetPath_Make32Bit = 1 << 31
};
typedef int AAnimTargetPath;

enum ASamplerInterpolation_ {
    ASamplerInterpolation_Linear, 
    ASamplerInterpolation_Step, 
    ASamplerInterpolation_CubicSpline
};
typedef int ASamplerInterpolation;

typedef struct AAnimChannel_
{
    int sampler;
    int targetNode;
    AAnimTargetPath targetPath; 
    int pad;
} AAnimChannel;

typedef struct AAnimSampler_
{
    float* input;
    float* output; // quaternion or vector array
    int count;
    int numComponent; // 3,4 vec3 or vec4
    AComponentType inputType;
    AComponentType outputType;
    ASamplerInterpolation interpolation;
} AAnimSampler;

typedef struct AAnimation_
{
    int numSamplers;
    int numChannels;
    float duration; // total duration
    float speed; // 1.0 by default, most of the time 1.0
    AAnimChannel* channels;
    AAnimSampler* samplers;
    char* name;
} AAnimation;

typedef struct SceneBundle_
{
    int numMeshes;
    int numNodes;
    int numMaterials;
    int numTextures;
    int numImages;
    int numSamplers;
    int numCameras;
    int numScenes;
    int defaultSceneIndex;
    int numBuffers;
    int numAnimations;
    int numSkins;
    int rootNode;
    int totalPrimitives;

    AErrorType error;

    void* allVertices;
    void* allIndices;

    int   totalVertices;
    int   totalIndices;
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
    void*       allocator;
} SceneBundle;

// outScene should not be null
int ParseGLTF(const char* path, SceneBundle* outScene, float scale);

void FreeGLTFBuffers(SceneBundle* gltf);

void FreeSceneBundle(SceneBundle* gltf);

const char* ParsedSceneGetError(AErrorType error);

int Prefab_FindAnimRootNodeIndex(const SceneBundle* prefab);

int Prefab_FindNodeFromName(const SceneBundle* prefab, const char* name);

/* Scene normalization */
void SceneBundle_BuildParentIndices(SceneBundle* scene);
void SceneBundle_FlattenNodes(SceneBundle* scene);
void SceneBundle_ValidateNodeHierarchy(const SceneBundle* scene);
void SceneBundle_Normalize(SceneBundle* scene);

static inline int GraphicsTypeToSize(AComponentType type)
{
    // BYTE, UNSIGNED_BYTE, SHORT, UNSIGNED_SHORT, INT, UNSIGNED_INT, FLOAT 
    const int TypeToSize[12] = { 1, 1, 2, 2, 4, 4, 4, 2, 4, 4, 8, 2 };
    return TypeToSize[type];
}

#if defined(__cplusplus)
}
#endif


#endif // ASTL_GLTF_PARSER
