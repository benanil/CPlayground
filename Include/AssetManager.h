
#ifndef ASSET_MANAGER_H
#define ASSET_MANAGER_H

// game build doesn't have astc encoder, ufbx, dxt encoder. 
// because we are only decoding when we release the game
// if true, reduces exe size and you will have faster compile times.
// also it uses zstddeclib instead of entire zstd. (only decompression in game builds) go to CMakeLists.txt for more details
#if defined(__ANDROID__)
    #define AX_GAME_BUILD 1
#else
    #define AX_GAME_BUILD 0 /* make zero for editor build */
#endif


#include "Graphics.h"
#include "Async.h"

#if defined(__cplusplus)
extern "C" {
#endif

s32  LoadFBX(const char* path, SceneBundle* fbxScene, f32 scale);
s32  LoadOBJ(const char* path, SceneBundle* objScene, f32 scale);

// Parse a source mesh file (.gltf/.glb/.fbx/.obj) into an intermediate SceneBundle by extension.
s32 ImportBundle(const char* path, SceneBundle* scene, f32 scale);

/* Binary asset cache */
s32 SaveGLTFBinary(const SceneBundle* gltf, const char* path);

s32 LoadSceneBundleBinary(const char* path, SceneBundle* gltf, void** outVertexHeapPtr, void** outIndexHeapPtr);

s32 LoadBundleMeshCached(const char* path, SceneBundle* bundle, void** outVertexHeapPtr, void** outIndexHeapPtr, bool* outBaked);
s32 LoadBundleImagesFromCache(const char* gltfPath, SceneBundle* bundle, Texture* staging);

// ABM = AX binary mesh
u8 IsABMLastVersion(const char* path);
u8 IsMeshPath(const char* path);

/* Mesh baking */
// returns 0 on not enough memory
s32 BakeSceneMeshesAndAnimations(SceneBundle* gltf, void** outVertexHeapPtr, void** outIndexHeapPtr);

void GenerateLOD_75_GLTF(SceneBundle* sceneBundle);

void GenerateLOD_50_GLTF(SceneBundle* sceneBundle);

/* Animation baking */
void BakeGLTFAnimations(SceneBundle* gltf);

void CreateVerticesIndicesSkined(SceneBundle* gltf);

/* Texture/image cache */
u8 IsTextureLastVersion(const char* path);

void SaveSceneImages(SceneBundle* scene, const char* savePath, bool deleteRemaining);

void SaveSceneImagesAsync(SceneBundle* scene, const char* savePath, bool deleteRemaining, AsyncCallback callback);

// returns: 0 = noFile, 1 = success, 2 = missingImages, 3 = fileNumImage missmatch
s32 LoadSceneImages(const char* texturePath, Texture* textures, s32 numImages);


#if defined(__cplusplus)
}
#endif

#endif // ASSET_MANAGER_H
