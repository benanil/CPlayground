// .scene text save/load: line separated description of a scene's bundles, entities,
// lights and texture tables, with the texture pages dumped raw in the platform's gpu
// format (.ctex) so loading skips transcoding and packing entirely. basis baking of
// the pages moves to the game build step
#include "Include/SceneSerializer.h"
#include "Include/AssetManager.h"
#include "Include/FileSystem.h"
#include "Include/Algorithm.h"
#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/Rendering.h"
#include "Include/Bitset.h"
#include "Math/Bitpack.h"

#define SCENE_FILE_VERSION 4

// first descriptors of a texture system are the built in defaults (TextureSystem.c)
enum { SceneSer_DefaultDescriptors = 4 };

static const char* const kAtlasSuffix[TextureClass_Count] = { "_albedo.ctex", "_normal.ctex", "_mr.ctex" };

/*//////////////////////////////////////////////////////////////////////////*/
/*                            Text Read / Write                             */
/*//////////////////////////////////////////////////////////////////////////*/

static char* WStr(char* p, const char* s)        { while (*s) *p++ = *s++; return p; }
static char* WInt(char* p, s64 v)                { *p++ = ' '; return p + IntToString(p, v, 0); }
static char* WFlt(char* p, float v)              { *p++ = ' '; return p + FloatToString(p, v, 6); }
static void  WEnd(AFile file, char* base, char* p) { *p++ = '\n'; AFileWrite(base, (u64)(p - base), file, 1); }

static const char* RU32(const char* p, u32* v)
{
    s64 value = 0;
    while (*p == ' ') p++;
    p = ParseNumberI64(p, &value);
    *v = (u32)value;
    return p;
}

static const char* RU64(const char* p, u64* v)
{
    s64 value = 0;
    while (*p == ' ') p++;
    p = ParseNumberI64(p, &value);
    *v = (u64)value;
    return p;
}

static const char* RFlt(const char* p, f32* v)
{
    while (*p == ' ') p++;
    return ParseFloat(p, v);
}

static v128f SceneUnpackLegacyScaleXY11Z10(u32 packed)
{
    v128u i = VeciSrl(VeciSet1(packed), VeciSetR(0, 11, 22, 31));
    i = VeciAnd(i, VeciSetR(0x7FF, 0x7FF, 0x3FF, 0));
    return VecMul(VecI32ToF32(i), VecSetR(1.0f / 2047.0f, 1.0f / 2047.0f, 1.0f / 1023.0f, 0.0f));
}

static const char* RSkipWord(const char* p)
{
    while (*p && *p != ' ') p++;
    return p;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                  Save                                    */
/*//////////////////////////////////////////////////////////////////////////*/

s32 SceneSerializer_Save(Scene* scene, const char* path)
{
    double startTime = TimeSinceStartup();
    TextureSystem* ts = &scene->textureSystem;

    // dump the page atlases raw in the gpu format, the text file references them by
    // name. empty scenes (only default descriptors) skip the dumps entirely
    char atlasPath[TextureClass_Count][1024];
    s32 atlasBaked[TextureClass_Count] = { 0, 0, 0 };

    if (ts->compressed && ts->numDescriptors > SceneSer_DefaultDescriptors)
    {
        for (u32 c = 0; c < TextureClass_Count; c++)
        {
            ChangeExtensionAndCopy(path, kAtlasSuffix[c], atlasPath[c], sizeof(atlasPath[c]));
            atlasBaked[c] = TextureSystem_SaveBakedClass(ts, c, atlasPath[c]);
        }
    }
    else if (!ts->compressed)
        AX_WARN("texture pages are not block compressed, scene saves without page dumps");

    // write the text to a tmp file, swap it in when complete
    char tmpPath[1024];
    int pathLen = StringLength(path);
    if (pathLen + 5 > (int)sizeof(tmpPath)) return 0;
    MemCopy(tmpPath, path, pathLen);
    MemCopy(tmpPath + pathLen, ".tmp", 5);

    // binary write keeps the file free of the text mode utf8 BOM
    AFile file = AFileOpen(tmpPath, AOpenFlag_WriteBinary);
    if (!AFileExist(file)) return 0;

    char line[1024];
    char* p;

    p = WStr(line, "axscene");
    p = WInt(p, SCENE_FILE_VERSION);
    WEnd(file, line, p);

    p = WStr(line, "sun");
    p = WFlt(p, g_RenderSettings.sunYaw);
    p = WFlt(p, g_RenderSettings.sunPitch);
    WEnd(file, line, p);

    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        p = WStr(line, "atlas");
        p = WInt(p, (s64)c);
        p = WInt(p, atlasBaked[c] > 0 ? (s64)ts->classes[c].openPages : 0);
        p = WStr(p, " ");
        p = WStr(p, atlasBaked[c] > 0 ? GetFileName(atlasPath[c]) : "-");
        WEnd(file, line, p);
    }

    // bundle indices are stable handles and may contain holes; persist only the live bundles,
    // a reload re-adds them sequentially into a hole-free scene.
    u32 liveBundles = 0;
    for (u32 b = 0; b < scene->numBundles; b++)
        liveBundles += scene->bundleRefs[b].bundle != NULL;

    p = WStr(line, "bundles");
    p = WInt(p, (s64)liveBundles);
    WEnd(file, line, p);
    for (u32 b = 0; b < scene->numBundles; b++)
    {
        const SceneBundleRef* ref = &scene->bundleRefs[b];
        if (!ref->bundle) continue;
        p = WStr(line, "bundle");
        p = WInt(p, ref->skinned != 0);
        p = WInt(p, (s64)ref->materialOffset);
        p = WInt(p, (s64)ref->bundle->numMaterials);
        p = WStr(p, " ");
        p = WStr(p, ref->path);
        WEnd(file, line, p);
    }

    p = WStr(line, "descriptors");
    p = WInt(p, (s64)ts->numDescriptors);
    WEnd(file, line, p);
    for (u32 i = 0; i < ts->numDescriptors; i++)
    {
        const TextureDescriptor* desc = &ts->descriptors[i];
        p = WStr(line, "desc");
        p = WInt(p, (s64)desc->pageIndex);
        p = WInt(p, (s64)(u32)(desc->uvBias.x  * TEXTURE_PAGE_SIZE + 0.5f));
        p = WInt(p, (s64)(u32)(desc->uvBias.y  * TEXTURE_PAGE_SIZE + 0.5f));
        p = WInt(p, (s64)(u32)(desc->uvScale.x * TEXTURE_PAGE_SIZE + 0.5f));
        p = WInt(p, (s64)(u32)(desc->uvScale.y * TEXTURE_PAGE_SIZE + 0.5f));
        p = WInt(p, (s64)desc->flags);
        WEnd(file, line, p);
    }

    p = WStr(line, "materials");
    p = WInt(p, (s64)ts->materialWatermark);
    WEnd(file, line, p);
    for (u32 i = 0; i < ts->materialWatermark; i++)
    {
        const MaterialGPU* material = &ts->materials[i];
        p = WStr(line, "mat");
        p = WInt(p, (s64)material->albedoDescriptor);
        p = WInt(p, (s64)material->normalDescriptor);
        p = WInt(p, (s64)material->metallicRoughnessDescriptor);
        p = WInt(p, (s64)material->flags);
        p = WInt(p, (s64)material->baseColorFactor);
        p = WInt(p, (s64)material->metallicRoughnessFactor);
        WEnd(file, line, p);
    }

    p = WStr(line, "lights");
    p = WInt(p, (s64)scene->numLights);
    WEnd(file, line, p);
    for (u32 i = 0; i < scene->numLights; i++)
    {
        const LightGPU* light = &scene->lights[i];
        p = WStr(line, "light");
        p = WInt(p, (s64)light->type);
        p = WInt(p, (s64)light->flags);
        for (u32 k = 0; k < 4u; k++) p = WFlt(p, light->positionRadius[k]);
        for (u32 k = 0; k < 4u; k++) p = WFlt(p, light->directionCone[k]);
        for (u32 k = 0; k < 4u; k++) p = WFlt(p, light->colorIntensity[k]);
        WEnd(file, line, p);
    }

    // raw render set entities in (group, local) order, the dense layout reproduces on load.
    // rotation and scale stay in their packed forms so the round trip is exact
    for (u32 s = 0; s < 3u; s++)
    {
        const RenderSet* set = s == 0u ? &scene->surfaceSet : (s == 1u ? &scene->skinnedSet : &scene->transparentSurfaceSet);
        p = WStr(line, "entities");
        p = WInt(p, (s64)s);
        p = WInt(p, (s64)set->numEntities);
        WEnd(file, line, p);

        for (u32 g = 0; g < set->numGroups; g++)
        {
            const PrimitiveGroup* group = &set->primitiveGroups[g];
            for (u32 e = 0; e < group->numEntities; e++)
            {
                const Entity* entity = &set->entities[group->entityOffset + e];
                AX_ALIGN(16) float position[4];
                VecStore(position, entity->position);

                p = WStr(line, "ent");
                p = WInt(p, (s64)g);
                for (u32 k = 0; k < 3u; k++) p = WFlt(p, position[k]);
                p = WInt(p, (s64)(u32)(entity->rotation & 0xFFFFFFFFull));
                p = WInt(p, (s64)(u32)(entity->rotation >> 32u));
                p = WInt(p, (s64)entity->scale);
                p = WInt(p, (s64)entity->sparseIdx);
                WEnd(file, line, p);
            }
        }
    }

    // physics overrides: only surface entities whose body deviates from the default
    // static-mesh collider. reapplied on load once the async collider build finishes.
    const RenderSet* surfaceSet = &scene->surfaceSet;
    u32 physCount = 0;
    for (u32 g = 0; g < surfaceSet->numGroups; g++)
    {
        const PrimitiveGroup* group = &surfaceSet->primitiveGroups[g];
        for (u32 e = 0; e < group->numEntities; e++)
        {
            ScenePhysicsRecord rec;
            physCount += Scene_PhysicsGetEntityOverride(scene, surfaceSet->entities[group->entityOffset + e].sparseIdx, &rec);
        }
    }

    p = WStr(line, "physics");
    p = WInt(p, (s64)physCount);
    WEnd(file, line, p);
    for (u32 g = 0; g < surfaceSet->numGroups; g++)
    {
        const PrimitiveGroup* group = &surfaceSet->primitiveGroups[g];
        for (u32 e = 0; e < group->numEntities; e++)
        {
            ScenePhysicsRecord rec;
            if (!Scene_PhysicsGetEntityOverride(scene, surfaceSet->entities[group->entityOffset + e].sparseIdx, &rec))
                continue;
            p = WStr(line, "phys");
            p = WInt(p, (s64)rec.sparseIdx);
            p = WInt(p, (s64)rec.bodyType);
            p = WInt(p, (s64)rec.shapeType);
            p = WInt(p, (s64)rec.lockBits);
            p = WFlt(p, rec.friction);
            p = WFlt(p, rec.restitution);
            p = WFlt(p, rec.density);
            p = WFlt(p, rec.linearDamping);
            p = WFlt(p, rec.angularDamping);
            p = WFlt(p, rec.gravityScale);
            p = WFlt(p, rec.sleepThreshold);
            WEnd(file, line, p);
        }
    }

    AFileClose(file);
    RemoveFile(path);
    RenameFile(tmpPath, path);
    AX_LOG("scene saved: %s bundles=%d entities=%d lights=%d %.2fs",
           path, scene->numBundles, scene->surfaceSet.numEntities + scene->skinnedSet.numEntities + scene->transparentSurfaceSet.numEntities,
           scene->numLights, TimeSinceStartup() - startTime);
    return 1;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                                  Load                                    */
/*//////////////////////////////////////////////////////////////////////////*/

// one serialized render set entity, packed transform forms round trip exactly
typedef struct SceneEntRecord_
{
    u64   rotation;
    u64   scale;
    float position[3];
    u32   primGroupIdx;
    u32   sparseIdx;
} SceneEntRecord;

typedef struct SceneFileData_
{
    f32 sunYaw, sunPitch;
    u32 atlasLayers[TextureClass_Count];
    char atlasPath[TextureClass_Count][1024];

    u32   numBundles;
    char* bundlePaths;       // numBundles * 1024
    u32*  bundleSkinned;
    u32*  bundleMaterialOff;

    TextureDescriptor* descriptors;
    u32 numDescriptors;

    MaterialGPU* materials;
    u32 materialWatermark;

    LightGPU* lights;
    u32 numLights;

    SceneEntRecord* entities[3]; // 0 surface, 1 skinned, 2 transparent surface
    u32 numEntities[3];

    ScenePhysicsRecord* physics; // surface entities that override the default collider
    u32 numPhysics;
} SceneFileData;

// reads one line and checks it starts with the expected keyword. the line keeps its
// trailing newline from AFileReadLine, trim it so path tails parse clean.
// out: token tail or NULL
static const char* ReadRecord(AFile file, char* buffer, int bufferSize, const char* keyword)
{
    int len = AFileReadLine(buffer, bufferSize, file);
    if (len <= 0)
    {
        AX_WARN("scene record missing, expected '%s'", keyword);
        return NULL;
    }
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' || buffer[len - 1] == ' '))
        buffer[--len] = '\0';
    int keyLen = StringLength(keyword);
    if (len < keyLen || !StringEqual(buffer, keyword, keyLen))
    {
        AX_WARN("scene record mismatch, expected '%s' got '%.48s'", keyword, buffer);
        return NULL;
    }
    return buffer + keyLen;
}

static s32 ParseSceneFile(const char* path, SceneFileData* data)
{
    AFile file = AFileOpen(path, AOpenFlag_ReadBinary);
    if (!AFileExist(file))
    {
        AX_ERROR("scene file not found: %s", path);
        return 0;
    }

    char baseDir[1024];
    char line[2048];
    const char* p;
    GetBaseDir(path, baseDir);
    s32 baseLen = StringLength(baseDir);

    u32 version = 0;
    if (!(p = ReadRecord(file, line, sizeof(line), "axscene")))
    {
        // tolerate a utf8 BOM in front of the first record (text mode writers add one)
        bool bom = line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF' &&
                   StringEqual(line + 3, "axscene", 7);
        if (!bom) goto fail;
        p = line + 3 + 7;
    }
    RU32(p, &version);
    if (version > SCENE_FILE_VERSION)
    {
        AX_ERROR("scene file version %d not supported: %s", version, path);
        AFileClose(file);
        return 0;
    }

    if (!(p = ReadRecord(file, line, sizeof(line), "sun"))) goto fail;
    p = RFlt(p, &data->sunYaw);
    RFlt(p, &data->sunPitch);

    for (u32 c = 0; c < TextureClass_Count; c++)
    {
        u32 classIdx = 0;
        if (!(p = ReadRecord(file, line, sizeof(line), "atlas"))) goto fail;
        p = RU32(p, &classIdx);
        p = RU32(p, &data->atlasLayers[c]);
        while (*p == ' ') p++;
        data->atlasPath[c][0] = '\0';
        if (data->atlasLayers[c] > 0u && *p && *p != '-')
        {
            int nameLen = StringLength(p);
            if (baseLen + nameLen + 1 <= (int)sizeof(data->atlasPath[c]))
            {
                MemCopy(data->atlasPath[c], baseDir, baseLen);
                MemCopy(data->atlasPath[c] + baseLen, p, nameLen + 1);
            }
        }
    }

    if (!(p = ReadRecord(file, line, sizeof(line), "bundles"))) goto fail;
    RU32(p, &data->numBundles);
    if (data->numBundles > MAX_SCENE_BUNDLES) goto fail;
    data->bundlePaths       = (char*)ArenaAllocZero(&GlobalArena, (u64)Maxu32(data->numBundles, 1u) * 1024u);
    data->bundleSkinned     = (u32*)ArenaAllocZero(&GlobalArena, Maxu32(data->numBundles, 1u) * sizeof(u32));
    data->bundleMaterialOff = (u32*)ArenaAllocZero(&GlobalArena, Maxu32(data->numBundles, 1u) * sizeof(u32));
    for (u32 b = 0; b < data->numBundles; b++)
    {
        u32 numMaterials = 0;
        if (!(p = ReadRecord(file, line, sizeof(line), "bundle"))) goto fail;
        p = RU32(p, &data->bundleSkinned[b]);
        p = RU32(p, &data->bundleMaterialOff[b]);
        p = RU32(p, &numMaterials);
        while (*p == ' ') p++;
        int nameLen = StringLength(p);
        if (nameLen <= 0 || nameLen >= 1024) goto fail;
        MemCopy(data->bundlePaths + (u64)b * 1024u, p, nameLen + 1);
    }

    if (!(p = ReadRecord(file, line, sizeof(line), "descriptors"))) goto fail;
    RU32(p, &data->numDescriptors);
    if (data->numDescriptors > MAX_TEXTURE_DESCRIPTORS) goto fail;
    data->descriptors = (TextureDescriptor*)ArenaAllocZero(&GlobalArena, Maxu32(data->numDescriptors, 1u) * sizeof(TextureDescriptor));
    for (u32 i = 0; i < data->numDescriptors; i++)
    {
        u32 page = 0, x = 0, y = 0, w = 0, h = 0, flags = 0;
        if (!(p = ReadRecord(file, line, sizeof(line), "desc"))) goto fail;
        p = RU32(p, &page);
        p = RU32(p, &x);
        p = RU32(p, &y);
        p = RU32(p, &w);
        p = RU32(p, &h);
        RU32(p, &flags);
        TextureDescriptor* desc = &data->descriptors[i];
        desc->pageIndex = page;
        desc->flags = flags;
        desc->uvBias.x  = (float)x / (float)TEXTURE_PAGE_SIZE;
        desc->uvBias.y  = (float)y / (float)TEXTURE_PAGE_SIZE;
        desc->uvScale.x = (float)w / (float)TEXTURE_PAGE_SIZE;
        desc->uvScale.y = (float)h / (float)TEXTURE_PAGE_SIZE;
    }

    if (!(p = ReadRecord(file, line, sizeof(line), "materials"))) goto fail;
    RU32(p, &data->materialWatermark);
    if (data->materialWatermark > MAX_GPU_MATERIALS) goto fail;
    data->materials = (MaterialGPU*)ArenaAllocZero(&GlobalArena, Maxu32(data->materialWatermark, 1u) * sizeof(MaterialGPU));
    for (u32 i = 0; i < data->materialWatermark; i++)
    {
        MaterialGPU* material = &data->materials[i];
        if (!(p = ReadRecord(file, line, sizeof(line), "mat"))) goto fail;
        p = RU32(p, &material->albedoDescriptor);
        p = RU32(p, &material->normalDescriptor);
        p = RU32(p, &material->metallicRoughnessDescriptor);
        p = RU32(p, &material->flags);
        p = RU32(p, &material->baseColorFactor);
        RU32(p, &material->metallicRoughnessFactor);
    }

    if (!(p = ReadRecord(file, line, sizeof(line), "lights"))) goto fail;
    RU32(p, &data->numLights);
    if (data->numLights > MAX_SCENE_LIGHTS) goto fail;
    data->lights = (LightGPU*)ArenaAllocZero(&GlobalArena, Maxu32(data->numLights, 1u) * sizeof(LightGPU));
    for (u32 i = 0; i < data->numLights; i++)
    {
        LightGPU* light = &data->lights[i];
        if (!(p = ReadRecord(file, line, sizeof(line), "light"))) goto fail;
        p = RU32(p, &light->type);
        p = RU32(p, &light->flags);
        for (u32 k = 0; k < 4u; k++) p = RFlt(p, &light->positionRadius[k]);
        for (u32 k = 0; k < 4u; k++) p = RFlt(p, &light->directionCone[k]);
        for (u32 k = 0; k < 4u; k++) p = RFlt(p, &light->colorIntensity[k]);
        light->shadowIndex = LIGHT_SHADOW_INDEX_INVALID;
        light->padding = 0u;
    }

    u32 serializedSets = version >= 3u ? 3u : 2u;
    for (u32 s = 0; s < serializedSets; s++)
    {
        u32 setIdx = 0;
        if (!(p = ReadRecord(file, line, sizeof(line), "entities"))) goto fail;
        p = RU32(p, &setIdx);
        RU32(p, &data->numEntities[s]);
        if (setIdx != s || data->numEntities[s] > (s == 1u ? MAX_ANIM_INSTANCES : MAX_ENTITY)) goto fail;
        data->entities[s] = (SceneEntRecord*)ArenaAllocZero(&GlobalArena, Maxu32(data->numEntities[s], 1u) * sizeof(SceneEntRecord));
        for (u32 i = 0; i < data->numEntities[s]; i++)
        {
            SceneEntRecord* record = &data->entities[s][i];
            u32 rotLo = 0, rotHi = 0;
            if (!(p = ReadRecord(file, line, sizeof(line), "ent"))) goto fail;
            p = RU32(p, &record->primGroupIdx);
            for (u32 k = 0; k < 3u; k++) p = RFlt(p, &record->position[k]);
            p = RU32(p, &rotLo);
            p = RU32(p, &rotHi);
            record->scale = 0;
            if (version == 1)
            {
                u32 legacyScale = 0;
                p = RU32(p, &legacyScale);
                record->scale = EntityPackWorldScale(VecMulf(SceneUnpackLegacyScaleXY11Z10(legacyScale), 10.0f));
            }
            if (version >= 2)
                p = RU64(p, &record->scale);

            RU32(p, &record->sparseIdx);
            record->rotation = (u64)rotLo | ((u64)rotHi << 32u);
        }
    }

    // physics overrides section (version 4+); absent in older files
    if (version >= 4u)
    {
        if (!(p = ReadRecord(file, line, sizeof(line), "physics"))) goto fail;
        RU32(p, &data->numPhysics);
        if (data->numPhysics > MAX_ENTITY) goto fail;
        data->physics = (ScenePhysicsRecord*)ArenaAllocZero(&GlobalArena, Maxu32(data->numPhysics, 1u) * sizeof(ScenePhysicsRecord));
        for (u32 i = 0; i < data->numPhysics; i++)
        {
            ScenePhysicsRecord* rec = &data->physics[i];
            if (!(p = ReadRecord(file, line, sizeof(line), "phys"))) goto fail;
            p = RU32(p, &rec->sparseIdx);
            p = RU32(p, &rec->bodyType);
            p = RU32(p, &rec->shapeType);
            p = RU32(p, &rec->lockBits);
            p = RFlt(p, &rec->friction);
            p = RFlt(p, &rec->restitution);
            p = RFlt(p, &rec->density);
            p = RFlt(p, &rec->linearDamping);
            p = RFlt(p, &rec->angularDamping);
            p = RFlt(p, &rec->gravityScale);
            p = RFlt(p, &rec->sleepThreshold);
        }
    }

    AFileClose(file);
    return 1;
fail:
    AX_ERROR("scene file parse failed: %s", path);
    AFileClose(file);
    return 0;
}

static void BuildColliderEndCallback(void* data, s32 result)
{
	Scene* scene = (Scene*)data;
	Scene_PhysicsApplyPendingOverrides(scene); // no-op when nothing was persisted
	scene->physicsReady = true;
}

s32 SceneSerializer_Load(Scene* scene, const char* path)
{
    double startTime = TimeSinceStartup();
    ArenaMark mark = ArenaSave(&GlobalArena);

    SceneFileData data;
    MemsetZero(&data, sizeof(data));
    if (!ParseSceneFile(path, &data))
    {
        ArenaRestore(&GlobalArena, mark);
        return 0;
    }

    // the fast path needs compressed pages, every referenced atlas on disk and the tables
    bool fast = scene->textureSystem.compressed != 0u && data.numDescriptors >= SceneSer_DefaultDescriptors;
    for (u32 c = 0; c < TextureClass_Count && fast; c++)
        if (data.atlasLayers[c] > 0u && (data.atlasPath[c][0] == '\0' || !FileExist(data.atlasPath[c])))
            fast = false;

    if (fast)
    {
        for (u32 b = 0; b < data.numBundles; b++)
        {
            const char* bundlePath = data.bundlePaths + (u64)b * 1024u;
            u32 bundleIdx = Scene_AddBundleBaked(scene, bundlePath, data.bundleMaterialOff[b]);
            if (bundleIdx == INVALID_BUNDLE)
            {
                ArenaRestore(&GlobalArena, mark);
                return 0;
            }
        }

        const char* atlasPaths[TextureClass_Count];
        for (u32 c = 0; c < TextureClass_Count; c++)
            atlasPaths[c] = data.atlasLayers[c] > 0u ? data.atlasPath[c] : NULL;

        if (TextureSystem_RestoreBaked(&scene->textureSystem, atlasPaths,
                                       data.descriptors, data.numDescriptors,
                                       data.materials, data.materialWatermark))
        {
            scene->texturesBaked = 1;
        }
        else
        {
            AX_WARN("baked atlas restore failed, repacking from bundle caches: %s", path);
            if (!Scene_RepackTextures(scene))
            {
                ArenaRestore(&GlobalArena, mark);
                return 0;
            }
        }
        if (data.materialWatermark > scene->numMaterials)
            scene->numMaterials = data.materialWatermark;
    }
    else
    {
        AX_LOG("scene loading through the slow path (no baked atlases): %s", path);
        for (u32 b = 0; b < data.numBundles; b++)
        {
            const char* bundlePath = data.bundlePaths + (u64)b * 1024u;
            u32 bundleIdx = Scene_AddBundle(scene, bundlePath, data.bundleSkinned[b] != 0u);
            if (bundleIdx == INVALID_BUNDLE)
            {
                ArenaRestore(&GlobalArena, mark);
                return 0;
            }
            if (scene->bundleRefs[bundleIdx].materialOffset != data.bundleMaterialOff[b])
                AX_WARN("scene bundle material offset drifted: %s %d != %d",
                        bundlePath, scene->bundleRefs[bundleIdx].materialOffset, data.bundleMaterialOff[b]);
        }
    }

    // entities restore straight into the render sets, records are in (group, local) order
    // so the dense layout and sparse ids come back exactly as saved
    for (u32 s = 0; s < 3u; s++)
    {
        bool isSkinned = s == 1u;
        RenderSet* set = s == 0u ? &scene->surfaceSet : (s == 1u ? &scene->skinnedSet : &scene->transparentSurfaceSet);

        u32* primitiveCounts = (u32*)ArenaAllocGlobal(set->numGroups * sizeof(u32));
        MemSet(primitiveCounts, 0, set->numGroups * sizeof(u32));
        u32 validEntities = 0;

        for (u32 i = 0; i < data.numEntities[s]; i++)
        {
            const SceneEntRecord* record = &data.entities[s][i];
            if (record->primGroupIdx >= set->numGroups)
            {
                AX_WARN("scene entity group out of range: %d >= %d", record->primGroupIdx, set->numGroups);
                continue;
            }
            if (record->sparseIdx >= set->maxEntities)
            {
                AX_WARN("scene entity sparse id out of range: %d >= %d", record->sparseIdx, set->maxEntities);
                continue;
            }
            primitiveCounts[record->primGroupIdx]++;
            validEntities++;
        }

        u32 entityOffset = 0;
        for (u32 g = 0; g < set->numGroups; g++)
        {
            PrimitiveGroup* group = &set->primitiveGroups[g];
            group->entityOffset = entityOffset;
            u32 numEntities = primitiveCounts[g];
            group->numEntities = 0;
            group->capacity = numEntities;
            entityOffset += numEntities;
        }
        set->numEntities = validEntities;
        primitiveCounts = NULL;
        ArenaPopGlobal(set->numGroups * sizeof(u32)); // primitiveCounts

        for (u32 i = 0; i < data.numEntities[s]; i++)
        {
            const SceneEntRecord* record = &data.entities[s][i];
            u32 groupIdx = record->primGroupIdx;
            if (groupIdx >= set->numGroups || record->sparseIdx >= set->maxEntities)
                continue;

            PrimitiveGroup* group = &set->primitiveGroups[groupIdx];
            u32 denseIdx = group->entityOffset + group->numEntities;
            Entity* entity = &set->entities[denseIdx];
            MemsetZero(entity, sizeof(entity));
            group->numEntities++;
            entity->position  = Vec3Load(record->position);
            entity->rotation  = record->rotation;
            entity->scale     = record->scale;
            entity->primitiveIdx = groupIdx;
            entity->sparseIdx = record->sparseIdx;

            BitsetSet(set->sparseSlots, (s32)record->sparseIdx);
            if (set->sparseID[record->sparseIdx] == INVALID_ENTITY || denseIdx < set->sparseID[record->sparseIdx])
                set->sparseID[record->sparseIdx] = denseIdx;

            if (isSkinned)
            {
                u32 bundleIdx = Scene_FindBundleForRenderGroup(scene, true, groupIdx);
                if (bundleIdx != INVALID_BUNDLE)
                {
                    GPUAnimationInstance instance = {
                        .animIdx = Scene_DefaultAnimation(scene, bundleIdx),
                        .timeOffset = 0.0f
                    };
                    AnimationSystem_SetInstance(&scene->animSystem, entity->sparseIdx, instance);
                }
            }
        }
        RenderSet_Validate(set, isSkinned ? "load skinned" : (s == 2u ? "load transparent surface" : "load surface"));
    }

  if (data.numLights > 0)
        MemCopy(scene->lights, data.lights, data.numLights * sizeof(LightGPU));
    scene->numLights = data.numLights;

    g_RenderSettings.sunYaw   = data.sunYaw;
    g_RenderSettings.sunPitch = data.sunPitch;
    scene->renderDataDirty = 1;

    // copy physics overrides out of the arena; applied when the collider build finishes
    if (scene->pendingPhysics) DeAllocateTLSFGlobal(scene->pendingPhysics);
    scene->pendingPhysics = NULL;
    scene->numPendingPhysics = 0;
    if (data.numPhysics > 0)
    {
        scene->pendingPhysics = (ScenePhysicsRecord*)AllocateTLSFGlobal(data.numPhysics * sizeof(ScenePhysicsRecord));
        if (scene->pendingPhysics)
        {
            MemCopy(scene->pendingPhysics, data.physics, data.numPhysics * sizeof(ScenePhysicsRecord));
            scene->numPendingPhysics = data.numPhysics;
        }
    }

    ArenaRestore(&GlobalArena, mark);
	scene->physicsReady = false;
    // every mesh instance is static for now: give each a static rigid body with a triangle collider
    Scene_BuildStaticCollidersAsync(scene, BuildColliderEndCallback);

    AX_LOG("scene loaded: %s bundles=%d entities=%d lights=%d baked=%d %.2fs",
           path, scene->numBundles, scene->surfaceSet.numEntities + scene->skinnedSet.numEntities + scene->transparentSurfaceSet.numEntities,
           scene->numLights, scene->texturesBaked, TimeSinceStartup() - startTime);
    return 1;
}
