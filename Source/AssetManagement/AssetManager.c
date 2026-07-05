/*********************************************************************************
*    Purpose:                                                                    *
*         Saves or Loads given FBX or GLTF scene, as binary                      *
*         Compresses Vertices for GPU using half precison and xyz10w2 format.    *
*         Uses zstd to reduce size on disk                                       *
*    Author:                                                                     *
*        Anilcan Gulkaya 2024 anilcangulkaya7@gmail.com github @benanil          *
*********************************************************************************/

#include "Include/AssetManager.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"
#include "Include/Algorithm.h"
#include "Include/Animation.h" // maxBonePoses
#include "Include/Memory.h"

// #include "Scene.h"

#if !AX_GAME_BUILD
	#include "Extern/ufbx.h"
	#include "Extern/meshoptimizer/src/meshoptimizer.h"
#endif

#include "Math/Matrix.h"
#include "Math/Color.h"
#include "Math/Bitpack.h"
#include "Include/FileSystem.h"
#include "Include/Common.h"
#include "Include/BasisBinding.h"
#include "Include/FastDelta.h"

#define SINFL_IMPLEMENTATION
#define SDEFL_IMPLEMENTATION
#include "Extern/sdefl.h"
#include "Extern/sinfl.h"
#include "Include/DataStructures/Array.h"

#include "Include/BasisCompressWrapper.h"

extern Graphics gGFX;


/*//////////////////////////////////////////////////////////////////////////*/
/*                              FBX LOAD                                    */
/*//////////////////////////////////////////////////////////////////////////*/

#if !AX_GAME_BUILD

static u16 GetFBXTexture(const ufbx_material* umaterial, const ufbx_scene* uscene, ufbx_material_feature feature, ufbx_material_pbr_map pbr, ufbx_material_fbx_map fbx)
{
    (void)uscene;
    if (umaterial->features.features[feature].enabled)
    {
        ufbx_texture* texture = NULL;
        // search for normal texture
        if (umaterial->features.pbr.enabled)
            texture = umaterial->pbr.maps[pbr].texture;

        if (!texture)
            texture = umaterial->fbx.maps[fbx].texture;

        if (texture)
        {
            // typed_id is the texture's index within uscene->textures (which we mirror 1:1).
            return (u16)texture->typed_id;
        }
    }
    return -1;
}

// ufbx folds the legacy FBX TransparencyFactor/Color into pbr.opacity, so relying on the
// normalized pbr value avoids double counting. Default opacity is 1.0 (fully opaque).
static f32 GetUFBXOpacity(const ufbx_material* material)
{
    f32 opacity = 1.0f;
    if (material->pbr.opacity.has_value || material->pbr.opacity.value_components >= 1)
        opacity = (f32)material->pbr.opacity.value_real;
    return Saturatef32(opacity);
}

static u32 PackUFBXBaseColorFactor(const ufbx_material* material, f32 opacity)
{
    ufbx_vec3 color = { 1.0f, 1.0f, 1.0f };
    f32 factor = 1.0f;

    // Only override the white/1.0 defaults when the material actually defines the value.
    // ufbx always fills value_vec3/value_real with a fallback (often 0) even when the
    // property is unset, so gating on value_components would tint everything black.
    if (material->features.pbr.enabled)
    {
        if (material->pbr.base_color.has_value)         color  = material->pbr.base_color.value_vec3;
        else if (material->fbx.diffuse_color.has_value) color = material->fbx.diffuse_color.value_vec3;

        if (material->pbr.base_factor.has_value)       factor = (f32)material->pbr.base_factor.value_real;
    }
    else
    {
        if (material->fbx.diffuse_color.has_value)     color  = material->fbx.diffuse_color.value_vec3;
        else if (material->pbr.base_color.has_value)   color  = material->pbr.base_color.value_vec3;

        if (material->fbx.diffuse_factor.has_value)    factor = (f32)material->fbx.diffuse_factor.value_real;
    }
	return PackColor3PtrToUint(&color.x);
}

static char* GetNameFromFBX(ufbx_string ustr, FixedPow2Allocator* stringAllocator)
{
    if (ustr.length == 0) return NULL;
    char* name = FixedPow2Allocator_AllocateUninitialized(stringAllocator, ustr.length + 1);
    SmallMemCpy(name, ustr.data, ustr.length);
    name[ustr.length] = 0;
    return name;
}

static bool CopyUFBXPath(char* dst, s32 dstSize, const char* baseDir, s32 baseLen, ufbx_string path)
{
    if (dstSize <= 0) return false;
    dst[0] = '\0';
    if (path.length == 0 || path.length >= (size_t)dstSize) return false;

    s32 prefixLen = baseDir ? baseLen : 0;
    s32 copyLen = (s32)path.length;
    if (copyLen <= 0 || prefixLen + copyLen >= dstSize) return false;

    if (prefixLen > 0)
        SmallMemCpy(dst, baseDir, prefixLen);
    SmallMemCpy(dst + prefixLen, path.data, copyLen);
    dst[prefixLen + copyLen] = '\0';
    return true;
}

static s32 CopyUFBXFilename(char* dst, s32 dstSize, ufbx_string path)
{
    if (dstSize <= 0) return 0;
    dst[0] = '\0';
    if (path.length == 0 || path.length >= (size_t)dstSize) return 0;

    s32 start = (s32)path.length;
    while (start > 0 && path.data[start - 1] != '/' && path.data[start - 1] != '\\')
        start--;

    s32 copyLen = (s32)path.length - start;
    if (copyLen >= dstSize) copyLen = dstSize - 1;
    if (copyLen <= 0) return 0;

    SmallMemCpy(dst, path.data + start, copyLen);
    dst[copyLen] = '\0';
    return copyLen;
}

// Resolve an FBX texture file to a real path on this machine so the basis encoder
// (SaveSceneImages) has a source to compress. Prefers <fbxDir>/<relative_filename> because
// the absolute path baked into an FBX usually points at the artist's drive; embedded image
// blobs are written out next to the FBX. Returns an allocator-owned, possibly-empty path.
static char* ResolveFBXImagePath(const char* fbxBaseDir, s32 fbxBaseLen,
                                 ufbx_string filename, ufbx_string absolute_filename, ufbx_string relative_filename,
                                 ufbx_blob content, s32 index, FixedPow2Allocator* allocator)
{
    enum { PATH_CAP = 1024 };
    char* out = (char*)FixedPow2Allocator_AllocateUninitialized(allocator, PATH_CAP);
    out[0] = '\0';

    if (content.size > 0)
    {
        // Embedded image data: derive a file name and write the blob beside the FBX.
        char nameBuf[256];
        s32 nameLen = 0;
        const ufbx_string* src = filename.length ? &filename
                               : (relative_filename.length ? &relative_filename : NULL);
        if (src)
        {
            nameLen = CopyUFBXFilename(nameBuf, sizeof(nameBuf), *src);
        }
        else
        {
            const char prefix[] = "fbx_embedded_";
            SmallMemCpy(nameBuf, prefix, sizeof(prefix) - 1);
            s32 n = IntToString(nameBuf + sizeof(prefix) - 1, index, 0);
            SmallMemCpy(nameBuf + sizeof(prefix) - 1 + n, ".png", 4);
            nameLen = (s32)(sizeof(prefix) - 1) + n + 4;
        }
        nameBuf[nameLen] = '\0';

        if (fbxBaseLen + nameLen < PATH_CAP)
        {
            SmallMemCpy(out, fbxBaseDir, fbxBaseLen);
            SmallMemCpy(out + fbxBaseLen, nameBuf, nameLen + 1);
            WriteAllBytes(out, (const char*)content.data, (unsigned long)content.size);
        }
        else
        {
            AX_WARN("fbx embedded texture path too long, skipping image %d: %s", index, nameBuf);
            out[0] = '\0';
        }
        return out;
    }

    // External file: FBX-relative path resolves on this machine, try it first.
    if (relative_filename.length && fbxBaseLen + (s32)relative_filename.length < PATH_CAP)
    {
        CopyUFBXPath(out, PATH_CAP, fbxBaseDir, fbxBaseLen, relative_filename);
    }
    // Fall back to the stored absolute path, then the raw filename.
    if ((out[0] == '\0' || !FileExist(out)) && absolute_filename.length && absolute_filename.length < PATH_CAP)
        CopyUFBXPath(out, PATH_CAP, NULL, 0, absolute_filename);
    if (out[0] == '\0' && filename.length && filename.length < PATH_CAP)
        CopyUFBXPath(out, PATH_CAP, NULL, 0, filename);

    if (out[0] == '\0' || !FileExist(out))
        AX_WARN("fbx texture source not found on disk, compression skipped for image %d: %s", index, out[0] ? out : "<empty>");

    return out;
}
#endif

s32 LoadFBX(const char* path, SceneBundle* fbxScene, f32 scale)
{
#if !AX_GAME_BUILD
    s32 pathLen = StringLength(path);
    bool isObj = FileHasExtension(path, pathLen, ".obj");
    const char* importType = isObj ? "obj" : "fbx";

    MemsetZero(fbxScene, sizeof(SceneBundle));
    AX_LOG("%s import: %s scale=%f", importType, path, scale);

    ufbx_load_opts opts = { 0 };
    // ufbx imports FBX/OBJ as static scene data here; animation stacks are intentionally ignored.
    opts.evaluate_skinning = false;
    opts.evaluate_caches = false;
    opts.load_external_files = isObj; // OBJ needs this for implicitly referenced .mtl files.
    opts.generate_missing_normals = true;
    opts.ignore_missing_external_files = true;
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f * (1.0f / scale);
    opts.obj_search_mtl_by_filename = true;

    opts.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_ABORT_LOADING;
    
    ufbx_error error;
    ufbx_scene* uscene;
    
    uscene = ufbx_load_file((const char*)path, &opts, &error);
    
    if (!uscene) {
        AX_WARN("%s mesh load failed! %s", importType, error.info);
        return 0;
    }    
    
    fbxScene->numMeshes     = (u16)uscene->meshes.count;
    fbxScene->numNodes      = (u16)uscene->nodes.count;
    fbxScene->numMaterials  = (u16)uscene->materials.count;
    fbxScene->numImages     = (u16)uscene->texture_files.count; 
    fbxScene->numTextures   = (u16)uscene->textures.count;
    fbxScene->numCameras    = (u16)uscene->cameras.count;
    fbxScene->totalVertices = 0;
    fbxScene->totalIndices  = 0;
    fbxScene->numScenes     = 1;
    fbxScene->defaultSceneIndex = 0;
    fbxScene->numAnimations = 0;
    AX_LOG("%s parse: nodes=%d meshes=%d materials=%d textures=%d cameras=%d skins=%d",
           importType,
           fbxScene->numNodes, fbxScene->numMeshes, fbxScene->numMaterials,
           fbxScene->numTextures, fbxScene->numCameras, (s32)uscene->skin_deformers.count);
    if (uscene->skin_deformers.count > 0)
        AX_WARN("%s contains skin deformers, but this importer is intended for non-skinned meshes only: %s", importType, path);
    
    FixedPow2Allocator* allocator = AllocateTLSFGlobal(sizeof(FixedPow2Allocator));
    FixedPow2Allocator_Init(allocator, 2048);

    // Static import only: skinning and animation are intentionally dropped so the
    // baker selects the surface (non-skinned) vertex path. See plan: skip animation.
    fbxScene->numSkins      = 0;
    fbxScene->skins         = NULL;
    fbxScene->numAnimations = 0;
    fbxScene->animations    = NULL;

    if (fbxScene->numMeshes) fbxScene->meshes = (AMesh*)AllocZeroTLSFGlobal(fbxScene->numMeshes, sizeof(AMesh));
    fbxScene->scenes = (AScene*)AllocZeroTLSFGlobal(1, sizeof(AScene));

    s32 totalIndices = 0, totalVertices = 0;

    // Build the same intermediate representation ParseGLTF produces: per-primitive raw
    // float attribute arrays + an index buffer. BakeSceneMeshesAndAnimations() then
    // compresses, generates LODs and uploads to the GPU geometry heap, identical to GLTF.
    for (s32 i = 0; i < fbxScene->numMeshes; i++)
    {
        AMesh* amesh = &fbxScene->meshes[i];
        ufbx_mesh* umesh = uscene->meshes.data[i];
        amesh->name = GetNameFromFBX(umesh->name, allocator);
        amesh->primitives = ArrayCreatePrealloc(APrimitive, 1);
        amesh->numPrimitives = 1;

        APrimitive* primitive = &amesh->primitives[0];
        MemsetZero(primitive, sizeof(APrimitive));
        primitive->material    = 0;
        primitive->mode        = 4;
        primitive->indiceIndex = UINT16_MAX;
        primitive->indexType   = AComponentType_UNSIGNED_INT;

        if (umesh->num_triangles == 0 || umesh->num_vertices == 0)
        {
            AX_WARN("fbx mesh %d has no renderable triangles/vertices", i);
            continue;
        }

        bool hasUV     = umesh->vertex_uv.exists;
        bool hasNormal = umesh->vertex_normal.exists;
        bool hasColor  = umesh->vertex_color.exists;
        if (!hasUV)
            AX_WARN("fbx mesh %d (%s) has no UVs", i, amesh->name ? amesh->name : "<unnamed>");

        // Flatten every triangulated face corner into temporary attribute streams.
        // cornerCapacity is fixed for the arena push/pop pairing; numCorners is the
        // actually-emitted count fed to meshopt (they match unless a face is degenerate).
        s32 cornerCapacity = (s32)umesh->num_triangles * 3;
        float3* cornerPos = (float3*)ArenaPushGlobal((u64)cornerCapacity * sizeof(float3));
        float2* cornerUV  = hasUV     ? (float2*)ArenaPushGlobal((u64)cornerCapacity * sizeof(float2)) : NULL;
        float3* cornerNrm = hasNormal ? (float3*)ArenaPushGlobal((u64)cornerCapacity * sizeof(float3)) : NULL;
        v128f* cornerColor = hasColor ? (v128f*)ArenaPushGlobal((u64)cornerCapacity * sizeof(v128f)) : NULL;

        u32 triIndices[64];
        s32 corner = 0;
        for (s32 f = 0; f < (s32)umesh->faces.count; f++)
        {
            ufbx_face face = umesh->faces.data[f];
            u32 numTris = ufbx_triangulate_face(triIndices, ARRAY_SIZE(triIndices), umesh, face);
            for (u32 c = 0; c < numTris * 3u && corner < cornerCapacity; c++, corner++)
            {
                u32 ci = triIndices[c];
                ufbx_vec3 p = ufbx_get_vertex_vec3(&umesh->vertex_position, ci);
                cornerPos[corner] = (float3){ p.x, p.y, p.z };
                if (hasUV)
                {
                    // FBX UVs are bottom-left origin (V up); the rest of the pipeline assumes
                    // glTF top-left origin (V down), so flip V to match baked glTF meshes.
                    ufbx_vec2 uv = ufbx_get_vertex_vec2(&umesh->vertex_uv, ci);
                    cornerUV[corner] = (float2){ uv.x, 1.0f - uv.y };
                }
                if (hasNormal)
                {
                    ufbx_vec3 n = ufbx_get_vertex_vec3(&umesh->vertex_normal, ci);
                    cornerNrm[corner] = (float3){ n.x, n.y, n.z };
                }
                if (hasColor)
                {
                    ufbx_vec4 color = ufbx_get_vertex_vec4(&umesh->vertex_color, ci);
                    cornerColor[corner] = VecSetR((f32)color.x, (f32)color.y, (f32)color.z, (f32)color.w);
                }
            }
        }
        s32 numCorners = corner;

        // Collapse binary-equal corners into unique vertices + a real index buffer.
        struct meshopt_Stream streams[4];
        size_t streamCount = 0;
        streams[streamCount++] = (struct meshopt_Stream){ cornerPos, sizeof(float3), sizeof(float3) };
        if (hasUV)     streams[streamCount++] = (struct meshopt_Stream){ cornerUV,  sizeof(float2), sizeof(float2) };
        if (hasNormal) streams[streamCount++] = (struct meshopt_Stream){ cornerNrm, sizeof(float3), sizeof(float3) };
        if (hasColor)  streams[streamCount++] = (struct meshopt_Stream){ cornerColor, sizeof(v128f), sizeof(v128f) };

        u32* remap = (u32*)ArenaPushGlobal((u64)numCorners * sizeof(u32));
        size_t uniqueCount = meshopt_generateVertexRemapMulti(remap, NULL, (size_t)numCorners, (size_t)numCorners, streams, streamCount);

        // Final attribute arrays live in the bundle allocator so they survive until bake.
        // One extra position slot pads wide v128f loads in BoundsForPrimitive().
        float3* finalPos = (float3*)FixedPow2Allocator_Allocate(allocator, (uniqueCount + 1) * sizeof(float3));
        meshopt_remapVertexBuffer(finalPos, cornerPos, (size_t)numCorners, sizeof(float3), remap);
        primitive->vertexAttribs[AAttribIdx_POSITION] = finalPos;
        primitive->attributes = AAttribType_POSITION;

        if (hasUV)
        {
            float2* finalUV = (float2*)FixedPow2Allocator_Allocate(allocator, uniqueCount * sizeof(float2));
            meshopt_remapVertexBuffer(finalUV, cornerUV, (size_t)numCorners, sizeof(float2), remap);
            primitive->vertexAttribs[AAttribIdx_TEXCOORD_0] = finalUV;
            primitive->attributes |= AAttribType_TEXCOORD_0;
        }
        if (hasNormal)
        {
            float3* finalNrm = (float3*)FixedPow2Allocator_Allocate(allocator, uniqueCount * sizeof(float3));
            meshopt_remapVertexBuffer(finalNrm, cornerNrm, (size_t)numCorners, sizeof(float3), remap);
            primitive->vertexAttribs[AAttribIdx_NORMAL] = finalNrm;
            primitive->attributes |= AAttribType_NORMAL;
        }
        if (hasColor)
        {
            v128f* finalColor = (v128f*)FixedPow2Allocator_Allocate(allocator, uniqueCount * sizeof(v128f));
            meshopt_remapVertexBuffer(finalColor, cornerColor, (size_t)numCorners, sizeof(v128f), remap);
            primitive->vertexAttribs[AAttribIdx_COLOR_0] = finalColor;
            primitive->attributes |= AAttribType_COLOR_0;
            primitive->colorType = AComponentType_FLOAT;
            primitive->colorCount = 4;
            primitive->colorStride = sizeof(v128f);
        }

        u32* finalIdx = (u32*)FixedPow2Allocator_Allocate(allocator, (u64)numCorners * sizeof(u32));
        meshopt_remapIndexBuffer(finalIdx, NULL, (size_t)numCorners, remap);

        primitive->numVertices = (s32)uniqueCount;
        primitive->numIndices  = numCorners;
        primitive->indices     = finalIdx;

        ArenaPopGlobal((u64)numCorners * sizeof(u32));
        if (hasColor)  ArenaPopGlobal((u64)cornerCapacity * sizeof(v128f));
        if (hasNormal) ArenaPopGlobal((u64)cornerCapacity * sizeof(float3));
        if (hasUV)     ArenaPopGlobal((u64)cornerCapacity * sizeof(float2));
        ArenaPopGlobal((u64)cornerCapacity * sizeof(float3));

        totalIndices  += primitive->numIndices;
        totalVertices += primitive->numVertices;
    }

    fbxScene->totalIndices  = totalIndices;
    fbxScene->totalVertices = totalVertices;

    if (uscene->skin_deformers.count > 0)
        AX_WARN("%s skin deformers ignored (static import only): %s", importType, path);

    // Build the image list from the textures themselves: ufbx does not always populate
    // scene->texture_files, but every FILE texture carries its own path/content. Resolve and
    // dedup by path so repeated references share one basis-compressed output.
    char fbxBaseDir[1024];
    GetBaseDir(path, fbxBaseDir);
    s32 fbxBaseLen = StringLengthSafe(fbxBaseDir, sizeof(fbxBaseDir));

    u16 numTextures = (u16)uscene->textures.count;
    fbxScene->numTextures = numTextures;
    fbxScene->numSamplers = numTextures;

    AImage* images = ArrayCreatePrealloc(AImage, numTextures ? numTextures : 1);

    if (numTextures)
    {
        fbxScene->textures = (ATexture*)AllocZeroTLSFGlobal(numTextures, sizeof(ATexture));
        fbxScene->samplers = (ASampler*)AllocZeroTLSFGlobal(numTextures, sizeof(ASampler));
    }

    for (u16 i = 0; i < numTextures; i++)
    {
        ufbx_texture* utexture = uscene->textures.data[i];
        ATexture* atexture = &fbxScene->textures[i];

        atexture->name    = GetNameFromFBX(utexture->name, allocator);
        atexture->sampler = i;
        atexture->source  = 0;
        fbxScene->samplers[i].wrapS = utexture->wrap_u;
        fbxScene->samplers[i].wrapT = utexture->wrap_v;

        // Only FILE textures map to an image source.
        if (utexture->type != UFBX_TEXTURE_FILE || (utexture->filename.length == 0 && utexture->content.size == 0))
            continue;

        char* resolved = ResolveFBXImagePath(fbxBaseDir, fbxBaseLen, utexture->filename,
                                             utexture->absolute_filename, utexture->relative_filename,
                                             utexture->content, i, allocator);
        if (resolved[0] == '\0')
            continue;

        // Reuse an existing image entry when the same file is referenced again.
        s32 found = -1;
        s32 resolvedLen = StringLength(resolved);
        for (s32 k = 0; k < (s32)ArrayLength(images); k++)
            if (images[k].path && StringLength(images[k].path) == resolvedLen &&
                StringEqual(images[k].path, resolved, resolvedLen))
            {
                found = k;
                break;
            }

        if (found >= 0)
            atexture->source = found;
        else
        {
            atexture->source = (s32)ArrayLength(images);
            ArrayPush(images, (AImage){ resolved });
        }
    }
    
    u16 numMaterials = (u16)uscene->materials.count;
    fbxScene->numMaterials = numMaterials;
    
    // Zero-init so unset AMaterial fields get clean defaults. alphaMode/alphaCutoff are
    // consumed by PackMaterialFlags and the opaque/transparent render-set split; leaving them
    // as garbage pulls meshes into the blended pass or alpha-clips them away (renders black).
    if (numMaterials) {
        fbxScene->materials = AllocZeroTLSFGlobal(numMaterials, sizeof(AMaterial));
    }

    for (u16 i = 0; i < numMaterials; i++)
    {
        ufbx_material* umaterial = uscene->materials.data[i];
        AMaterial* amaterial = fbxScene->materials + i;
        
        ufbx_texture* normalTexture = NULL;
        // search for normal texture
        if (umaterial->features.pbr.enabled) 
            normalTexture = umaterial->pbr.normal_map.texture;
        
        if (!normalTexture && umaterial->fbx.normal_map.has_value)
            normalTexture = umaterial->fbx.normal_map.texture;
        
        // index defaults to UINT16_MAX (no texture); 0 would alias bundle texture slot 0.
        amaterial->textures[0].index = normalTexture ? (u16)normalTexture->typed_id : UINT16_MAX;
        
        amaterial->textures[1].index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_AMBIENT_OCCLUSION,
                                                                        UFBX_MATERIAL_PBR_AMBIENT_OCCLUSION,
                                                                        UFBX_MATERIAL_FBX_AMBIENT_COLOR);
        
        amaterial->textures[2].index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_EMISSION,
                                                                        UFBX_MATERIAL_PBR_EMISSION_COLOR,
                                                                        UFBX_MATERIAL_FBX_EMISSION_COLOR);
        
        amaterial->baseColorTexture.index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_PBR,
                                                                             UFBX_MATERIAL_PBR_BASE_COLOR,
                                                                             UFBX_MATERIAL_FBX_DIFFUSE_COLOR);
        if (amaterial->baseColorTexture.index == UINT16_MAX)
            amaterial->baseColorTexture.index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_DIFFUSE,
                                                                                 UFBX_MATERIAL_PBR_BASE_COLOR,
                                                                                 UFBX_MATERIAL_FBX_DIFFUSE_COLOR);
        
        amaterial->specularTexture.index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_SPECULAR,
                                                                            UFBX_MATERIAL_PBR_SPECULAR_COLOR,
                                                                            UFBX_MATERIAL_FBX_SPECULAR_COLOR);
        
        
        amaterial->metallicRoughnessTexture.index = GetFBXTexture(umaterial, uscene, UFBX_MATERIAL_FEATURE_DIFFUSE_ROUGHNESS,
                                                                                    UFBX_MATERIAL_PBR_ROUGHNESS,
                                                                                    UFBX_MATERIAL_FBX_VECTOR_DISPLACEMENT_FACTOR);
        
        f32 opacity = GetUFBXOpacity(umaterial);
        // metallicFactor/roughnessFactor are unorm16 (value * UINT16_MAX), like the GLTF path.
        // MakeFloat16 (value * 400) would land roughness=1.0 at 400/65535 ~= 0.006 (a mirror),
        // which with only a sun and no IBL renders nearly black. Default to dielectric/matte
        // (0 / 1) when the material does not define them, matching the engine's reset material.
        f32 metalness = umaterial->pbr.metalness.has_value ? (f32)umaterial->pbr.metalness.value_real : 0.0f;
        f32 roughness = umaterial->pbr.roughness.has_value ? (f32)umaterial->pbr.roughness.value_real : 1.0f;
        amaterial->metallicFactor   = PackUnorm16(Saturatef32(metalness));
        amaterial->roughnessFactor  = PackUnorm16(Saturatef32(roughness));
        amaterial->baseColorFactor  = PackUFBXBaseColorFactor(umaterial, opacity);
        amaterial->alphaCutoff      = 0.5f;
        amaterial->alphaMode        = opacity < 0.999f ? AMaterialAlphaMode_Blend : AMaterialAlphaMode_Opaque;
        
        amaterial->specularFactor   = umaterial->features.pbr.enabled ? MakeFloat16(umaterial->pbr.specular_factor.value_real)
                                                                     : MakeFloat16(umaterial->fbx.specular_factor.value_real);
        amaterial->diffuseColor     = PackColor3PtrToUint(&umaterial->fbx.diffuse_color.value_real);
        amaterial->specularColor    = PackColor3PtrToUint(&umaterial->fbx.specular_color.value_real);
        
        amaterial->doubleSided = umaterial->features.double_sided.enabled;
        
        if (umaterial->pbr.emission_factor.value_components == 1)
        {
            amaterial->emissiveFactor[0] = amaterial->emissiveFactor[1] = amaterial->emissiveFactor[2] 
                 = MakeFloat16(umaterial->pbr.emission_factor.value_real);
        }
        else if (umaterial->pbr.emission_factor.value_components > 2)
        {
            amaterial->emissiveFactor[0] = MakeFloat16(umaterial->pbr.emission_factor.value_vec3.x);
            amaterial->emissiveFactor[1] = MakeFloat16(umaterial->pbr.emission_factor.value_vec3.y);
            amaterial->emissiveFactor[2] = MakeFloat16(umaterial->pbr.emission_factor.value_vec3.z);
        }
    }
    
    // Copy local node transforms; SceneBundle_Normalize() will flatten hierarchy and build parent indices.
    u16 numNodes = (u16)uscene->nodes.count;
    fbxScene->numNodes = numNodes;
    
    if (numNodes)
        fbxScene->nodes = (ANode*)AllocZeroTLSFGlobal(numNodes, sizeof(ANode));

    fbxScene->rootNode = uscene->root_node ? (s32)uscene->root_node->typed_id : 0;
    if (fbxScene->rootNode < 0) fbxScene->rootNode = 0;
    fbxScene->scenes[0].numNodes = 1;
    fbxScene->scenes[0].nodes = FixedPow2Allocator_AllocateUninitialized(allocator, sizeof(s32));
    fbxScene->scenes[0].nodes[0] = fbxScene->rootNode;

    for (s32 i = 0; i < numNodes; i++)
    {
        ANode* anode = &fbxScene->nodes[i];
        ufbx_node* unode = uscene->nodes.data[i];
        anode->type = unode->camera ? 1 : 0;
        anode->index = -1;
        anode->parent = -1;
        anode->name = GetNameFromFBX(unode->name, allocator);
        anode->numChildren = (s32)unode->children.count;
        if (anode->numChildren > 0)
            anode->children = FixedPow2Allocator_AllocateUninitialized(allocator, sizeof(s32) * anode->numChildren);
        
        for (s32 j = 0; j < anode->numChildren; j++)
        {
            anode->children[j] = (s32)unode->children.data[j]->typed_id;
        }
        
        SmallMemCpy(anode->translation, &unode->local_transform.translation.x, sizeof(float3));
        SmallMemCpy(anode->rotation, &unode->local_transform.rotation.x, sizeof(v128f));
        SmallMemCpy(anode->scale, &unode->local_transform.scale.x, sizeof(float3));
        
        if (unode->mesh)
        {
            anode->index = (s32)unode->mesh->typed_id;
            if (anode->index >= 0 && unode->materials.count > 0)
                fbxScene->meshes[anode->index].primitives[0].material = (u16)unode->materials.data[0]->typed_id;
        }
        else if (unode->camera)
            anode->index = (s32)unode->camera->typed_id;
    }
    
    fbxScene->numImages = ArrayLength(images);
    fbxScene->images    = images;
    fbxScene->allocator = allocator;
    fbxScene->scale = scale;
    fbxScene->error = AError_NONE;
    SceneBundle_Normalize(fbxScene);
    AX_LOG("%s import complete: %s", importType, path);
    ufbx_free_scene(uscene);
#endif // android
    return 1;
}

s32 LoadOBJ(const char* path, SceneBundle* objScene, f32 scale)
{
#if !AX_GAME_BUILD
    return LoadFBX(path, objScene, scale);
#else
    (void)objScene;
    (void)scale;
    AX_WARN("obj import unavailable in game build: %s", path);
    return 0;
#endif
}

// Parse a source mesh file into the intermediate SceneBundle, dispatching by extension.
// FBX/OBJ import is editor-only (ufbx is excluded from game builds); shipped assets are pre-baked .abm.
s32 ImportSceneBundle(const char* path, SceneBundle* scene, f32 scale)
{
    s32 pathLen = StringLength(path);
    if (FileHasExtension(path, pathLen, ".obj"))
    {
#if !AX_GAME_BUILD
        return LoadOBJ(path, scene, scale);
#else
        AX_WARN("obj import unavailable in game build: %s", path);
        return 0;
#endif
    }
    if (FileHasExtension(path, pathLen, ".fbx"))
    {
#if !AX_GAME_BUILD
        return LoadFBX(path, scene, scale);
#else
        AX_WARN("fbx import unavailable in game build: %s", path);
        return 0;
#endif
    }
    return ParseGLTF(path, scene, scale);
}

void SaveSceneImages(SceneBundle* scene, const char* savePath, bool deleteRemaining)
{
    char pathBuf[2048], baseDir[2048], name[512], bdcPath[2048], tmpPath[2048], srcPath[2048];

    // .bdc path
    s32 len = StringLengthSafe(savePath, sizeof(bdcPath));
    SmallMemCpy(bdcPath, savePath, len);
    ChangeExtension(bdcPath, len, "bdc");
    s32 bdcLen = StringLengthSafe(bdcPath, sizeof(bdcPath));
    SmallMemCpy(tmpPath, bdcPath, bdcLen);
    SmallMemCpy(tmpPath + bdcLen, ".tmp", 5);

    AFile file = AFileOpen(tmpPath, AOpenFlag_WriteText);
    GetBaseDir(savePath, baseDir);

    // write count
    s32 n = IntToString(pathBuf, scene->numImages, 0);
    pathBuf[n++] = '\n';
    AFileWrite(pathBuf, n, file, 1);

    basis_encoder_init();

    for (s32 i = 0; i < scene->numImages; i++)
    {
        const char* src = scene->images[i].path;
        if (src == NULL || src[0] == '\0')
        {
            AX_WARN("scene image has no path, writing empty cache entry index:%d", i);
            pathBuf[0] = '\n';
            AFileWrite(pathBuf, 1, file, 1);
            pathBuf[0] = '0';
            pathBuf[1] = '\n';
            AFileWrite(pathBuf, 2, file, 1);
            continue;
        }

        s32 srcLen = (s32)StringLengthSafe(src, sizeof(srcPath) - 1);
        SmallMemCpy(srcPath, src, srcLen);
        srcPath[srcLen] = '\0';

        // write original path
        s32 l = (s32)StringLengthSafe(srcPath, sizeof(pathBuf) - 2);
        SmallMemCpy(pathBuf, srcPath, l);
        pathBuf[l++] = '\n';
        AFileWrite(pathBuf, l, file, 1);

        // build output path = baseDir + name + ".basis"
        s32 baseLen = (s32)StringLengthSafe(baseDir, sizeof(baseDir));
        SmallMemCpy(pathBuf, baseDir, baseLen);

        s32 nameLen = GetFileNameNoExt(srcPath, name);
        SmallMemCpy(pathBuf + baseLen, name, nameLen);
        SmallMemCpy(pathBuf + baseLen + nameLen, ".basis", 7);

        bool isNormal = false;
        bool isMR     = false; // metallic roughness

        for (s32 j = 0; j < scene->numMaterials; j++)
        {
            AMaterial m = scene->materials[j];

            if (m.textures[0].index < scene->numTextures)
                isNormal |= (scene->textures[m.textures[0].index].source == i);

            if (m.metallicRoughnessTexture.index < scene->numTextures)
                isMR |= (scene->textures[m.metallicRoughnessTexture.index].source == i);

            if (m.specularTexture.index < scene->numTextures)
                isMR |= (scene->textures[m.specularTexture.index].source == i);
        }

        // baseColor wins -> it's neither normal nor MR
        for (s32 j = 0; j < scene->numMaterials; j++)
        {
            AMaterial m = scene->materials[j];

            if (m.baseColorTexture.index < scene->numTextures &&
                scene->textures[m.baseColorTexture.index].source == i)
            {
                isNormal = false;
                isMR     = false;
                break;
            }
        }

        s32 type = (isNormal ? 1 : 0) | (isMR ? 2 : 0);
        AX_LOG("output path: %s", pathBuf);

        const s32 quality = 0; // 100 is max
        s32 r = basis_compress_file((const char*)srcPath, (const char*)pathBuf, type, 16, quality, -1);
        if (r) AX_WARN("Failed: %s (%d)\n", srcPath, r);

        n = IntToString(pathBuf, type, 0);
        pathBuf[n++] = '\n';
        AFileWrite(pathBuf, n, file, 1);

        if (deleteRemaining) RemoveFile(srcPath);
    }

    AFileClose(file);
    RemoveFile(bdcPath);
    RenameFile(tmpPath, bdcPath);
}

typedef struct ScaneImgCompTaskData_
{
    SceneBundle* scene; 
    char* savePath; 
    bool deleteRemaining;
} ScaneImgCompTaskData;

static s32 SaveSceneImagesTask(void* userData)
{
    ScaneImgCompTaskData* taskData = (ScaneImgCompTaskData*)userData;
    SaveSceneImages(taskData->scene, taskData->savePath, taskData->deleteRemaining);
    SDL_free(taskData->savePath);
    SDL_free(taskData);
    return 1;
}

// unusded for now 
void SaveSceneImagesAsync(SceneBundle* scene, const char* path, bool deleteRemaining, AsyncCallback callback)
{
    if (!scene || !path || path[0] == '\0')
    {
        AX_WARN("Texture compress task invalid arguments");
        return;
    }

    ScaneImgCompTaskData* taskData = (ScaneImgCompTaskData*)SDL_malloc(sizeof(ScaneImgCompTaskData));
    if (!taskData)
    {
        AX_WARN("Texture compress task allocation failed");
        return;
    }

    int newLen = StringLength(path);
    char* savePath = (char*)SDL_malloc(newLen + 1);
    if (!savePath)
    {
        SDL_free(taskData);
        AX_WARN("Texture compress path allocation failed");
        return;
    }
    MemCopy(savePath, path, newLen + 1);
    *taskData = (ScaneImgCompTaskData){
        .scene = scene,
        .savePath = savePath,
        .deleteRemaining = deleteRemaining
    };
	AsyncRun("SaveSceneImages", SaveSceneImagesTask, callback, taskData);
}

// result: 1 fine, 2 some file is not exist, 3 not enough images for scene
s32 LoadSceneImages(const char* texturePath, Texture* textures, s32 numImages)
{
    if (numImages <= 0)
        return 1;

    AFile file = AFileOpen(texturePath, AOpenFlag_ReadBinary);
    if (!AFileExist(file))
        return 0;

    char buffer[2048];
    char typeBuffer[64];
    char baseDir[2048];
    char fileName[512];
    char basisPath[2048];
    GetBaseDir(texturePath, baseDir);

    s32 result = 1;
    s32 fileNumImages = AFileReadI32(buffer, sizeof(buffer), file); // First line: numImages
    if (fileNumImages != numImages)
    {
        AX_WARN("basis file num images not equal to requested numImages. file: %d, requested: %d", fileNumImages, numImages);
        numImages = fileNumImages;
        result = 3;
    }

    for (s32 i = 0; i < numImages; i++)
    {
        s32 pathLen = AFileReadLine(buffer, sizeof(buffer), file);
        s32 textureType = AFileReadI32(typeBuffer, sizeof(typeBuffer), file);

        if (pathLen <= 0)
        {
            AX_WARN("basis metadata ended early index:%d, file:%s", i, texturePath);
            textures[i] = rCreateTexture(32, 32, buffer, TEX_FMT_8UNORM1, 0, TEX_SAMPLER, "BasisNoMetadata");
            result = 3;
            continue;
        }

        GetFileNameNoExt(buffer, fileName);
        s32 fileNameLen = (s32)StringLengthSafe(fileName, sizeof(fileName));
        textures[i].type = (u32)textureType;
        textures[i].channels = (textureType & 3u) ? 2u : 4u;
        if (fileNameLen <= 0)
        {
            AX_WARN("basis metadata has empty image path index:%d, file:%s", i, texturePath);
            textures[i] = rCreateTexture(32, 32, buffer, TEX_FMT_8UNORM1, 0, TEX_SAMPLER, "BasisNoMetadata");
            result = 3;
            continue;
        }
        s32 baseLen = (s32)StringLengthSafe(baseDir, sizeof(baseDir));
        s32 nameLen = (s32)StringLengthSafe(fileName, sizeof(fileName));
        if (baseLen + nameLen + 7 > (s32)sizeof(basisPath))
        {
            AX_WARN("basis path too long index:%d, file:%s", i, texturePath);
            textures[i] = rCreateTexture(32, 32, buffer, TEX_FMT_8UNORM1, 0, TEX_SAMPLER, "BasisNoMetadata");
            result = 2;
            continue;
        }

        SmallMemCpy(basisPath, baseDir, baseLen);
        SmallMemCpy(basisPath + baseLen, fileName, nameLen);
        SmallMemCpy(basisPath + baseLen + nameLen, ".basis", 7);

        u64 size = FileSize(basisPath);
        void* mem = SDL_malloc(size);
        
        if (!mem || size == 0)
        {
            const char* reason = size == 0 ? "BasisFileNotExist" : "OSAllocFailed";
            textures[i] = rCreateTexture(32, 32, buffer,TEX_FMT_8UNORM1, 0, TEX_SAMPLER, "BasisNotFound");
            AX_WARN("%s index:%d, fileSize:%llu, path:%s", reason, i, size, basisPath);
            result = 2;
            continue;
        }

        void* basisData = ReadAllFile(basisPath, mem, size);
        s32 isNormal =  textureType & 1;
        s32 isMetallicRoughness = (textureType >> 1) & 1;

        textures[i].buffer = basisData;
        textures[i].bufferSize = size;
        textures[i].handle = BasisuMakeImage(basisData, size, &textures[i].width, &textures[i].height, &textures[i].format, &textures[i].mipLevels,
                                             (u8)isNormal, (u8)isMetallicRoughness);

        if (!textures[i].handle)
        {
            AX_WARN("basis gpu texture creation failed index:%d size:%llu dimensions:%dx%d path:%s", i, size, textures[i].width, textures[i].height, basisPath);
            SDL_free(mem);
            textures[i] = rCreateTexture(32, 32, buffer, TEX_FMT_8UNORM1, 0, TEX_SAMPLER, "BasisNotFound");
            result = 2;
        }
    }
    AFileClose(file);
    return result;
}

s32 LoadGLTFCached(const char* path, SceneBundle* scene, Texture* textures, void** outVertexHeapPtr, void** outIndexHeapPtr)
{
    char buffer[1024];
    size_t pathLen = StringLength(path);
    MemCopy(buffer, path, pathLen + 1);
    int newLen = ChangeExtension(buffer, pathLen, "glb");
    bool deleteRemaining = FileExist(buffer);
    newLen = ChangeExtension(buffer, pathLen, "abm");
    s32 result = 1;
    if (IsABMLastVersion(buffer)) {
        AX_LOG("asset cache hit: %s", buffer);
        result = LoadSceneBundleBinary(buffer, scene, outVertexHeapPtr, outIndexHeapPtr);
    }
    else if (ImportSceneBundle(path, scene, 1.0f)) {
        AX_LOG("asset cache rebuild: %s -> %s", path, buffer);
        if (!BakeSceneMeshesAndAnimations(scene, outVertexHeapPtr, outIndexHeapPtr))
        {
            AX_WARN("asset import failed during mesh bake: %s vertices=%d indices=%d", path, scene->totalVertices, scene->totalIndices);
            return 0;
        }
        if (!SaveGLTFBinary(scene, buffer))
        {
            AX_WARN("asset cache save failed: %s", buffer);
            return 0;
        }
        ChangeExtension(buffer, newLen, "bdc");
        if (!FileExist(buffer))
        {
            SaveSceneImages(scene, buffer, deleteRemaining);
        }
    }
    else {
        AX_WARN("asset import failed: %s", path);
        return 0;
    }
    ChangeExtension(buffer, newLen, "bdc");
    s32 imageResult = LoadSceneImages(buffer, textures, scene->numImages);
    if (imageResult == 0 || imageResult == 3)
    {
        AX_WARN("scene image cache invalid, rebuilding: %s result=%d", buffer, imageResult);
        SaveSceneImages(scene, buffer, deleteRemaining);
        imageResult = LoadSceneImages(buffer, textures, scene->numImages);
    }
    else if (imageResult == 2)
    {
        AX_WARN("scene image cache has missing basis files, keeping metadata: %s", buffer);
    }
    return result && (imageResult != 0);
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                            Binary Save                                   */
/*//////////////////////////////////////////////////////////////////////////*/

// ZSTD_CCtx* zstdCompressorCTX = NULL;
// 79: geometry offsets and index values are stored bundle relative, the
// runtime placement comes from the mega buffer range allocators
// 80: surface (static) AVertex.position quantized to xyz unorm16 vs the primitive AABB
const s32 ABMMeshVersion = 80;

u8 IsABMLastVersion(const char* path)
{
    if (!FileExist(path))
    {
        AX_LOG("abm cache miss: %s does not exist", path);
        return false;
    }

    AFile file = AFileOpen(path, AOpenFlag_ReadBinary);
    if (AFileSize(file) < sizeof(s32) + sizeof(u64))
    {
        AX_WARN("abm cache invalid: %s is too small", path);
        AFileClose(file);
        return false;
    }

    s32 version = 0;
    AFileRead(&version, sizeof(s32), file, 1);
    u64 hex;
    AFileRead(&hex, sizeof(u64), file, 1);
    AFileClose(file);
    if (hex != 0xABFABF)
        AX_WARN("abm cache invalid: %s has wrong magic", path);
    else if (version != ABMMeshVersion)
        AX_LOG("abm cache stale: %s version=%d expected=%d", path, version, ABMMeshVersion);
    return version == ABMMeshVersion && hex == 0xABFABF;
}

static void WriteAMaterialTexture(GLTFTexture texture, AFile file)
{
    u64 data = texture.scale; data <<= sizeof(uint16_t) * 8;
    data |= texture.strength; data <<= sizeof(uint16_t) * 8;
    data |= texture.index;    data <<= sizeof(uint16_t) * 8;
    data |= texture.texCoord;
    
    AFileWrite(&data, sizeof(u64), file, 1);
}

static void WriteGLTFString(const char* str, AFile file)
{
    if (str == NULL || str == (char*)0xCDCDCDCDCDCDCDCDull)
    {
        s32 nameLen = sizeof("no_name_mat");
        AFileWrite(&nameLen, sizeof(s32), file, 1);
        AFileWrite("no_name_mat", (uint64_t)(nameLen + 1), file, 1);
        return;
    }
    s32 nameLen = str ? StringLength(str) : 0;
    AFileWrite(&nameLen, sizeof(s32), file, 1);
    AFileWrite(str, (uint64_t)(nameLen + 1), file, 1);
}

s32 SaveGLTFBinary(const SceneBundle* gltf, const char* path)
{
#if !AX_GAME_BUILD
    AX_LOG("saving abm: %s version=%d meshes=%d nodes=%d skins=%d animations=%d",
           path, ABMMeshVersion, gltf->numMeshes, gltf->numNodes, gltf->numSkins, gltf->numAnimations);
    if (gltf->allVertices == NULL || gltf->allIndices == NULL || gltf->totalVertices <= 0 || gltf->totalIndices <= 0)
    {
        AX_WARN("abm save skipped: scene is not baked path=%s vertices=%d indices=%d", path, gltf->totalVertices, gltf->totalIndices);
        return 0;
    }
    EnsurePath(path);
    AFile file = AFileOpen(path, AOpenFlag_WriteBinary);
    s32 version = ABMMeshVersion;
    AFileWrite(&version, sizeof(s32), file, 1);
    
    u64 reserved[4] = { 0xABFABF };
    AFileWrite(&reserved, sizeof(u64) * 4, file, 1);
    
    AFileWrite(&gltf->scale, sizeof(float), file, 1);
    AFileWrite(&gltf->numMeshes, sizeof(u16), file, 1);
    AFileWrite(&gltf->numNodes, sizeof(u16), file, 1);
    AFileWrite(&gltf->numMaterials,  sizeof(u16), file, 1);
    AFileWrite(&gltf->numTextures, sizeof(u16), file, 1);
    AFileWrite(&gltf->numImages, sizeof(u16), file, 1);
    AFileWrite(&gltf->numSamplers, sizeof(u16), file, 1);
    AFileWrite(&gltf->numCameras, sizeof(u16), file, 1);
    AFileWrite(&gltf->numScenes, sizeof(u16), file, 1);
    AFileWrite(&gltf->numSkins, sizeof(u16), file, 1);
    AFileWrite(&gltf->numAnimations, sizeof(u16), file, 1);
    AFileWrite(&gltf->defaultSceneIndex, sizeof(u16), file, 1);
    u16 isSkined = (u16)(gltf->numSkins > 0);
    AFileWrite(&isSkined, sizeof(u16), file, 1);
    
    AFileWrite(&gltf->totalIndices, sizeof(s32), file, 1);
    AFileWrite(&gltf->totalVertices, sizeof(s32), file, 1);
    
    u64 vertexSize    = isSkined ? sizeof(ASkinedVertex) : sizeof(AVertex);
    u64 allVertexSize = vertexSize * (u64)gltf->totalVertices;
    u64 allIndexSize  = (u64)gltf->totalIndices * sizeof(u32);

    // offsets and index values are saved relative to the bundle's placement so
    // the cache can be loaded into any allocated range
    u32 bakedVertexBase = isSkined ? (u32)((const ASkinedVertex*)gltf->allVertices - gGFX.SkinnedVertexBuffer)
                                   : (u32)((const AVertex*)gltf->allVertices - gGFX.SurfaceVertexBuffer);
    u32 bakedIndexBase  = (u32)((const u32*)gltf->allIndices - gGFX.IndexBuffer);

    // This runs on a background thread (see the async cache-save task), so it must only READ the
    // resident geometry. Delta-encode indices into a temp buffer instead of mutating the live index
    // buffer, and use a per-call deflate state (the shared static one is not thread safe).
    // layout: [deflate output (max(vtx,idx)) | delta-encoded indices (idx)]
    u64 deflateSlotSize = Maxu64(allVertexSize, allIndexSize);
    char* compressedBuffer = (char*)AllocZeroTLSFGlobal(1ull, deflateSlotSize + allIndexSize + 64);
    struct sdefl* sdfl = (struct sdefl*)AllocZeroTLSFGlobal(1ull, sizeof(struct sdefl));

    if (!compressedBuffer || !sdfl)
    {
        AX_WARN("abm save alloc failed: %s", path);
        if (compressedBuffer) DeAllocateTLSFGlobal(compressedBuffer);
        if (sdfl) DeAllocateTLSFGlobal(sdfl);
        AFileClose(file);
        return 0;
    }

    char* deflateOutput = compressedBuffer;
    u32*  deltaIndices  = (u32*)(compressedBuffer + deflateSlotSize);

    u64 afterCompSize = zsdeflate(sdfl, deflateOutput, gltf->allVertices, allVertexSize, 5);
    AFileWrite(&afterCompSize, sizeof(u64), file, 1);
    AFileWrite(deflateOutput, afterCompSize, file, 1);

    DeltaEncodingU32((const u32*)gltf->allIndices, gltf->totalIndices, deltaIndices, bakedVertexBase);
    afterCompSize = zsdeflate(sdfl, deflateOutput, deltaIndices, allIndexSize, 5);
    AFileWrite(&afterCompSize, sizeof(u64), file, 1);
    AFileWrite(deflateOutput, afterCompSize, file, 1);

    DeAllocateTLSFGlobal(sdfl);
    DeAllocateTLSFGlobal(compressedBuffer);
    // Cache stores runtime-ready mesh/animation data. Morph target animation is intentionally not serialized yet.

    for (s32 i = 0; i < gltf->numMeshes; i++)
    {
        AMesh mesh = gltf->meshes[i];
        WriteGLTFString(mesh.name, file);
        
        AFileWrite(&mesh.numPrimitives  , sizeof(s32), file, 1);
        
        for (s32 j = 0; j < mesh.numPrimitives; j++)
        {
            APrimitive* primitive = &mesh.primitives[j];
            // all 4 lod slots rebase uniformly, slot 3 holds fallback values
            s32 relIndexOffset = primitive->indexOffset - (s32)bakedIndexBase;
            s32 relLodIndexOffset[4], relLodVertexOffset[4];
            for (s32 lod = 0; lod < 4; lod++)
            {
                relLodIndexOffset[lod]  = primitive->lodIndexOffset[lod] - (s32)bakedIndexBase;
                relLodVertexOffset[lod] = primitive->lodVertexOffset[lod] - (s32)bakedVertexBase;
            }

            AFileWrite(&primitive->attributes , sizeof(s32), file, 1);
            AFileWrite(&primitive->indexType  , sizeof(s32), file, 1);
            AFileWrite(&primitive->numIndices , sizeof(s32), file, 1);
            AFileWrite(&primitive->numVertices, sizeof(s32), file, 1);
            AFileWrite(&relIndexOffset, sizeof(s32), file, 1);
            AFileWrite(relLodIndexOffset, sizeof(s32) * 4, file, 1);
            AFileWrite(primitive->lodNumIndices, sizeof(s32) * 4, file, 1);
            AFileWrite(relLodVertexOffset, sizeof(s32) * 4, file, 1);
            AFileWrite(primitive->lodNumVertices, sizeof(s32) * 4, file, 1);
            AFileWrite(primitive->lodAnimatedVertexOffset, sizeof(s32) * 4, file, 1); // already bundle local
            AFileWrite(&primitive->jointType  , sizeof(u16), file, 1);
            AFileWrite(&primitive->jointCount , sizeof(u16), file, 1);
            AFileWrite(&primitive->jointStride, sizeof(u16), file, 1);
            AFileWrite(&primitive->material   , sizeof(u16), file, 1);
            AFileWrite(primitive->min, sizeof(v128f), file, 1);
            AFileWrite(primitive->max, sizeof(v128f), file, 1);
        }
    }
    
    for (s32 i = 0; i < gltf->numNodes; i++)
    {
        ANode* node = &gltf->nodes[i];
        AFileWrite(&node->type       , sizeof(s32), file, 1);
        AFileWrite(&node->index      , sizeof(s32), file, 1);
        AFileWrite(&node->translation, sizeof(float) * 3, file, 1);
        AFileWrite(&node->rotation   , sizeof(float) * 4, file, 1);
        AFileWrite(&node->scale      , sizeof(float) * 3, file, 1);
        AFileWrite(&node->numChildren, sizeof(s32), file, 1);
        AFileWrite(&node->parent     , sizeof(s32), file, 1);
        
        if (node->numChildren)
            AFileWrite(node->children, sizeof(s32) * node->numChildren, file, 1);
        
        WriteGLTFString(node->name, file);
    }
    
    for (s32 i = 0; i < gltf->numMaterials; i++)
    {
        AMaterial* material = &gltf->materials[i];
        for (s32 j = 0; j < 3; j++)
        {
            WriteAMaterialTexture(material->textures[j], file);
        }
        
        WriteAMaterialTexture(material->baseColorTexture, file);
        WriteAMaterialTexture(material->specularTexture, file);
        WriteAMaterialTexture(material->metallicRoughnessTexture, file);
        
        u64 data = material->emissiveFactor[0]; data <<= sizeof(u16) * 8;
        data |= material->emissiveFactor[1];         data <<= sizeof(u16) * 8;
        data |= material->emissiveFactor[2];         data <<= sizeof(u16) * 8;
        data |= material->specularFactor;
        AFileWrite(&data, sizeof(u64), file, 1);
        
        data = ((u64)(material->diffuseColor) << 32) | material->specularColor;
        AFileWrite(&data, sizeof(u64), file, 1);
        
        data = ((u64)(material->baseColorFactor) << 32) | (u64)material->doubleSided;
        AFileWrite(&data, sizeof(u64), file, 1);

        u32 packedFactors = ((u32)material->metallicFactor << 16u) | (u32)material->roughnessFactor;
        AFileWrite(&packedFactors, sizeof(u32), file, 1);
        
        AFileWrite(&material->alphaCutoff, sizeof(float), file, 1);
        AFileWrite(&material->alphaMode, sizeof(s32), file, 1);
        
        WriteGLTFString(material->name, file);
    }
    
    for (s32 i = 0; i < gltf->numTextures; i++)
    {
        ATexture texture = gltf->textures[i];
        AFileWrite(&texture.sampler, sizeof(s32), file, 1);
        AFileWrite(&texture.source, sizeof(s32), file, 1);
        WriteGLTFString(texture.name , file);
    }
    
    for (s32 i = 0; i < gltf->numImages; i++)
    {
        WriteGLTFString(gltf->images[i].path, file);
    }
    
    for (s32 i = 0; i < gltf->numSamplers; i++)
    {
    	AFileWrite(&gltf->samplers[i], sizeof(ASampler), file, 1);
    }
    
    for (s32 i = 0; i < gltf->numCameras; i++)
    {
        ACamera camera = gltf->cameras[i];
        AFileWrite(&camera.aspectRatio, sizeof(float), file, 1);
        AFileWrite(&camera.yFov, sizeof(float), file, 1);
        AFileWrite(&camera.zFar, sizeof(float), file, 1);
        AFileWrite(&camera.zNear, sizeof(float), file, 1);
        AFileWrite(&camera.type, sizeof(s32), file, 1);
        WriteGLTFString(camera.name , file);
    }
    for (s32 i = 0; i < gltf->numScenes; i++)
    {
        AScene scene = gltf->scenes[i];
        WriteGLTFString(scene.name, file);
        AFileWrite(&scene.numNodes, sizeof(s32), file, 1);
        AFileWrite(scene.nodes, sizeof(s32) * scene.numNodes, file, 1);
    }
    
    for (s32 i = 0; i < gltf->numSkins; i++)
    {
        ASkin skin = gltf->skins[i];
        AFileWrite(&skin.skeleton, sizeof(s32), file, 1);
        AFileWrite(&skin.numJoints, sizeof(s32), file, 1);
        AFileWrite(skin.inverseBindMatrices, sizeof(mat4x4) * skin.numJoints, file, 1);
        AFileWrite(skin.joints, sizeof(s32) * skin.numJoints, file, 1);
    }
    
    s32 totalAnimSamplerInput = 0;
    if (gltf->numAnimations > 0)
    {
        for (s32 a = 0; a < gltf->numAnimations; a++)
            for (s32 s = 0; s < gltf->animations[a].numSamplers; s++)
                totalAnimSamplerInput += gltf->animations[a].samplers[s].count;
    }

    AFileWrite(&totalAnimSamplerInput, sizeof(s32), file, 1);
    if (totalAnimSamplerInput > 0) {
        // all sampler input and outputs are allocated in one buffer each. at the end of the CreateVerticesIndicesSkined function
        AFileWrite(gltf->animations[0].samplers[0].input, sizeof(float) * totalAnimSamplerInput, file, 1);
        AFileWrite(gltf->animations[0].samplers[0].output, sizeof(v128f) * totalAnimSamplerInput, file, 1);
    }

    for (s32 i = 0; i < gltf->numAnimations; i++)
    {
        AAnimation animation = gltf->animations[i];
        AFileWrite(&animation.numSamplers, sizeof(s32), file, 1);
        AFileWrite(&animation.numChannels, sizeof(s32), file, 1);
        AFileWrite(&animation.duration, sizeof(float), file, 1);
        AFileWrite(&animation.speed, sizeof(float), file, 1);
        WriteGLTFString(animation.name, file);

        AFileWrite(animation.channels, sizeof(AAnimChannel) * animation.numChannels, file, 1);
        
        for (s32 j = 0; j < animation.numSamplers; j++)
        {
            AFileWrite(&animation.samplers[j].count, sizeof(s32), file, 1);
            AFileWrite(&animation.samplers[j].numComponent, sizeof(s32), file, 1);
            AFileWrite(&animation.samplers[j].inputType, sizeof(AComponentType), file, 1);
            AFileWrite(&animation.samplers[j].outputType, sizeof(AComponentType), file, 1);
            AFileWrite(&animation.samplers[j].interpolation, sizeof(ASamplerInterpolation), file, 1);
        }
    }
    
    AFileWrite(&gltf->rootNode, sizeof(int), file, 1);
    AFileClose(file);
    AX_LOG("abm save complete: %s vertices=%d indices=%d", path, gltf->totalVertices, gltf->totalIndices);
#endif
    return 1;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                            Binary Read                                   */
/*//////////////////////////////////////////////////////////////////////////*/

void ReadAMaterialTexture(GLTFTexture* texture, AFile file)
{
    u64 data;
    AFileRead(&data, sizeof(u64), file, 1);
    
    texture->texCoord = data & 0xFFFFu; data >>= sizeof(u16) * 8;
    texture->index    = data & 0xFFFFu; data >>= sizeof(u16) * 8;
    texture->strength = data & 0xFFFFu; data >>= sizeof(u16) * 8;
    texture->scale    = data & 0xFFFFu;
}

void ReadGLTFString(char** str, AFile file, FixedPow2Allocator* stringAllocator)
{
    s32 nameLen = 0;
    AFileRead(&nameLen, sizeof(s32), file, 1);
    if (nameLen)    
    {
        *str = FixedPow2Allocator_AllocateUninitialized(stringAllocator, nameLen + 1);
        AFileRead(*str, nameLen + 1, file, 1);
        (*str)[nameLen] = 0;
    }
}

s32 LoadSceneBundleBinary(const char* path, SceneBundle* gltf, void** outVertexHeapPtr, void** outIndexHeapPtr)
{
    AX_LOG("abm load: %s", path);
    AFile file = AFileOpen(path, AOpenFlag_ReadBinary);
    if (!AFileExist(file))
    {
        AX_WARN("Failed to open file for reading: %s", path);
        return 0;
    }
    
    MemsetZero(gltf, sizeof(SceneBundle));
    FixedPow2Allocator* allocator = AllocateTLSFGlobal(sizeof(FixedPow2Allocator));
    FixedPow2Allocator_Init(allocator, 1024);

    s32 version = 0;
    AFileRead(&version, sizeof(s32), file, 1);
    if (version != ABMMeshVersion)
        AX_WARN("abm version mismatch: %s version=%d expected=%d", path, version, ABMMeshVersion);
    ASSERT(version == ABMMeshVersion);
    
    u64 reserved[4];
    AFileRead(&reserved, sizeof(u64) * 4, file, 1);
    
    AFileRead(&gltf->scale            , sizeof(float), file, 1);
    AFileRead(&gltf->numMeshes        , sizeof(u16), file, 1);
    AFileRead(&gltf->numNodes         , sizeof(u16), file, 1);
    AFileRead(&gltf->numMaterials     , sizeof(u16), file, 1);
    AFileRead(&gltf->numTextures      , sizeof(u16), file, 1);
    AFileRead(&gltf->numImages        , sizeof(u16), file, 1);
    AFileRead(&gltf->numSamplers      , sizeof(u16), file, 1);
    AFileRead(&gltf->numCameras       , sizeof(u16), file, 1);
    AFileRead(&gltf->numScenes        , sizeof(u16), file, 1);
    AFileRead(&gltf->numSkins         , sizeof(u16), file, 1);
    AFileRead(&gltf->numAnimations    , sizeof(u16), file, 1);
    AFileRead(&gltf->defaultSceneIndex, sizeof(u16), file, 1);
    u16 isSkined;
    AFileRead(&isSkined, sizeof(u16), file, 1);
    
    AFileRead(&gltf->totalIndices, sizeof(s32), file, 1);
    AFileRead(&gltf->totalVertices, sizeof(s32), file, 1);
    
    size_t vertexSize = isSkined ? sizeof(ASkinedVertex) : sizeof(AVertex);
    size_t vertexAlignment = 4;

    GeometryBufferKind vertexKind = isSkined ? GeometryBuffer_SkinnedVertex : GeometryBuffer_SurfaceVertex;
    u32 vertexBase = GEOMETRY_ALLOC_FAIL;
    u32 indexBase  = GEOMETRY_ALLOC_FAIL;

    {
        // +1 vertex / +4 index padding, zsinflate may write a few bytes past the
        // exact size and the next range can belong to another live bundle
        void* vertexRaw = NULL;
        void* indexRaw  = NULL;
        vertexBase = GeometryHeapAlloc(vertexKind, (u32)gltf->totalVertices + 1u, &vertexRaw);
        indexBase  = GeometryHeapAlloc(GeometryBuffer_Index, (u32)gltf->totalIndices + 4u, &indexRaw);
        if (vertexBase == GEOMETRY_ALLOC_FAIL || indexBase == GEOMETRY_ALLOC_FAIL)
        {
            AX_ERROR("VERTEX buffer space is not enough for %s", path);
            GeometryHeapFree(vertexKind, vertexRaw);
            GeometryHeapFree(GeometryBuffer_Index, indexRaw);
            return 0;
        }
        if (outVertexHeapPtr) *outVertexHeapPtr = vertexRaw;
        if (outIndexHeapPtr)  *outIndexHeapPtr = indexRaw;

        if (isSkined) gGFX.NumSkinnedVertices += gltf->totalVertices;
        else          gGFX.NumSurfaceVertices += gltf->totalVertices;
        gGFX.NumIndices += gltf->totalIndices;

        gltf->allVertices = isSkined ? (void*)(gGFX.SkinnedVertexBuffer + vertexBase) : (void*)(gGFX.SurfaceVertexBuffer + vertexBase);
        gltf->allIndices  = gGFX.IndexBuffer + indexBase;

        u64 allVertexSize = gltf->totalVertices * vertexSize;
        u64 allIndexSize  = gltf->totalIndices * sizeof(u32);

        u64 deflateSlotSize = Maxu64(allVertexSize, allIndexSize);
        u64 tempSize        = deflateSlotSize;
        char* compressedBuffer = (char*)AllocateTLSFGlobal(tempSize + 16);

        u64 compressedSize;

        AFileRead(&compressedSize, sizeof(u64), file, 1);
        AFileRead(compressedBuffer, compressedSize, file, 1);
        zsinflate(gltf->allVertices, allVertexSize, compressedBuffer, compressedSize);

        AFileRead(&compressedSize, sizeof(u64), file, 1);
        AFileRead(compressedBuffer, compressedSize, file, 1);
        zsinflate(gltf->allIndices, allIndexSize, compressedBuffer, compressedSize);
        // index values were saved relative to the bundle's vertex base, the
        // prefix sum seed rebases the whole stream to the allocated range
        PrefixSumU32Inplace(gltf->allIndices, gltf->totalIndices, vertexBase);

        Rendering_QueueGeometryUpload(isSkined ? GeometryBuffer_SkinnedVertex : GeometryBuffer_SurfaceVertex,
                                      vertexBase, vertexBase + (u32)gltf->totalVertices);
        Rendering_QueueGeometryUpload(GeometryBuffer_Index, indexBase, indexBase + (u32)gltf->totalIndices);
        DeAllocateTLSFGlobal(compressedBuffer);
    }
    
    char* currVertices = (char*)gltf->allVertices;
    char* currIndices = (char*)gltf->allIndices;
    
    if (gltf->numMeshes > 0) gltf->meshes = AllocZeroTLSFGlobal(gltf->numMeshes, sizeof(AMesh));
    s32 totalPrimitives = 0;
    for (s32 i = 0; i < gltf->numMeshes; i++)
    {
        AMesh* mesh = &gltf->meshes[i];
        ReadGLTFString(&mesh->name, file, allocator);
        
        AFileRead(&mesh->numPrimitives, sizeof(s32), file, 1);
        mesh->primitiveOffset = totalPrimitives;
        totalPrimitives += mesh->numPrimitives;

        mesh->primitives = FixedPow2Allocator_AllocateUninitialized(allocator, sizeof(APrimitive) * mesh->numPrimitives);
        
        for (s32 j = 0; j < mesh->numPrimitives; j++)
        {
            APrimitive* primitive = &mesh->primitives[j];
            AFileRead(&primitive->attributes , sizeof(s32), file, 1);
            AFileRead(&primitive->indexType  , sizeof(s32), file, 1);
            AFileRead(&primitive->numIndices , sizeof(s32), file, 1);
            AFileRead(&primitive->numVertices, sizeof(s32), file, 1);
            AFileRead(&primitive->indexOffset, sizeof(s32), file, 1);
            AFileRead(primitive->lodIndexOffset, sizeof(s32) * 4, file, 1);
            AFileRead(primitive->lodNumIndices, sizeof(s32) * 4, file, 1);
            AFileRead(primitive->lodVertexOffset, sizeof(s32) * 4, file, 1);
            AFileRead(primitive->lodNumVertices, sizeof(s32) * 4, file, 1);
            AFileRead(primitive->lodAnimatedVertexOffset, sizeof(s32) * 4, file, 1);

            // stored bundle relative, rebase onto the allocated ranges
            primitive->indexOffset += (s32)indexBase;
            for (s32 lod = 0; lod < 4; lod++)
            {
                primitive->lodIndexOffset[lod]  += (s32)indexBase;
                primitive->lodVertexOffset[lod] += (s32)vertexBase;
            }
            AFileRead(&primitive->jointType  , sizeof(u16), file, 1);
            AFileRead(&primitive->jointCount , sizeof(u16), file, 1);
            AFileRead(&primitive->jointStride, sizeof(u16), file, 1);
            u64 indexSize = (u64)(GraphicsTypeToSize(primitive->indexType)) * primitive->numIndices;
            primitive->indices = (void*)currIndices;
            currIndices += indexSize;
            
            u64 primitiveVertexSize = (u64)(primitive->numVertices) * vertexSize;
            primitive->vertices = currVertices;
            currVertices += primitiveVertexSize;
            AFileRead(&primitive->material, sizeof(u16), file, 1);
            primitive->hasOutline = false; // always false 

            AFileRead(primitive->min, sizeof(v128f), file, 1);
            AFileRead(primitive->max, sizeof(v128f), file, 1);
        }
    }
    gltf->totalPrimitives = totalPrimitives;
    
    if (gltf->numNodes > 0) gltf->nodes = AllocZeroTLSFGlobal(gltf->numNodes, sizeof(ANode));
    
    for (s32 i = 0; i < gltf->numNodes; i++)
    {
        ANode* node = &gltf->nodes[i];
        AFileRead(&node->type       , sizeof(s32), file, 1);
        AFileRead(&node->index      , sizeof(s32), file, 1);
        AFileRead(&node->translation, sizeof(float) * 3, file, 1);
        AFileRead(&node->rotation   , sizeof(float) * 4, file, 1);
        AFileRead(&node->scale      , sizeof(float) * 3, file, 1);
        AFileRead(&node->numChildren, sizeof(s32), file, 1);
        AFileRead(&node->parent     , sizeof(s32), file, 1);
        
        if (node->numChildren)
        {
            node->children = FixedPow2Allocator_AllocateUninitialized(allocator, sizeof(s32) * (node->numChildren+1));
            AFileRead(node->children, sizeof(s32) * node->numChildren, file, 1);
        }
        
        ReadGLTFString(&node->name, file, allocator);
    }
    
    if (gltf->numMaterials > 0) gltf->materials = AllocZeroTLSFGlobal(gltf->numMaterials, sizeof(AMaterial));
    for (s32 i = 0; i < gltf->numMaterials; i++)
    {
        AMaterial* material = &gltf->materials[i];
        for (s32 j = 0; j < 3; j++)
        {
            ReadAMaterialTexture(&material->textures[j], file);
        }
        
        ReadAMaterialTexture(&material->baseColorTexture, file);
        ReadAMaterialTexture(&material->specularTexture, file);
        ReadAMaterialTexture(&material->metallicRoughnessTexture, file);
        
        u64 data;
        AFileRead(&data, sizeof(u64), file, 1);
        
        material->specularFactor    = data & 0xFFFF; data >>= sizeof(u16) * 8;
        material->emissiveFactor[2] = data & 0xFFFF; data >>= sizeof(u16) * 8;
        material->emissiveFactor[1] = data & 0xFFFF; data >>= sizeof(u16) * 8;
        material->emissiveFactor[0] = data & 0xFFFF; 
        
        AFileRead(&data, sizeof(u64), file, 1);
        material->diffuseColor  = (data >> 32);
        material->specularColor = data & 0xFFFFFFFF;
        
        AFileRead(&data, sizeof(u64), file, 1);
        material->baseColorFactor = (data >> 32);
        material->doubleSided     = data & 0x1;

        u32 packedFactors;
        AFileRead(&packedFactors, sizeof(u32), file, 1);
        material->metallicFactor  = (u16)(packedFactors >> 16u);
        material->roughnessFactor = (u16)(packedFactors & 0xFFFFu);
        
        AFileRead(&material->alphaCutoff, sizeof(float), file, 1);
        AFileRead(&material->alphaMode, sizeof(s32), file, 1);
        
        ReadGLTFString(&material->name, file, allocator);
    }
    
    if (gltf->numTextures > 0) gltf->textures = (ATexture*)AllocZeroTLSFGlobal(gltf->numTextures, sizeof(ATexture));
    for (s32 i = 0; i < gltf->numTextures; i++)
    {
        ATexture* texture = &gltf->textures[i];
        AFileRead(&texture->sampler, sizeof(s32), file, 1);
        AFileRead(&texture->source, sizeof(s32), file, 1);
        ReadGLTFString(&texture->name, file, allocator);
    }
    if (gltf->numImages > 0) gltf->images = (AImage*)AllocZeroTLSFGlobal(gltf->numImages, sizeof(AImage));
    for (s32 i = 0; i < gltf->numImages; i++)
    {
        ReadGLTFString(&gltf->images[i].path, file, allocator);
    }
    
    if (gltf->numSamplers > 0) gltf->samplers = (ASampler*)AllocZeroTLSFGlobal(gltf->numSamplers, sizeof(ASampler));
    for (s32 i = 0; i < gltf->numSamplers; i++)
    {
        AFileRead(&gltf->samplers[i], sizeof(ASampler), file, 1);
    }
    
    if (gltf->numCameras > 0) gltf->cameras = (ACamera*)AllocZeroTLSFGlobal(gltf->numCameras, sizeof(ACamera));
    for (s32 i = 0; i < gltf->numCameras; i++)
    {
        ACamera* camera = &gltf->cameras[i];
        AFileRead(&camera->aspectRatio, sizeof(float), file, 1);
        AFileRead(&camera->yFov, sizeof(float), file, 1);
        AFileRead(&camera->zFar, sizeof(float), file, 1);
        AFileRead(&camera->zNear, sizeof(float), file, 1);
        AFileRead(&camera->type, sizeof(s32), file, 1);
        ReadGLTFString(&camera->name, file, allocator);
    }
    
    if (gltf->numScenes > 0) gltf->scenes = (AScene*)AllocZeroTLSFGlobal(gltf->numScenes, sizeof(AScene));
    for (s32 i = 0; i < gltf->numScenes; i++)
    {
        AScene* scene = &gltf->scenes[i];
        ReadGLTFString(&scene->name, file, allocator);
        AFileRead(&scene->numNodes, sizeof(s32), file, 1);
        scene->nodes = FixedPow2Allocator_AllocateUninitialized(allocator, scene->numNodes * sizeof(s32));
        AFileRead(scene->nodes, sizeof(s32) * scene->numNodes, file, 1);
    }

    if (gltf->numSkins > 0) gltf->skins = (ASkin*)AllocZeroTLSFGlobal(gltf->numSkins, sizeof(ASkin));
    for (s32 i = 0; i < gltf->numSkins; i++)
    {
        ASkin* skin = &gltf->skins[i];
        AFileRead(&skin->skeleton, sizeof(s32), file, 1);
        AFileRead(&skin->numJoints, sizeof(s32), file, 1);
        skin->inverseBindMatrices = (f32*)AllocateTLSFGlobal(sizeof(mat4x4) * skin->numJoints);
        skin->joints = FixedPow2Allocator_AllocateUninitialized(allocator, skin->numJoints * sizeof(s32));
        AFileRead(skin->inverseBindMatrices, sizeof(mat4x4) * skin->numJoints, file, 1);
        AFileRead(skin->joints, sizeof(s32) * skin->numJoints, file, 1);
    }

    s32 totalAnimSamplerInput = 0;
    AFileRead(&totalAnimSamplerInput, sizeof(s32), file, 1);
    f32* currSamplerInput = NULL;
    v128f* currSamplerOutput = NULL;

    if (totalAnimSamplerInput) {
        currSamplerInput  = (f32*)AllocZeroTLSFGlobal(totalAnimSamplerInput, sizeof(float));
        currSamplerOutput = (v128f*)AllocZeroTLSFGlobal(totalAnimSamplerInput, sizeof(v128f));
        AFileRead(currSamplerInput, sizeof(float) * totalAnimSamplerInput, file, 1);
        AFileRead(currSamplerOutput, sizeof(v128f) * totalAnimSamplerInput, file, 1);
    }

    if (gltf->numAnimations) gltf->animations = AllocZeroTLSFGlobal(gltf->numAnimations, sizeof(AAnimation));
    for (s32 i = 0; i < gltf->numAnimations; i++)
    {
        AAnimation* animation = &gltf->animations[i];

        AFileRead(&animation->numSamplers, sizeof(s32), file, 1);
        AFileRead(&animation->numChannels, sizeof(s32), file, 1);
        AFileRead(&animation->duration, sizeof(float), file, 1);
        AFileRead(&animation->speed, sizeof(float), file, 1);
        ReadGLTFString(&animation->name, file, allocator);
        animation->channels = AllocateTLSFGlobal(animation->numChannels * sizeof(AAnimChannel));
        AFileRead(animation->channels, sizeof(AAnimChannel) * animation->numChannels, file, 1);
        animation->samplers = AllocateTLSFGlobal(animation->numSamplers * sizeof(AAnimSampler));

        for (s32 j = 0; j < animation->numSamplers; j++)
        {
            AFileRead(&animation->samplers[j].count, sizeof(s32), file, 1);
            AFileRead(&animation->samplers[j].numComponent, sizeof(s32), file, 1);
            AFileRead(&animation->samplers[j].inputType, sizeof(AComponentType), file, 1);
            AFileRead(&animation->samplers[j].outputType, sizeof(AComponentType), file, 1);
            AFileRead(&animation->samplers[j].interpolation, sizeof(ASamplerInterpolation), file, 1);
            s32 count = animation->samplers[j].count;
            animation->samplers[j].input = currSamplerInput;
            animation->samplers[j].output = (f32*)currSamplerOutput;
            currSamplerInput += count;
            currSamplerOutput += count;
        }
    }
    
    AFileRead(&gltf->rootNode, sizeof(int), file, 1);

    AFileClose(file);
    gltf->allocator = allocator;
    AX_LOG("abm load complete: %s meshes=%d nodes=%d skins=%d animations=%d",
           path, gltf->numMeshes, gltf->numNodes, gltf->numSkins, gltf->numAnimations);
    return 1;
}
