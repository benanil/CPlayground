// scene hierarchy and texture system inspector windows
#include "EditorInternal.h"
#include "Include/Platform.h"
#include "Include/Random.h"
#include "Include/Algorithm.h"
#include "Include/Scene.h"
#include <box3d/box3d.h>
#include "Include/FileSystem.h"
#include "Include/AssetManager.h"
#include "Include/GLTFParser.h"
#include "Include/Rendering.h"
#include "Include/Graphics.h"
#include "Include/Camera.h"
#include "Include/BVH.h"
#include "Math/Quaternion.h"
#include "Math/Bitpack.h"
#include "Math/Color.h"
#include "Math/Half.h"

#define SCENE_LIGHT_GIZMO_RADIUS 10.0f
#define EDITOR_SCENE_FOLDER "Assets/Scenes"
#define EDITOR_SCENE_MAX_FILES 256
#define EDITOR_SCENE_DOUBLE_CLICK_SECONDS 0.4f

typedef struct EditorSceneFile_
{
    char path[512];
    u16 nameOffset;
} EditorSceneFile;

typedef struct SceneObjectSelection_
{
    bool valid;
    u32 skinned;
    u32 groupIdx;
    u32 entityIdx;
    u32 bundleIdx;
    u32 animIdx;
    f32 animTime;
} SceneObjectSelection;

extern SDL_GPUDevice* g_GPUDevice;
extern WindowState g_WindowState;
extern Graphics gGFX;

//------------------------------------------------------------------------
// Scene Window

// hierarchy selection, bundle row when node is -1, nothing when bundle is INVALID_BUNDLE
static u32  sceneSelectedBundle = INVALID_BUNDLE;
static s32  sceneSelectedNode   = -1;
static bool sceneInfoOpen       = true;
static bool sceneLightsOpen     = true;
static bool sceneInspectorOpen  = true;
static bool scenePhysicsOpen    = true;
static bool scenePhysicsLocksOpen = false;
static s32  sceneSelectedLight  = -1;
static u32  sceneTreeRowBudget;
static bool sceneLightGizmoDragging;
static f32  sceneLightGizmoDepth;

static SceneObjectSelection sceneObjectSelection;

static bool sceneSavePopupOpen;
static bool sceneSaveConfirmOpen;
static bool sceneDeletePopupOpen;
static char sceneSaveName[128];
static char sceneDeletePath[512];

static EditorSceneFile sceneFiles[EDITOR_SCENE_MAX_FILES];
static f32  sceneFilesLastScan;
static f32  sceneLastClickTime = -10.0f;
static s32  sceneSelectedFile  = -1;
static bool sceneFilesDirty = true;
static u32  sceneNumFiles;
static u32  sceneLastClickHash;

static const char* const kEditorSceneAtlasSuffix[TextureClass_Count] = { "_albedo.ctex", "_normal.ctex", "_mr.ctex" };

extern void TerrainEditorSceneChanged(bool loadSidecar);

static bool SceneRowRightClicked(Clay_ElementId id);
static bool EditorImportNeedsDetailWarning(const char* normalizedPath);


// spawns one instance and assigns the default animation to skinned ones
static void EditorSpawnBundleAt(Scene* scene, u32 bundleIdx, v128f position, v128f rotation, v128f scale)
{
    Scene_Spawn(scene, bundleIdx, position, rotation, scale);
}

static void EditorSpawnBundle(Scene* scene, u32 bundleIdx, f32 scale)
{
    EditorSpawnBundleAt(scene, bundleIdx, VecZero(), QIdentity(), VecSet1(scale));
}

static void EditorSceneResetState(void)
{
    EditorGizmoClear();
    sceneSelectedBundle = INVALID_BUNDLE;
    sceneSelectedNode   = -1;
    sceneSelectedLight  = -1;
    sceneObjectSelection.valid = false;
}

Scene* EditorNewScene(void)
{
    Scene* scene = Scene_NewActive();
    EditorSceneResetState();
    TerrainEditorSceneChanged(false);
    return scene;
}

static void ImportMeshToSceneFinish(SceneAsyncRequest* request)
{
    Scene* scene = Scene_GetActive();
    if (!scene) scene = EditorNewScene();

    u32 bundleIdx = Scene_AddBundleAuto(scene, request->path);
    if (bundleIdx == INVALID_BUNDLE)
    {
        AX_ERROR("import to scene failed: %s", request->path);
        return;
    }
    EditorSpawnBundle(scene, bundleIdx, 1.0f);
}

static void OpenSceneFinish(SceneAsyncRequest* request)
{
    const char* path = request->path;
    Scene* scene = Scene_OpenActive(path);
    if (!scene) return;
    EditorSceneResetState();
    EditorSettingsSetLastScene(Scene_GetActivePath());
    TerrainEditorSceneChanged(false);
}

static void SceneSelectObject(u32 skinned, u32 groupIdx, u32 entityIdx, u32 bundleIdx)
{
    sceneObjectSelection = (SceneObjectSelection){
        .valid     = true,
        .skinned   = skinned,
        .groupIdx  = groupIdx,
        .entityIdx = entityIdx,
        .bundleIdx = bundleIdx,
        .animIdx   = 0u,
        .animTime  = 0.0f
    };
}

void EditorImportMeshToScene(const char* path)
{
    char normalized[512];
    NormalizePath(path, normalized, sizeof(normalized));
    if (EditorImportNeedsDetailWarning(normalized))
    {
        EditorOpenImportDetail(normalized);
        return;
    }

    SceneAsyncBegin(SceneAsyncOp_ImportMesh, path, "EditorImportMesh", ImportMeshToSceneFinish);
}

void EditorOpenScene(const char* path)
{
    SceneAsyncBegin(SceneAsyncOp_OpenScene, path, "EditorOpenScene", OpenSceneFinish);
}

//------------------------------------------------------------------------
// Import with detail popup

static bool importDetailOpen;
static char importDetailPath[512];
static f32  importDetailScale = 1.0f;
static f32  importDetailEuler[3];
static char importDetailScaleText[32];

typedef struct ImportDetailInfo_
{
    u32 meshes;
    u32 nodes;
    u32 materials;
    u32 images;
    u32 vertices;
    u32 indices;
    u32 skins;
    u32 animations;
    f32 maxSkinnedAnimMeters;
    f32 recommendedScale;
    bool animBoundsWarning;
} ImportDetailInfo;

static ImportDetailInfo importDetailInfo;

static void ImportDetailSetScaleText(f32 scale)
{
    importDetailScale = scale;
    int len = FloatToString(importDetailScaleText, scale, 4);
    importDetailScaleText[len] = '\0';
}

static f32 ImportRecommendAnimationScale(f32 maxMeters)
{
    if (maxMeters <= 0.0f) return 1.0f;
    f32 exact = ANIMATION_MAX_METERS / maxMeters;
    if (exact >= 1.0f)  return 1.0f;
    if (exact >= 0.1f)  return 0.1f;
    if (exact >= 0.01f) return 0.01f;
    return 0.01f;
}

static bool ImportPositionValid(v128f position)
{
    f32 maxAbs = Max3(VecFabs(position));
    return maxAbs == maxAbs && maxAbs < 100000.0f;
}

static void ImportDetailSetAnimationBoundsInfo(ImportDetailInfo* info, const SceneBundle* bundle)
{
    if (!info || !bundle || bundle->numSkins <= 0) return;

    f32 maxMeters = 0.0f;
    bool foundBounds = false;
    for (s32 m = 0; m < bundle->numMeshes; m++)
    {
        const AMesh* mesh = &bundle->meshes[m];
        for (s32 p = 0; p < mesh->numPrimitives; p++)
        {
            const APrimitive* primitive = &mesh->primitives[p];
            const float3* positions = (const float3*)primitive->vertexAttribs[AAttribIdx_POSITION];
            bool skinned = bundle->numSkins > 0;
            if (!positions || (float3*)(0xcdcdcdcdcdcdcdcdull) == positions)
            {
                if (bundle->numSkins > 0)
                {
                    const ASkinedVertex* vertices = (ASkinedVertex*)primitive->vertices;
                    for (s32 v = 0; v < primitive->numVertices; v++)
                    {
                        v128f position = Half4ToFloat4Vec(&vertices[v].positionXY);
                        if (!ImportPositionValid(position)) continue;
                        maxMeters = Maxf32(maxMeters, Max3(VecFabs(position)));
                        foundBounds = true;
                    }
                }
                else
                {
                    v128f primMin = VecLoad(primitive->min);
                    v128f primExtend = VecMax(VecSub(VecLoad(primitive->max), primMin), VecSet1(1.0e-6f));
                    for (s32 v = 0; v < primitive->numVertices; v++)
                    {
                        const AVertex* v = (const AVertex*)primitive->vertices;
                        v128f position = VecAdd(primMin, VecMul(UnpackUnorm16x4(v->position), primExtend));
                        if (!ImportPositionValid(position)) continue;
                        maxMeters = Maxf32(maxMeters, Max3(VecFabs(position)));
                        foundBounds = true;
                    }
                }
                continue;
            }
            for (s32 v = 0; v < primitive->numVertices; v++)
            {
                v128f position = Vec3Load(&positions[v].x);
                if (!ImportPositionValid(position)) continue;
                maxMeters = Maxf32(maxMeters, Max3(VecFabs(position)));
                foundBounds = true;
            }
        }
    }

    if (!foundBounds)
    {
        AX_LOG("couldn't found bounds of mesh when importing");
        return;
    }
    info->maxSkinnedAnimMeters = maxMeters;
    info->recommendedScale = ImportRecommendAnimationScale(maxMeters);
    info->animBoundsWarning = maxMeters > ANIMATION_MAX_METERS;
}

static bool EditorReadBundleInfoFromScene(const char* path, ImportDetailInfo* info)
{
    Scene* scene = Scene_GetActive();
    if (!scene) return false;

    u32 pathLen = (u32)StringLength(path) + 1u;
    for (u32 i = 0; i < scene->numBundles; i++)
    {
        if (scene->bundleRefs[i].bundle && StringEqual(scene->bundleRefs[i].path, path, pathLen))
        {
            const SceneBundle* bundle = scene->bundleRefs[i].bundle;
            *info = (ImportDetailInfo){
                .meshes     = (u32)bundle->numMeshes,
                .nodes      = (u32)bundle->numNodes,
                .materials  = (u32)bundle->numMaterials,
                .images     = (u32)bundle->numImages,
                .vertices   = (u32)bundle->totalVertices,
                .indices    = (u32)bundle->totalIndices,
                .skins      = (u32)bundle->numSkins,
                .animations = (u32)bundle->numAnimations
            };
            ImportDetailSetAnimationBoundsInfo(info, bundle);
            return true;
        }
    }
    return false;
}

static bool EditorReadBundleInfoFromABM(const char* path, ImportDetailInfo* info)
{
    char buffer[1024];
    int pathLen = StringLength(path);
    MemCopy(buffer, path, pathLen + 1);
    ChangeExtension(buffer, pathLen, "abm");
    if (!IsABMLastVersion(buffer)) return false;

    AFile file = AFileOpen(buffer, AOpenFlag_ReadBinary);
    if (!AFileExist(file)) return false;

    s32 version;
    u64 reserved[4];
    f32 scale;
    u16 numMeshes, numNodes, numMaterials, numTextures, numImages, numSamplers;
    u16 numCameras, numScenes, numSkins, numAnimations, defaultSceneIndex, isSkinned;
    s32 totalIndices, totalVertices;
    AFileRead(&version          , sizeof(version)          , file, 1);
    AFileRead(reserved          , sizeof(reserved)         , file, 1);
    AFileRead(&scale            , sizeof(scale)            , file, 1);
    AFileRead(&numMeshes        , sizeof(numMeshes)        , file, 1);
    AFileRead(&numNodes         , sizeof(numNodes)         , file, 1);
    AFileRead(&numMaterials     , sizeof(numMaterials)     , file, 1);
    AFileRead(&numTextures      , sizeof(numTextures)      , file, 1);
    AFileRead(&numImages        , sizeof(numImages)        , file, 1);
    AFileRead(&numSamplers      , sizeof(numSamplers)      , file, 1);
    AFileRead(&numCameras       , sizeof(numCameras)       , file, 1);
    AFileRead(&numScenes        , sizeof(numScenes)        , file, 1);
    AFileRead(&numSkins         , sizeof(numSkins)         , file, 1);
    AFileRead(&numAnimations    , sizeof(numAnimations)    , file, 1);
    AFileRead(&defaultSceneIndex, sizeof(defaultSceneIndex), file, 1);
    AFileRead(&isSkinned        , sizeof(isSkinned)        , file, 1);
    AFileRead(&totalIndices     , sizeof(totalIndices)     , file, 1);
    AFileRead(&totalVertices    , sizeof(totalVertices)    , file, 1);
    AFileClose(file);

    (void)version;
    (void)reserved;
    (void)scale;
    (void)numTextures;
    (void)numSamplers;
    (void)numCameras;
    (void)numScenes;
    (void)defaultSceneIndex;
    (void)isSkinned;
    *info = (ImportDetailInfo){
        .meshes     = (u32)numMeshes,
        .nodes      = (u32)numNodes,
        .materials  = (u32)numMaterials,
        .images     = (u32)numImages,
        .vertices   = (u32)Maxs32(totalVertices, 0),
        .indices    = (u32)Maxs32(totalIndices, 0),
        .skins      = (u32)numSkins,
        .animations = (u32)numAnimations
    };
    return true;
}

static bool EditorReadBundleInfoFromGLTF(const char* path, ImportDetailInfo* info)
{
    SceneBundle bundle;
    bool loaded = false;
    int pathLen = StringLength(path);
    if (FileHasExtension(path, pathLen, ".glb") || FileHasExtension(path, pathLen, ".gltf"))
        loaded = ParseGLTF(path, &bundle, 1.0f) != 0;
    else if (FileHasExtension(path, pathLen, ".abm"))
        return EditorReadBundleInfoFromABM(path, info);
    else // fbx obj
        loaded = LoadFBX(path, &bundle, 1.0f) != 0;
    
    if (!loaded) return false;
    *info = (ImportDetailInfo){
        .meshes     = (u32)bundle.numMeshes,
        .nodes      = (u32)bundle.numNodes,
        .materials  = (u32)bundle.numMaterials,
        .images     = (u32)bundle.numImages,
        .vertices   = (u32)bundle.totalVertices,
        .indices    = (u32)bundle.totalIndices,
        .skins      = (u32)bundle.numSkins,
        .animations = (u32)bundle.numAnimations
    };
    ImportDetailSetAnimationBoundsInfo(info, &bundle);
    FreeSceneBundle(&bundle);
    return true;
}

static bool EditorReadBundleInfo(const char* path, ImportDetailInfo* info)
{
    if (EditorReadBundleInfoFromScene(path, info)) return true;
    if (EditorReadBundleInfoFromGLTF(path, info)) return true;
    return EditorReadBundleInfoFromABM(path, info);
}

static bool EditorImportNeedsDetailWarning(const char* normalizedPath)
{
    ImportDetailInfo info;
    if (!EditorReadBundleInfo(normalizedPath, &info)) return false;
    return info.animBoundsWarning;
}

// todo remove this
void EditorOpenImportDetail(const char* path)
{
    char normalized[512];
    NormalizePath(path, normalized, sizeof(normalized));
    ImportDetailInfo info;
    if (!EditorReadBundleInfo(normalized, &info))
    {
        AX_ERROR("mesh load failed: %s", normalized);
        return;
    }
    MemCopy(importDetailPath, normalized, StringLength(normalized) + 1);
    importDetailInfo = info;
    ImportDetailSetScaleText(info.animBoundsWarning ? info.recommendedScale : 1.0f);
    importDetailEuler[0] = 0.0f;
    importDetailEuler[1] = 0.0f;
    importDetailEuler[2] = 0.0f;
    importDetailOpen = true;
}

static void SceneImportDetailPopup(void)
{
    if (!importDetailOpen)
        return;

    float2 windowSize = importDetailInfo.animBoundsWarning ? (float2){ 720.0f, 780.0f } : (float2){ 620.0f, 680.0f };
    float2 center = {
        Maxf32(10.0f, g_WindowState.prev_width * 0.5f - windowSize.x * 0.5f),
        Maxf32(10.0f, g_WindowState.prev_height * 0.5f - windowSize.y * 0.5f)
    };
    if (!UIBeginWindow("Import Mesh", center, windowSize, &importDetailOpen, 0)) return;

    bool skinned = importDetailInfo.skins > 0u;

    CLAY_TEXT(UIStr(GetFileName(importDetailPath)), CLAY_TEXT_CONFIG({
        .fontSize = 16,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    UIDivider(CLAY_ID("ImportDetailDivider"));

    CLAY(CLAY_ID("ImportDetailInfo"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
            .padding = { 6, 0, 0, 0 },
            .childGap = 4,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        UITextU32("Meshes", importDetailInfo.meshes);
        UITextU32("Nodes", importDetailInfo.nodes);
        UITextU32("Materials", importDetailInfo.materials);
        UITextU32("Images", importDetailInfo.images);
        UITextU32("Vertices", importDetailInfo.vertices);
        UITextU32("Triangles", importDetailInfo.indices / 3u);
        UITextU32("Skins", importDetailInfo.skins);
        UITextU32("Animations", importDetailInfo.animations);
    }

    if (skinned && importDetailInfo.animations > 0u)
    {
        u32 defaultAnim = importDetailInfo.animations > 1u ? 1u : 0u;
        char* text = UIFrameStringAlloc(96u);
        if (text)
        {
            u32 len = (u32)StringLength("Default animation: ");
            MemCopy(text, "Default animation: ", len);
            len += (u32)IntToString(text + len, (int64_t)defaultAnim, 0);
            text[len] = '\0';
            CLAY_TEXT(((Clay_String) { .isStaticallyAllocated = false, .length = (s32)len, .chars = text }),
                      CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = UIGetClayColor(UIColor_SubText) }));
        }
    }

    if (importDetailInfo.animBoundsWarning)
    {
        char* text = UIFrameStringAlloc(384u);
        if (text)
        {
            u32 len = 0u;
            const char* a = "Warning: this object is bigger than usual ";
            u32 n = (u32)StringLength(a); MemCopy(text + len, a, n); len += n;
            len += (u32)FloatToString(text + len, ANIMATION_MAX_METERS, 3);
            const char* b = "m.\nit mesh reaches ";
            n = (u32)StringLength(b); MemCopy(text + len, b, n); len += n;
            len += (u32)FloatToString(text + len, importDetailInfo.maxSkinnedAnimMeters, 3);
            const char* c = "m\nRecommended import scale: ";
            n = (u32)StringLength(c); MemCopy(text + len, c, n); len += n;
            len += (u32)FloatToString(text + len, importDetailInfo.recommendedScale, 4);
            text[len] = '\0';
            CLAY_TEXT(((Clay_String) { .isStaticallyAllocated = false, .length = (s32)len, .chars = text }),
                      CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = UIColorToClay(0xFF30B0FFu), .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
    }

    static UITextAreaCustomData scaleData;
    scaleData.type = UICustomType_TextArea;
    scaleData.buffer = importDetailScaleText;
    scaleData.capacity = sizeof(importDetailScaleText);
    scaleData.flags = UITextAreaFlags_CenterX | UITextAreaFlags_CenterY | UITextAreaFlags_NoWrap | UITextAreaFlags_Clip;
    CLAY(CLAY_ID("ImportDetailScaleRow"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(CLAY_STRING("Scale"), CLAY_TEXT_CONFIG({ .fontSize = 15, .textColor = UIGetClayColor(UIColor_Text) }));
        CLAY(CLAY_ID_LOCAL("Spacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}
        CLAY(CLAY_ID_LOCAL("Text"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(110.0f), CLAY_SIZING_FIXED(28.0f) } },
            .custom = { .customData = &scaleData }
        }) {}
    }

    UIEditFloatN(CLAY_ID("ImportDetailEuler"), CLAY_STRING("Euler"), importDetailEuler, 3u, -360.0f, 360.0f, 3);

    CLAY(CLAY_ID("ImportDetailButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("ImportDetailOk"), CLAY_STRING("Import"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            ParseFloat(importDetailScaleText, &importDetailScale);
            Scene* scene = Scene_GetActive();
            if (!scene) scene = EditorNewScene();
            u32 bundleIdx = Scene_AddBundleAuto(scene, importDetailPath);
            if (bundleIdx != INVALID_BUNDLE)
            {
                v128f rotation = VecNorm(QFromEuler(importDetailEuler[0] * MATH_DegToRad,
                                                    importDetailEuler[1] * MATH_DegToRad,
                                                    importDetailEuler[2] * MATH_DegToRad));
                EditorSpawnBundleAt(scene, bundleIdx, VecZero(), rotation, VecSet1(importDetailScale));
            }
            else
                AX_ERROR("import to scene failed: %s", importDetailPath);
            importDetailOpen = false;
        }
        if (UIButton(CLAY_ID("ImportDetailCancel"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            importDetailOpen = false;
        }
    }
    UIEndWindow();
}


// click picking: ray casts the scene through the bundle blas data, logs the hit and
// outlines it. runs once per left click outside the ui
void EditorPickingUpdate(Camera* camera)
{
    if (!GetMousePressed(MouseButton_Left) || !EditorSceneInteractAllowed()) return;

    Scene* scene = Scene_GetActive();
    if (!scene) return;

    RayV ray = ScreenPointToRay(camera, EditorSceneMouse());
    BVHHit hit;
    if (!BVH_RaycastScene(scene, ray.origin, ray.dir, &hit))
    {
        // clicking empty space clears the selection unless adding with ctrl
        if (!GetKeyDown(SDLK_LCTRL)) EditorGizmoClear();
        return;
    }

    const RenderSet* set = hit.skinnedSet ? &scene->skinnedSet : &scene->surfaceSet;
    const PrimitiveGroup* group = &set->primitiveGroups[hit.groupIdx];
    const SceneBundleRef* ref = hit.bundleIdx < scene->numBundles ? &scene->bundleRefs[hit.bundleIdx] : NULL;
    const char* meshName = NULL;
    if (ref && group->meshIndex < (u32)ref->bundle->numMeshes)
        meshName = ref->bundle->meshes[group->meshIndex].name;

    if (ref)
    {
        sceneSelectedBundle = hit.bundleIdx;
        sceneSelectedNode = -1;
        SceneSelectObject(hit.skinnedSet, hit.groupIdx, hit.entityIdx, hit.bundleIdx);
    }
    // the gizmo submits the outlines of the whole selection every frame
    if (GetKeyDown(SDLK_LCTRL)) // ctrl click toggles the object in the multi selection
        EditorGizmoAddTarget(hit.skinnedSet, hit.groupIdx, hit.entityIdx);
    else
        EditorGizmoSetTarget(hit.skinnedSet, hit.groupIdx, hit.entityIdx);
}

void EditorSceneHotkeys(void)
{
    Scene* scene = Scene_GetActive();
    if (!scene) return;

    bool ctrl = GetKeyDown(SDLK_LCTRL) || GetKeyDown(SDLK_RCTRL);
    if (ctrl && GetKeyPressed('d'))
    {
        if (EditorGizmoDuplicateSelected())
            sceneObjectSelection.valid = false;
    }

    if (GetKeyPressed(SDLK_DELETE) && (!UIAnyWindowHovered() || EditorSceneInteractAllowed()))
    {
        if (EditorGizmoDeleteSelected())
        {
            sceneObjectSelection.valid = false;
            sceneSelectedBundle = INVALID_BUNDLE;
            sceneSelectedNode = -1;
        }
    }
}

static void EditorSaveSceneAs(const char* name)
{
    Scene* scene = Scene_GetActive();
    if (!scene || name[0] == '\0') return;

    CreateFolder(EDITOR_SCENE_FOLDER);
    char path[512];
    MemsetZero(path, sizeof(path));
    if (!CombinePaths(path, sizeof(path), EDITOR_SCENE_FOLDER, name)) return;
    int pathLen = StringLength(path);
    if (!FileHasExtension(path, pathLen, ".scene"))
    {
        if (pathLen + 7 > (int)sizeof(path)) return;
        MemCopy(path + pathLen, ".scene", 7);
    }
    if (Scene_SaveActiveAs(path))
    {
        EditorSettingsSetLastScene(Scene_GetActivePath());
        sceneFilesDirty = true;
    }
}

static void EditorSaveActiveScene(void)
{
    Scene* scene = Scene_GetActive();
    if (!scene || Scene_GetActivePath()[0] == '\0') return;
    if (Scene_SaveActive())
    {
        EditorSettingsSetLastScene(Scene_GetActivePath());
        sceneFilesDirty = true;
    }
}

static bool ScenePathsEqual(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void SceneFilesCollectFn(const char* path, void* data)
{
    (void)data;
    if (sceneNumFiles >= EDITOR_SCENE_MAX_FILES || IsFolder(path)) return;
    if (!FileHasExtension(path, StringLength(path), ".scene")) return;

    EditorSceneFile* file = &sceneFiles[sceneNumFiles++];
    NormalizePath(path, file->path, sizeof(file->path));
    file->nameOffset = 0u;
    for (u32 i = 0u; file->path[i]; i++)
        if (file->path[i] == '/' || file->path[i] == '\\') file->nameOffset = (u16)(i + 1u);
}

static void SceneFilesRefresh(void)
{
    sceneNumFiles = 0u;
    CreateFolder(EDITOR_SCENE_FOLDER);
    VisitFolder(EDITOR_SCENE_FOLDER, SceneFilesCollectFn, NULL, false);
    sceneFilesDirty = false;
    sceneFilesLastScan = TimeSinceStartup();
}

static void SceneDeleteWithDependencies(const char* path)
{
    for (u32 i = 0u; i < TextureClass_Count; i++)
    {
        char atlasPath[512];
        ChangeExtensionAndCopy(path, kEditorSceneAtlasSuffix[i], atlasPath, sizeof(atlasPath));
        if (atlasPath[0] && FileExist(atlasPath)) RemoveFile(atlasPath);
    }
    RemoveFile(path);
    if (ScenePathsEqual(path, Scene_GetActivePath())) EditorNewScene();
    sceneSelectedFile = -1;
    sceneFilesDirty = true;
}

static void SceneEventDeleteFile(void* unused)
{
    (void)unused;
    if (sceneSelectedFile < 0 || sceneSelectedFile >= (s32)sceneNumFiles) return;
    MemCopy(sceneDeletePath, sceneFiles[sceneSelectedFile].path, StringLength(sceneFiles[sceneSelectedFile].path) + 1);
    sceneDeletePopupOpen = true;
}

void EditorSceneStartup(void)
{
    if (!EditorSettingsOpenLastScene()) return;

    const char* path = EditorSettingsLastScene();
    if (path && path[0] && FileExist(path)) EditorOpenScene(path);
}

static void SceneFilesUI(void)
{
    if (sceneFilesDirty || TimeSinceStartup() - sceneFilesLastScan > 2.0f) SceneFilesRefresh();

    CLAY_TEXT(CLAY_STRING("Scenes"), CLAY_TEXT_CONFIG({
        .fontSize = 15,
        .textColor = UIGetClayColor(UIColor_Text)
    }));

    if (sceneNumFiles == 0u)
    {
        CLAY_TEXT(CLAY_STRING("No .scene files in Assets/Scenes."), CLAY_TEXT_CONFIG({
            .fontSize = 13,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        return;
    }

    for (u32 i = 0u; i < sceneNumFiles; i++)
    {
        EditorSceneFile* file = &sceneFiles[i];
        Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("SceneFileRow"), i);
        u32 flags = UITreeNodeFlags_Leaf;
        if ((s32)i == sceneSelectedFile || ScenePathsEqual(file->path, Scene_GetActivePath())) flags |= UITreeNodeFlags_Selected;

        bool selected = false;
        UITreeNode(id, UIStr(file->path + file->nameOffset), 0u, flags, false, &selected);
        if (selected)
        {
            u32 clickHash = StringToHash(file->path, 5381u);
            f32 now = TimeSinceStartup();
            bool doubleClicked = clickHash == sceneLastClickHash && now - sceneLastClickTime < EDITOR_SCENE_DOUBLE_CLICK_SECONDS;
            sceneLastClickHash = clickHash;
            sceneLastClickTime = doubleClicked ? -10.0f : now;
            sceneSelectedFile = (s32)i;
            if (doubleClicked) EditorOpenScene(file->path);
        }
        if (SceneRowRightClicked(id)) sceneSelectedFile = (s32)i;
    }
}

// caller owned tree expansion state, node bits start closed,
// bundle flags are stored inverted so bundle rows start open
#define SCENE_TREE_MAX_NODES 16000
static u32 sceneNodeOpenBits[MAX_SCENE_BUNDLES][SCENE_TREE_MAX_NODES / 32];
static u8  sceneBundleClosed[MAX_SCENE_BUNDLES];

static bool SceneNodeIsOpen(u32 bundleIdx, u32 nodeIdx)
{
    if (nodeIdx >= SCENE_TREE_MAX_NODES) return false;
    return (sceneNodeOpenBits[bundleIdx][nodeIdx >> 5u] >> (nodeIdx & 31u)) & 1u;
}

static void SceneNodeFlipOpen(u32 bundleIdx, u32 nodeIdx)
{
    if (nodeIdx >= SCENE_TREE_MAX_NODES) return;
    sceneNodeOpenBits[bundleIdx][nodeIdx >> 5u] ^= 1u << (nodeIdx & 31u);
}

static Clay_String SceneNodeLabel(const ANode* node, s32 nodeIdx)
{
    if (node->name && node->name[0]) return UIStr(node->name);

    char* text = UIFrameStringAlloc(20u);
    if (!text) return CLAY_STRING("Node");
    u32 len = 5u;
    MemCopy(text, "Node ", len);
    len += (u32)IntToString(text + len, (int64_t)nodeIdx, 0);
    text[len] = '\0';
    return (Clay_String) { .isStaticallyAllocated = false, .length = (s32)len, .chars = text };
}

// selecting a node in the tree selects its whole subtree in the viewport: the node and
// every descendant mesh node of the same spawned instance become gizmo targets
static void SceneSelectNodeInViewport(const Scene* scene, u32 bundleIdx, s32 nodeIdx)
{
    if (bundleIdx >= scene->numBundles || !scene->bundleRefs[bundleIdx].bundle) return;
    const SceneBundleRef* ref = &scene->bundleRefs[bundleIdx];
    const SceneBundle* bundle = ref->bundle;
    if (nodeIdx < 0 || nodeIdx >= bundle->numNodes) return;

    const RenderSet* set = ref->skinned ? &scene->skinnedSet : &scene->surfaceSet;
    if (ref->renderIdx >= set->numBundles) return;
    Range range = set->bundlePrimRange[ref->renderIdx];

    // gather the subtree's mesh nodes (including the picked one)
    enum { MaxSubtree = 2048 };
    s32 meshNodes[MaxSubtree];
    u32 numMeshNodes = 0;
    s32 stack[MaxSubtree];
    u32 stackCount = 1;
    stack[0] = nodeIdx;
    while (stackCount > 0)
    {
        s32 current = stack[--stackCount];
        if (current < 0 || current >= bundle->numNodes) continue;
        const ANode* node = &bundle->nodes[current];
        if (node->type == 0 && node->index >= 0 && numMeshNodes < MaxSubtree)
            meshNodes[numMeshNodes++] = current;
        for (s32 c = 0; c < node->numChildren && stackCount < MaxSubtree; c++)
            stack[stackCount++] = node->children[c];
    }
    if (numMeshNodes == 0) return;

    // skinned spawns share one sparse id, one target already covers the character
    if (ref->skinned)
    {
        for (u32 i = 0; i < numMeshNodes; i++)
        {
            const ANode* node = &bundle->nodes[meshNodes[i]];
            for (u32 g = range.start; g < range.start + range.count; g++)
            {
                const PrimitiveGroup* group = &set->primitiveGroups[g];
                if (group->meshIndex == (u32)node->index && group->numEntities > 0)
                {
                    EditorGizmoSetTarget(1, g, 0);
                    return;
                }
            }
        }
        return;
    }

    // base entity: first instance of the first mesh node, the rest of the subtree
    // resolves through the sparse id ordinal arithmetic
    u32 baseSparse = INVALID_ENTITY;
    s32 baseOrdinal = RenderSet_NodeSpawnOrdinal(bundle, meshNodes[0]);
    for (u32 g = range.start; g < range.start + range.count && baseSparse == INVALID_ENTITY; g++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[g];
        if (group->meshIndex == (u32)bundle->nodes[meshNodes[0]].index && group->numEntities > 0)
            baseSparse = set->entities[group->entityOffset].sparseIdx;
    }
    if (baseSparse == INVALID_ENTITY || baseOrdinal < 0) return;

    bool first = true;
    for (u32 i = 0; i < numMeshNodes; i++)
    {
        s32 ordinal = RenderSet_NodeSpawnOrdinal(bundle, meshNodes[i]);
        if (ordinal < 0) continue;
        u32 sparseIdx = baseSparse + (u32)(ordinal - baseOrdinal);
        u32 groupIdx, entityIdx;
        if (!RenderSet_FindNodeEntity(set, range, (u32)bundle->nodes[meshNodes[i]].index, sparseIdx, &groupIdx, &entityIdx))
            continue;
        if (first)
        {
            EditorGizmoSetTarget(0, groupIdx, entityIdx);
            first = false;
        }
        else
            EditorGizmoAddTarget(0, groupIdx, entityIdx);
    }
}

// right click selects the hovered row so the context menu targets it, last frame's layout
static bool SceneRowRightClicked(Clay_ElementId id)
{
    if (!GetMousePressed(MouseButton_Right)) return false;
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found) return false;
    Clay_PointerData pointer = Clay_GetPointerState();
    return pointer.position.x >= data.boundingBox.x && pointer.position.x <= data.boundingBox.x + data.boundingBox.width &&
           pointer.position.y >= data.boundingBox.y && pointer.position.y <= data.boundingBox.y + data.boundingBox.height;
}

static void SceneNodeTree(const SceneBundle* bundle, u32 bundleIdx, s32 nodeIdx, u32 depth)
{
    if (sceneTreeRowBudget == 0u) return;
    sceneTreeRowBudget--;

    const ANode* node = &bundle->nodes[nodeIdx];
    Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("SceneTreeNode"), bundleIdx * 0x10000u + (u32)nodeIdx);

    u32 flags = 0u;
    if (node->numChildren <= 0) flags |= UITreeNodeFlags_Leaf;
    if (bundleIdx == sceneSelectedBundle && nodeIdx == sceneSelectedNode) flags |= UITreeNodeFlags_Selected;

    bool open = SceneNodeIsOpen(bundleIdx, (u32)nodeIdx);
    bool selected = false;
    if (UITreeNode(id, SceneNodeLabel(node, nodeIdx), depth, flags, open, &selected))
    {
        SceneNodeFlipOpen(bundleIdx, (u32)nodeIdx);
        open = !open;
    }
    if (selected || SceneRowRightClicked(id))
    {
        sceneSelectedBundle = bundleIdx;
        sceneSelectedNode = nodeIdx;
        if (selected) SceneSelectNodeInViewport(Scene_GetActive(), bundleIdx, nodeIdx);
    }
    if (!open) return;

    for (s32 c = 0; c < node->numChildren; c++)
    {
        s32 child = node->children[c];
        if (child >= 0 && child < bundle->numNodes)
            SceneNodeTree(bundle, bundleIdx, child, depth + 1u);
    }
}

static void SceneBundleTree(const Scene* scene, u32 bundleIdx)
{
    const SceneBundle* bundle = scene->bundleRefs[bundleIdx].bundle;
    Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("SceneTreeBundle"), bundleIdx);

    u32 flags = 0u;
    if (!bundle || bundle->numNodes <= 0) flags |= UITreeNodeFlags_Leaf;
    if (bundleIdx == sceneSelectedBundle && sceneSelectedNode < 0) flags |= UITreeNodeFlags_Selected;

    bool open = !sceneBundleClosed[bundleIdx];
    bool selected = false;
    if (UITreeNode(id, UIStr(GetFileName(scene->bundleRefs[bundleIdx].path)), 0u, flags, open, &selected))
    {
        sceneBundleClosed[bundleIdx] ^= 1u;
        open = !open;
    }
    if (selected || SceneRowRightClicked(id))
    {
        sceneSelectedBundle = bundleIdx;
        sceneSelectedNode = -1;
    }
    if (!open || !bundle) return;

    // normalized bundles have parent indices built, roots are the unparented nodes
    for (s32 i = 0; i < bundle->numNodes; i++)
        if (bundle->nodes[i].parent < 0)
            SceneNodeTree(bundle, bundleIdx, i, 1u);
}

//------------------------------------------------------------------------
// Scene tree context menu

// duplicates the selected bundle's instance using its first entity's transform
// (root node world, the spawn transform for typical assets), offset to the side
static void SceneEventDuplicateBundle(void* unused)
{
    (void)unused;
    Scene* scene = Scene_GetActive();
    if (!scene || sceneSelectedBundle >= scene->numBundles || !scene->bundleRefs[sceneSelectedBundle].bundle) return;

    const SceneBundleRef* ref = &scene->bundleRefs[sceneSelectedBundle];
    const RenderSet* set = ref->skinned ? &scene->skinnedSet : &scene->surfaceSet;

    v128f position = VecZero();
    v128f rotation = QIdentity();
    v128f scale    = VecSet1(1.0f);
    if (ref->renderIdx < set->numBundles)
    {
        Range range = set->bundlePrimRange[ref->renderIdx];
        for (u32 g = range.start; g < range.start + range.count; g++)
        {
            const PrimitiveGroup* group = &set->primitiveGroups[g];
            if (group->numEntities == 0u) continue;
            const Entity* entity = &set->entities[group->entityOffset];
            position = entity->position;
            rotation = UnpackQuaternionS16Norm1(entity->rotation);
            scale    = EntityUnpackScale01(entity->scale);
            break;
        }
    }
    position = VecAdd(position, VecSetR(1.0f, 0.0f, 0.0f, 0.0f));
    EditorSpawnBundleAt(scene, sceneSelectedBundle, position, rotation, scale);
}

static void SceneEventDeleteBundle(void* unused)
{
    (void)unused;
    Scene* scene = Scene_GetActive();
    if (!scene || sceneSelectedBundle >= scene->numBundles) return;
    Scene_RemoveBundle(scene, sceneSelectedBundle);
    sceneSelectedBundle = INVALID_BUNDLE;
    sceneSelectedNode = -1;
    sceneObjectSelection.valid = false;
    EditorGizmoClear(); // group indices shifted
}

static bool SceneResolveSelectedObject(Scene* scene, RenderSet** outSet, PrimitiveGroup** outGroup, Entity** outEntity)
{
    if (!scene || !sceneObjectSelection.valid) return false;
    RenderSet* set = sceneObjectSelection.skinned ? &scene->skinnedSet : &scene->surfaceSet;
    *outSet = set;
    return RenderSet_ResolveEntity(set, sceneObjectSelection.groupIdx, sceneObjectSelection.entityIdx, outGroup, outEntity);
}

static void SceneDeleteSelectedObject(void)
{
    Scene* scene = Scene_GetActive();
    RenderSet* set;
    PrimitiveGroup* group;
    Entity* entity;
    if (!SceneResolveSelectedObject(scene, &set, &group, &entity)) return;
    (void)group;
    (void)entity;

    RenderSet_RemoveEntity(set, sceneObjectSelection.groupIdx, sceneObjectSelection.entityIdx);
    scene->renderDataDirty = 1;
    sceneObjectSelection.valid = false;
    EditorGizmoClear();
}

// removes the selected node's mesh entities from the render set, every spawned
// instance of the bundle loses that node. the tree row stays, node hierarchies are
// shared bundle data
static void SceneEventDeleteNode(void* unused)
{
    (void)unused;
    Scene* scene = Scene_GetActive();
    if (!scene || sceneSelectedBundle >= scene->numBundles || sceneSelectedNode < 0
        || !scene->bundleRefs[sceneSelectedBundle].bundle) return;

    const SceneBundleRef* ref = &scene->bundleRefs[sceneSelectedBundle];
    const SceneBundle* bundle = ref->bundle;
    if (sceneSelectedNode >= bundle->numNodes) return;
    const ANode* node = &bundle->nodes[sceneSelectedNode];
    if (node->type != 0 || node->index < 0)
    {
        AX_LOG("node has no mesh to remove: %d", sceneSelectedNode);
        return;
    }

    RenderSet* set = ref->skinned ? &scene->skinnedSet : &scene->surfaceSet;
    if (ref->renderIdx >= set->numBundles) return;

    u32 removed = 0;
    Range range = set->bundlePrimRange[ref->renderIdx];
    for (u32 g = range.start; g < range.start + range.count; g++)
    {
        const PrimitiveGroup* group = &set->primitiveGroups[g];
        if (group->meshIndex != (u32)node->index || group->numEntities == 0u) continue;
        removed += RenderSet_RemoveEntities(set, g, 0, group->numEntities);
    }
    scene->renderDataDirty = 1;
    sceneObjectSelection.valid = false;
    EditorGizmoClear(); // entity indices shifted
    AX_LOG("removed %d entities of node %d", removed, sceneSelectedNode);
}

static v128f SceneEntityWorldScale(const Entity* entity)
{
    return EntityUnpackWorldScale(entity->scale);
}

typedef struct SceneInspectorCache_
{
    bool valid;
    u32 skinned;
    u32 groupIdx;
    u32 entityIdx;
    v128f position;
    u64 rotation;
    u64 scalePacked;
    f32 positionUi[3];
    s32 rotationUi[3];
    f32 scaleUi[3];
} SceneInspectorCache;

static SceneInspectorCache sceneInspectorCache;

static void SceneInspectorRefreshCache(const Entity* entity)
{
    sceneInspectorCache.valid = true;
    sceneInspectorCache.skinned = sceneObjectSelection.skinned;
    sceneInspectorCache.groupIdx = sceneObjectSelection.groupIdx;
    sceneInspectorCache.entityIdx = sceneObjectSelection.entityIdx;
    sceneInspectorCache.position = entity->position;
    sceneInspectorCache.rotation = entity->rotation;
    sceneInspectorCache.scalePacked = entity->scale;

    sceneInspectorCache.positionUi[0] = VecGetX(entity->position);
    sceneInspectorCache.positionUi[1] = VecGetY(entity->position);
    sceneInspectorCache.positionUi[2] = VecGetZ(entity->position);

    float3 euler = QToEulerAngles(VecNorm(UnpackQuaternionS16Norm1(entity->rotation)));
    sceneInspectorCache.rotationUi[0] = Clamps32((s32)(euler.x * MATH_RadToDeg + (euler.x >= 0.0f ? 0.5f : -0.5f)), -180, 180);
    sceneInspectorCache.rotationUi[1] = Clamps32((s32)(euler.y * MATH_RadToDeg + (euler.y >= 0.0f ? 0.5f : -0.5f)), -180, 180);
    sceneInspectorCache.rotationUi[2] = Clamps32((s32)(euler.z * MATH_RadToDeg + (euler.z >= 0.0f ? 0.5f : -0.5f)), -180, 180);

    v128f scaleV = EntityUnpackWorldScale(entity->scale);
    sceneInspectorCache.scaleUi[0] = VecGetX(scaleV);
    sceneInspectorCache.scaleUi[1] = VecGetY(scaleV);
    sceneInspectorCache.scaleUi[2] = VecGetZ(scaleV);
}

static void SceneInspectorValidateCache(const Entity* entity)
{
    if (!sceneInspectorCache.valid ||
        sceneInspectorCache.skinned != sceneObjectSelection.skinned ||
        sceneInspectorCache.groupIdx != sceneObjectSelection.groupIdx ||
        sceneInspectorCache.entityIdx != sceneObjectSelection.entityIdx)
    {
        SceneInspectorRefreshCache(entity);
        return;
    }

    if (VecGetX(sceneInspectorCache.position) != VecGetX(entity->position) ||
        VecGetY(sceneInspectorCache.position) != VecGetY(entity->position) ||
        VecGetZ(sceneInspectorCache.position) != VecGetZ(entity->position) ||
        sceneInspectorCache.rotation != entity->rotation ||
        sceneInspectorCache.scalePacked != entity->scale)
        SceneInspectorRefreshCache(entity);
}

// A dimmed "label  value" row for physics properties that are simulation output
// (mass, velocities, center of mass) and cannot be edited from the inspector.
static void UIPhysicsReadonlyRow(Clay_ElementId id, const char* label, const char* value)
{
	CLAY(id, {
		.layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24.0f) },
			.childGap = 8,
			.layoutDirection = CLAY_LEFT_TO_RIGHT,
			.childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
		}
	}) {
		CLAY_TEXT(UIStr(label), CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = UIGetClayColor(UIColor_Text) }));
		// element spacer, never a whitespace-only CLAY_TEXT (that corrupts the text batch)
		CLAY(CLAY_ID_LOCAL("Spacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}
		CLAY_TEXT(UIStr(value), CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = UIGetClayColor(UIColor_SubText) }));
	}
}

// A per-axis lock row: a leading label followed by three checkboxes (X/Y/Z)
// laid out horizontally rather than stacked. Returns true when any axis toggled.
static bool UIPhysicsLockRow(Clay_ElementId rowId, Clay_String label,
                             Clay_ElementId idX, bool* x,
                             Clay_ElementId idY, bool* y,
                             Clay_ElementId idZ, bool* z)
{
	bool changed = false;
	CLAY(rowId, {
		.layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28.0f) },
			.childGap = 6,
			.layoutDirection = CLAY_LEFT_TO_RIGHT,
			.childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
		}
	}) {
		CLAY(CLAY_ID_LOCAL("Label"), {
			.layout = {
				.sizing = { CLAY_SIZING_FIXED(84.0f), CLAY_SIZING_GROW(0) },
				.childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
			}
		}) {
			CLAY_TEXT(label, CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = UIGetClayColor(UIColor_SubText) }));
		}
		changed |= UICheckbox(idX, CLAY_STRING("X"), x);
		changed |= UICheckbox(idY, CLAY_STRING("Y"), y);
		changed |= UICheckbox(idZ, CLAY_STRING("Z"), z);
	}
	return changed;
}

// formats into a per-frame string; safe to pass straight to UIPhysicsReadonlyRow
static const char* UIPhysicsFmtF(f32 v, int decimals)
{
	char* s = UIFrameStringAlloc(32u);
	if (!s) return "";
	SDL_snprintf(s, 32u, "%.*f", decimals, v);
	return s;
}

static const char* UIPhysicsFmtV3(f32 x, f32 y, f32 z, int decimals)
{
	char* s = UIFrameStringAlloc(96u);
	if (!s) return "";
	SDL_snprintf(s, 96u, "%.*f, %.*f, %.*f", decimals, x, decimals, y, decimals, z);
	return s;
}

// Visualizes and edits the box3d body attached to the selected entity.
// Simulation-output values (mass, velocities, center of mass) are read-only;
// body type / dynamics / material / lock widgets push their changes back through
// the box3d setters. Note the underlying collider is a static mesh, so a body
// type change is runtime-only and is reset to static on the next collider build.
static void SceneInspectorPhysicsUI(Scene* scene, const Entity* entity)
{
	if (sceneObjectSelection.skinned) return;
	if (!scene->surfacePhysicsBodies || entity->sparseIdx >= scene->surfaceSet.maxEntities) return;

	b3BodyId body = scene->surfacePhysicsBodies[entity->sparseIdx];
	if (B3_IS_NULL(body)) return;

	UIDivider(CLAY_ID("InspectorPhysicsDivider"));
	scenePhysicsOpen ^= UICollapsingHeader(CLAY_ID("InspectorPhysicsHeader"), CLAY_STRING("Physics Body"), scenePhysicsOpen);
	if (!scenePhysicsOpen) return;

	b3ShapeId shapeId;
	bool hasShape = b3Body_GetShapes(body, &shapeId, 1) > 0;

	// Body type is editable. Order matches b3BodyType (static, kinematic, dynamic).
	static const char* bodyTypes[] = { "Static", "Kinematic", "Dynamic" };
	u32 bodyType = (u32)b3Body_GetType(body);
	if (bodyType >= ARRAY_SIZE(bodyTypes)) bodyType = 0u;
	if (UIDropdown(CLAY_ID("InspectorPhysicsBodyType"), CLAY_STRING("Body Type"),
	               bodyTypes, ARRAY_SIZE(bodyTypes), &bodyType))
	{
		b3Body_SetType(body, (b3BodyType)bodyType);
		if (bodyType == (u32)b3_dynamicBody && hasShape && b3Body_GetMass(body) <= 0.0f)
			Scene_PhysicsApplyDefaultDynamicMass(body, shapeId);
	}

	// Shape type is editable. Order matches b3ShapeType so the index maps directly.
	// Mesh restores the original triangle collider; compound/height are no-ops.
	if (hasShape)
	{
		static const char* shapeTypes[] = { "Capsule", "Compound", "Height", "Hull", "Mesh", "Sphere" };
		u32 shapeType = (u32)b3Shape_GetType(shapeId);
		if (shapeType >= ARRAY_SIZE(shapeTypes)) shapeType = 0u;
		if (UIDropdown(CLAY_ID("InspectorPhysicsShape"), CLAY_STRING("Shape"),
		               shapeTypes, ARRAY_SIZE(shapeTypes), &shapeType))
			Scene_PhysicsSetEntityShape(scene, false, sceneObjectSelection.groupIdx, entity, (b3ShapeType)shapeType);
	}

	// Mass properties and velocities are simulation output -> read-only.
	UISectionHeader("State");
	UIPhysicsReadonlyRow(CLAY_ID("InspectorPhysicsMass"), "Mass", UIPhysicsFmtF(b3Body_GetMass(body), 3));

	b3Pos com = b3Body_GetWorldCenterOfMass(body);
	UIPhysicsReadonlyRow(CLAY_ID("InspectorPhysicsCom"), "Center of Mass",
	                     UIPhysicsFmtV3((f32)com.x, (f32)com.y, (f32)com.z, 3));

	b3Vec3 linVel = b3Body_GetLinearVelocity(body);
	UIPhysicsReadonlyRow(CLAY_ID("InspectorPhysicsLinVel"), "Linear Velocity",
	                     UIPhysicsFmtV3(linVel.x, linVel.y, linVel.z, 3));

	b3Vec3 angVel = b3Body_GetAngularVelocity(body);
	UIPhysicsReadonlyRow(CLAY_ID("InspectorPhysicsAngVel"), "Angular Velocity",
	                     UIPhysicsFmtV3(angVel.x, angVel.y, angVel.z, 3));

	// Integration scalars: each edit is pushed straight to the body.
	UISectionHeader("Dynamics");
	f32 linearDamping = b3Body_GetLinearDamping(body);
	if (UIEditFloat(CLAY_ID("InspectorPhysicsLinDamp"), CLAY_STRING("Linear Damping"), &linearDamping, 0.0f, 100.0f, 0.01f, 3))
		b3Body_SetLinearDamping(body, linearDamping);

	f32 angularDamping = b3Body_GetAngularDamping(body);
	if (UIEditFloat(CLAY_ID("InspectorPhysicsAngDamp"), CLAY_STRING("Angular Damping"), &angularDamping, 0.0f, 100.0f, 0.01f, 3))
		b3Body_SetAngularDamping(body, angularDamping);

	f32 gravityScale = b3Body_GetGravityScale(body);
	if (UIEditFloat(CLAY_ID("InspectorPhysicsGravity"), CLAY_STRING("Gravity Scale"), &gravityScale, -10.0f, 10.0f, 0.1f, 3))
		b3Body_SetGravityScale(body, gravityScale);

	f32 sleepThreshold = b3Body_GetSleepThreshold(body);
	if (UIEditFloat(CLAY_ID("InspectorPhysicsSleep"), CLAY_STRING("Sleep Threshold"), &sleepThreshold, 0.0f, 100.0f, 0.01f, 3))
		b3Body_SetSleepThreshold(body, sleepThreshold);

	// Surface material of the primary shape.
	if (hasShape)
	{
		UISectionHeader("Material");
		f32 friction = b3Shape_GetFriction(shapeId);
		if (UIEditFloat(CLAY_ID("InspectorPhysicsFriction"), CLAY_STRING("Friction"), &friction, 0.0f, 1.0f, 0.01f, 3))
			b3Shape_SetFriction(shapeId, friction);

		f32 restitution = b3Shape_GetRestitution(shapeId);
		if (UIEditFloat(CLAY_ID("InspectorPhysicsRestitution"), CLAY_STRING("Restitution"), &restitution, 0.0f, 1.0f, 0.01f, 3))
			b3Shape_SetRestitution(shapeId, restitution);

		f32 density = b3Shape_GetDensity(shapeId);
		// updateBodyMass = true so mass properties recompute (matters once bodies are dynamic).
		if (UIEditFloat(CLAY_ID("InspectorPhysicsDensity"), CLAY_STRING("Density"), &density, 0.0f, 100000.0f, 0.1f, 3))
			b3Shape_SetDensity(shapeId, density, true);
	}

	// Motion locks, collapsible; each axis triple is laid out horizontally.
	scenePhysicsLocksOpen ^= UICollapsingHeader(CLAY_ID("InspectorPhysicsLocksHeader"), CLAY_STRING("Motion Locks"), scenePhysicsLocksOpen);
	if (scenePhysicsLocksOpen)
	{
		b3MotionLocks locks = b3Body_GetMotionLocks(body);
		bool locksChanged = UIPhysicsLockRow(CLAY_ID("InspectorPhysLockLinRow"), CLAY_STRING("Position"),
		                 CLAY_ID("InspectorPhysLockLinX"), &locks.linearX,
		                 CLAY_ID("InspectorPhysLockLinY"), &locks.linearY,
		                 CLAY_ID("InspectorPhysLockLinZ"), &locks.linearZ);
		locksChanged |= UIPhysicsLockRow(CLAY_ID("InspectorPhysLockAngRow"), CLAY_STRING("Rotation"),
		                 CLAY_ID("InspectorPhysLockAngX"), &locks.angularX,
		                 CLAY_ID("InspectorPhysLockAngY"), &locks.angularY,
		                 CLAY_ID("InspectorPhysLockAngZ"), &locks.angularZ);
		if (locksChanged)
			b3Body_SetMotionLocks(body, locks);
	}
}

static void SceneInspectorUI(Scene* scene)
{
	sceneInspectorOpen ^= UICollapsingHeader(CLAY_ID("SceneInspectorHeader"), CLAY_STRING("Inspector"), sceneInspectorOpen);
	if (!sceneInspectorOpen) return;

	RenderSet* set;
	PrimitiveGroup* group;
	Entity* entity;
	if (!SceneResolveSelectedObject(scene, &set, &group, &entity))
	{
		sceneObjectSelection.valid = false;
		sceneInspectorCache.valid = false;
		CLAY_TEXT(CLAY_STRING("No object selected."), CLAY_TEXT_CONFIG( {
			.fontSize = 13,
			.textColor = UIGetClayColor(UIColor_SubText)
		}));
		return;
	}

	char* idText = UIFrameStringAlloc(96u);
	if (idText)
	{
		s32 len = SDL_snprintf(idText, 96u, "Bundle %u Group %u Entity %u", sceneObjectSelection.bundleIdx, sceneObjectSelection.groupIdx, sceneObjectSelection.entityIdx);
		if (len < 0) len = 0;
		CLAY_TEXT(((Clay_String) { .isStaticallyAllocated = false, .length = len, .chars = idText }), CLAY_TEXT_CONFIG( {
			.fontSize = 13,
			.textColor = UIGetClayColor(UIColor_SubText)
		}));
	}

	SceneInspectorValidateCache(entity);

	if (UIEditFloatN(CLAY_ID("InspectorPosition"), CLAY_STRING("Position"), sceneInspectorCache.positionUi, 3u, -100000.0f, 100000.0f, 3))
	{
		entity->position = VecSetR(sceneInspectorCache.positionUi[0], sceneInspectorCache.positionUi[1], sceneInspectorCache.positionUi[2], 0.0f);
		sceneInspectorCache.position = entity->position;
		if (!sceneObjectSelection.skinned)
			Scene_PhysicsSyncEntityBody(scene, false, sceneObjectSelection.groupIdx, entity);
		scene->renderDataDirty = 1;
	}

	if (UIEditIntN(CLAY_ID("InspectorRotation"), CLAY_STRING("Rotation"), sceneInspectorCache.rotationUi, 3u, -180, 180))
	{
		v128f oldRot = VecNorm(UnpackQuaternionS16Norm1(entity->rotation));
		v128f oldScale = SceneEntityWorldScale(entity);
		v128f center = RenderSet_EntityBoundsCenter(group, entity, oldRot, oldScale);
		v128f q = VecNorm(QFromEuler((f32)sceneInspectorCache.rotationUi[0] * MATH_DegToRad,
									 (f32)sceneInspectorCache.rotationUi[1] * MATH_DegToRad,
									 (f32)sceneInspectorCache.rotationUi[2] * MATH_DegToRad));
		PackQuaternionS16Norm(q, &entity->rotation);
		entity->position = VecSub(center, QMulVec3V(VecMul(RenderSet_GroupLocalCenter(group), oldScale), q));
		sceneInspectorCache.position = entity->position;
		sceneInspectorCache.positionUi[0] = VecGetX(entity->position);
		sceneInspectorCache.positionUi[1] = VecGetY(entity->position);
		sceneInspectorCache.positionUi[2] = VecGetZ(entity->position);
		sceneInspectorCache.rotation = entity->rotation;
		if (!sceneObjectSelection.skinned)
			Scene_PhysicsSyncEntityBody(scene, false, sceneObjectSelection.groupIdx, entity);
		scene->renderDataDirty = 1;
	}

	if (UIEditFloatN(CLAY_ID("InspectorScale"), CLAY_STRING("Scale"), sceneInspectorCache.scaleUi, 3u, 0.001f, 10.0f, 3))
	{
		v128f rotation = VecNorm(UnpackQuaternionS16Norm1(entity->rotation));
		v128f oldScale = SceneEntityWorldScale(entity);
		v128f center = RenderSet_EntityBoundsCenter(group, entity, rotation, oldScale);
		entity->scale = EntityPackWorldScale(Vec3Load(sceneInspectorCache.scaleUi));
		v128f newScale = SceneEntityWorldScale(entity);
		entity->position = VecSub(center, QMulVec3V(VecMul(RenderSet_GroupLocalCenter(group), newScale), rotation));
		sceneInspectorCache.position = entity->position;
		sceneInspectorCache.positionUi[0] = VecGetX(entity->position);
		sceneInspectorCache.positionUi[1] = VecGetY(entity->position);
		sceneInspectorCache.positionUi[2] = VecGetZ(entity->position);
		sceneInspectorCache.scalePacked = entity->scale;
		if (!sceneObjectSelection.skinned)
			Scene_PhysicsSyncEntityBody(scene, false, sceneObjectSelection.groupIdx, entity);
		scene->renderDataDirty = 1;
	}

	SceneInspectorPhysicsUI(scene, entity);

    if (!sceneObjectSelection.skinned) return;
    if (sceneObjectSelection.bundleIdx >= scene->numBundles) return;
    const SceneBundleRef* ref = &scene->bundleRefs[sceneObjectSelection.bundleIdx];
    const SceneBundle* bundle = ref->bundle;
    if (!bundle || bundle->numAnimations <= 0) return;

    UIDivider(CLAY_ID("InspectorAnimDivider"));
    u32 animLocal = sceneObjectSelection.animIdx;
    u32 numOptions = Minu32((u32)bundle->numAnimations, 64u);
    const char* options[64];
    for (u32 i = 0u; i < numOptions; i++)
        options[i] = bundle->animations[i].name && bundle->animations[i].name[0] ? bundle->animations[i].name : "Animation";
    if (animLocal >= numOptions) animLocal = 0u;

    bool changed = UIDropdown(CLAY_ID("InspectorAnimation"), CLAY_STRING("Animation"), options, numOptions, &animLocal);
    changed |= UISliderFloatValue(CLAY_ID("InspectorAnimationTime"), CLAY_STRING("Time offset"), &sceneObjectSelection.animTime, 0.0f, 10.0f, 2);
    if (changed)
    {
        sceneObjectSelection.animIdx = animLocal;
        GPUAnimationInstance instance = { .animIdx = ref->animOffset + animLocal, .timeOffset = sceneObjectSelection.animTime };
        AnimationSystem_SetInstance(&scene->animSystem, entity->sparseIdx, instance);
    }
}

//------------------------------------------------------------------------
// Lights
static void SceneAddLight(Scene* scene, u32 type)
{
    if (scene->numLights >= MAX_SCENE_LIGHTS)
    {
        AX_WARN("maximum scene lights reached: %d", MAX_SCENE_LIGHTS);
        return;
    }
    LightGPU* light = &scene->lights[scene->numLights];
    MemsetZero(light, sizeof(*light));
    light->positionRadius[1] = 4.0f;
    light->positionRadius[3] = 12.0f;
    f32 directionCone[4] = { 0.0f, -1.0f, 0.0f, type == LightType_Spot ? 0.72f : 0.0f };
    f32 color[3] = { 1.0f, 1.0f, 1.0f };
    LightGPU_SetDirectionCone(light, directionCone);
    LightGPU_SetColor3(light, color);
    LightGPU_SetIntensity(light, 25.0f);
    light->type = type;
    light->flags = LIGHT_FLAG_SHADOWED;
    light->shadowIndex = LIGHT_SHADOW_INDEX_INVALID;
    sceneSelectedLight = (s32)scene->numLights++;
}

static bool SceneLightWorldToScreen(Camera* camera, const LightGPU* light, float2* outScreen, f32* outDepth)
{
    if (!camera || !light || camera->viewportSize.x <= 0 || camera->viewportSize.y <= 0) return false;

    mat4x4 viewProj = M44Multiply(camera->view, camera->projection);
    v128f world = VecSetR(light->positionRadius[0], light->positionRadius[1], light->positionRadius[2], 1.0f);
    v128f clip = Vec4Transform(world, viewProj.r);
    f32 w = VecGetW(clip);
    if (w <= 0.001f) return false;

    v128f ndc = VecDivf(clip, w);
    f32 x = (VecGetX(ndc) * 0.5f + 0.5f) * (f32)camera->viewportSize.x;
    f32 y = (0.5f - VecGetY(ndc) * 0.5f) * (f32)camera->viewportSize.y;
    if (x < -SCENE_LIGHT_GIZMO_RADIUS || y < -SCENE_LIGHT_GIZMO_RADIUS ||
        x > (f32)camera->viewportSize.x + SCENE_LIGHT_GIZMO_RADIUS ||
        y > (f32)camera->viewportSize.y + SCENE_LIGHT_GIZMO_RADIUS)
        return false;

    *outScreen = (float2){ x, y };
    if (outDepth)
    {
        v128f camPos = VecLoad(&camera->position.x);
        v128f normal = Vec3NormV(VecLoad(&camera->Front.x));
        *outDepth = Maxf32(Vec3DotfV(VecSub(world, camPos), normal), 0.001f);
    }
    return true;
}

static u32 SceneLightGizmoColor(const LightGPU* light)
{
    f32 color[3];
    LightGPU_GetColor3(light, color);
    return 0xCC000000u | PackColor3PtrToUint(color);
}

static bool SceneLightPlaneHit(Camera* camera, float2 mouse, f32 depth, v128f* outHit)
{
    RayV ray = ScreenPointToRay(camera, mouse);
    v128f camPos = VecLoad(&camera->position.x);
    v128f normal = Vec3NormV(VecLoad(&camera->Front.x));
    v128f planePoint = VecAdd(camPos, VecMulf(normal, depth));
    return RayPlaneHit(ray.origin, ray.dir, planePoint, normal, outHit);
}

bool EditorLightGizmoUpdate(Camera* camera)
{
    Scene* scene = Scene_GetActive();
    if (!scene || scene->numLights == 0u) return false;

    float2 mouse = EditorSceneMouse();
    f32 mx = mouse.x, my = mouse.y;

    if (sceneLightGizmoDragging)
    {
        if (!GetMouseDown(MouseButton_Left))
        {
            sceneLightGizmoDragging = false;
            return true;
        }
        if (sceneSelectedLight >= 0 && sceneSelectedLight < (s32)scene->numLights)
        {
            v128f hit;
            if (SceneLightPlaneHit(camera, mouse, sceneLightGizmoDepth, &hit))
            {
                LightGPU* light = &scene->lights[sceneSelectedLight];
                light->positionRadius[0] = VecGetX(hit);
                light->positionRadius[1] = VecGetY(hit);
                light->positionRadius[2] = VecGetZ(hit);
            }
        }
        return true;
    }

    if (!GetMousePressed(MouseButton_Left) || !EditorSceneInteractAllowed()) return false;

    for (s32 i = (s32)scene->numLights - 1; i >= 0; i--)
    {
        float2 screen;
        f32 depth;
        if (!SceneLightWorldToScreen(camera, &scene->lights[i], &screen, &depth)) continue;
        f32 dx = mx - screen.x;
        f32 dy = my - screen.y;
        if (dx * dx + dy * dy <= SCENE_LIGHT_GIZMO_RADIUS * SCENE_LIGHT_GIZMO_RADIUS)
        {
            sceneSelectedLight = i;
            sceneLightGizmoDragging = true;
            sceneLightGizmoDepth = depth;
            EditorGizmoClear();
            return true;
        }
    }
    return false;
}

void DrawSceneLightGizmos(Camera* camera)
{
    Scene* scene = Scene_GetActive();
    if (!scene) return;

    float2 origin = EditorSceneViewOrigin();
    bool sceneView = EditorSceneViewActive();
    for (u32 i = 0u; i < scene->numLights; i++)
    {
        float2 screen;
        if (!SceneLightWorldToScreen(camera, &scene->lights[i], &screen, NULL)) continue;
        screen = F2Add(screen, origin);
        // in scene view mode the icons draw after the window quads, skip the spots
        // where another window covers the view so they don't bleed over it
        if (sceneView && !EditorSceneViewPointVisible(screen)) continue;
        f32 radius = (s32)i == sceneSelectedLight ? SCENE_LIGHT_GIZMO_RADIUS + 3.0f : SCENE_LIGHT_GIZMO_RADIUS;
        UIPushCircle(screen, radius, (s32)i == sceneSelectedLight ? 0xFFFFFFFFu : 0xAA000000u);
        UIPushCircle(screen, SCENE_LIGHT_GIZMO_RADIUS - 2.0f, SceneLightGizmoColor(&scene->lights[i]));
    }
}

static void SceneRemoveLight(Scene* scene, u32 lightIdx)
{
    if (lightIdx >= scene->numLights) return;
    for (u32 i = lightIdx + 1; i < scene->numLights; i++)
        scene->lights[i - 1] = scene->lights[i];
    scene->numLights--;
    if (sceneSelectedLight >= (s32)scene->numLights) sceneSelectedLight = (s32)scene->numLights - 1;
    if (scene->numLights == 0u)
        RendererSetLights(NULL, 0u); // clear the renderer's copy, submit skips empty scenes
}

static Clay_String SceneLightLabel(const LightGPU* light, u32 lightIdx)
{
    const char* kind = light->type == LightType_Spot ? "Spot " : "Point ";
    u32 len = (u32)StringLength(kind);
    return (Clay_String) { .isStaticallyAllocated = true, .length = (s32)len, .chars = kind };
}

static void SceneLightsUI(Scene* scene)
{
    sceneLightsOpen ^= UICollapsingHeader(CLAY_ID("SceneLightsHeader"), CLAY_STRING("Lights"), sceneLightsOpen);
    if (!sceneLightsOpen) return;

    CLAY(CLAY_ID("SceneLightButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        UIPushFloatAdd(UIFloat_TextScale, -0.15f);
        if (UIButton(CLAY_ID("SceneAddPoint"), CLAY_STRING("Add Point"), (Clay_Dimensions){ 100.0f, 26.0f }, false))
            SceneAddLight(scene, LightType_Point);
        if (UIButton(CLAY_ID("SceneAddSpot"), CLAY_STRING("Add Spot"), (Clay_Dimensions){ 100.0f, 26.0f }, false))
            SceneAddLight(scene, LightType_Spot);
        if (sceneSelectedLight >= 0 && sceneSelectedLight < (s32)scene->numLights &&
            UIButton(CLAY_ID("SceneDeleteLight"), CLAY_STRING("Delete"), (Clay_Dimensions){ 90.0f, 26.0f }, false))
            SceneRemoveLight(scene, (u32)sceneSelectedLight);
        UIPopFloat(UIFloat_TextScale);
    }

    for (u32 i = 0u; i < scene->numLights; i++)
    {
        Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("SceneLightRow"), i);
        u32 flags = UITreeNodeFlags_Leaf;
        if ((s32)i == sceneSelectedLight) flags |= UITreeNodeFlags_Selected;
        bool selected = false;
        UITreeNode(id, SceneLightLabel(&scene->lights[i], i), 1u, flags, false, &selected);
        if (selected) sceneSelectedLight = (s32)i;
    }

    if (sceneSelectedLight < 0 || sceneSelectedLight >= (s32)scene->numLights) return;
    LightGPU* light = &scene->lights[sceneSelectedLight];

    UIEditFloatN(CLAY_ID("SceneLightPos"), CLAY_STRING("Position"), light->positionRadius, 3u, -10000.0f, 10000.0f, 2);
    UISliderFloatValue(CLAY_ID("SceneLightRadius"), CLAY_STRING("Radius"), &light->positionRadius[3], 0.1f, 200.0f, 1);
    if (light->type == LightType_Spot)
    {
        f32 directionCone[4];
        LightGPU_GetDirectionCone(light, directionCone);
        bool directionChanged = UIEditFloatN(CLAY_ID("SceneLightDir"), CLAY_STRING("Direction"), directionCone, 3u, -1.0f, 1.0f, 2);
        directionChanged |= UISliderFloatValue(CLAY_ID("SceneLightCone"), CLAY_STRING("Cone"), &directionCone[3], 0.0f, 0.99f, 2);
        if (directionChanged) LightGPU_SetDirectionCone(light, directionCone);
    }

    f32 color[3];
    LightGPU_GetColor3(light, color);
    if (UIColorEdit3(CLAY_ID("SceneLightColor"), CLAY_STRING("Color"), color))
        LightGPU_SetColor3(light, color);

    f32 intensity = LightGPU_GetIntensity(light);
    if (UISliderFloatValue(CLAY_ID("SceneLightIntensity"), CLAY_STRING("Intensity"), &intensity, 0.0f, 200.0f, 1))
        LightGPU_SetIntensity(light, intensity);

    bool shadowed = (light->flags & LIGHT_FLAG_SHADOWED) != 0u;
    UICheckbox(CLAY_ID("SceneLightShadowed"), CLAY_STRING("Shadowed"), &shadowed);
    light->flags = shadowed ? LIGHT_FLAG_SHADOWED : 0u;
}

//------------------------------------------------------------------------
// Save popup and window

static void SceneSavePopup(void)
{
    if (!sceneSavePopupOpen) return;
    float2 center = { g_WindowState.prev_width * 0.5f - 190.0f, g_WindowState.prev_height * 0.5f - 90.0f };
    if (!UIBeginWindow("Save Scene", center, (float2){ 380.0f, 190.0f }, &sceneSavePopupOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Name:"), CLAY_TEXT_CONFIG({
        .fontSize = 14,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    static UITextAreaCustomData nameData;
    nameData.type = UICustomType_TextArea;
    nameData.buffer = sceneSaveName;
    nameData.capacity = sizeof(sceneSaveName);
    nameData.flags = UITextAreaFlags_CenterY;
    CLAY(CLAY_ID("SceneSaveNameBox"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26.0f) } },
        .custom = { .customData = &nameData }
    }) {}

    CLAY(CLAY_ID("SceneSaveButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("SceneSaveOk"), CLAY_STRING("Save"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            EditorSaveSceneAs(sceneSaveName);
            sceneSavePopupOpen = false;
        }
        if (UIButton(CLAY_ID("SceneSaveCancel"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            sceneSavePopupOpen = false;
        }
    }
    UIEndWindow();
}

static void SceneSaveConfirmPopup(void)
{
    if (!sceneSaveConfirmOpen) return;
    if (Scene_GetActivePath()[0] == '\0')
    {
        sceneSaveConfirmOpen = false;
        return;
    }

    float2 center = { g_WindowState.prev_width * 0.5f - 230.0f, g_WindowState.prev_height * 0.5f - 85.0f };
    if (!UIBeginWindow("Save Scene?", center, (float2){ 460.0f, 180.0f }, &sceneSaveConfirmOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Overwrite the active scene?"), CLAY_TEXT_CONFIG({
        .fontSize = 15,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    CLAY_TEXT(UIStr(Scene_GetActivePath()), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));

    CLAY(CLAY_ID("SceneSaveConfirmButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("SceneSaveConfirmYes"), CLAY_STRING("Save"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            EditorSaveActiveScene();
            sceneSaveConfirmOpen = false;
        }
        if (UIButton(CLAY_ID("SceneSaveConfirmAsNew"), CLAY_STRING("Save As New"), (Clay_Dimensions){ 128.0f, 30.0f }, false))
        {
            sceneSaveConfirmOpen = false;
            sceneSavePopupOpen = true;
            sceneSaveName[0] = '\0';
        }
        if (UIButton(CLAY_ID("SceneSaveConfirmNo"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            sceneSaveConfirmOpen = false;
        }
    }
    UIEndWindow();
}

static void SceneDeletePopup(void)
{
    if (!sceneDeletePopupOpen) return;
    if (sceneDeletePath[0] == '\0')
    {
        sceneDeletePopupOpen = false;
        return;
    }

    float2 center = { g_WindowState.prev_width * 0.5f - 240.0f, g_WindowState.prev_height * 0.5f - 95.0f };
    if (!UIBeginWindow("Delete Scene?", center, (float2){ 480.0f, 200.0f }, &sceneDeletePopupOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Delete this scene and baked texture sidecars?"), CLAY_TEXT_CONFIG({
        .fontSize = 15,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    CLAY_TEXT(UIStr(sceneDeletePath), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));
    CLAY_TEXT(CLAY_STRING("Bundle .abm/.basis sources are kept."), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));

    CLAY(CLAY_ID("SceneDeleteButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("SceneDeleteYes"), CLAY_STRING("Delete"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            SceneDeleteWithDependencies(sceneDeletePath);
            sceneDeletePath[0] = '\0';
            sceneDeletePopupOpen = false;
        }
        if (UIButton(CLAY_ID("SceneDeleteNo"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            sceneDeletePopupOpen = false;
        }
    }
    UIEndWindow();
}

void DrawSceneWindow(bool* open)
{
    Clay_ElementId windowID = (Clay_ElementId) { .id = StringToHash("SceneWindow", 5381u) };
    if (UIBeginWindowId(windowID, "Scene", (float2) { 18.0f, 18.0f }, (float2) { 500.0f, 760.0f }, open, UIWindowFlags_RightClickable))
    {
        Scene* scene = Scene_GetActive();

        if (scene && GetKeyPressed(SDLK_DELETE) && (!UIAnyWindowHovered() || EditorSceneInteractAllowed()))
        {
            if (EditorGizmoDeleteSelected())
            {
                sceneObjectSelection.valid = false;
                sceneSelectedBundle = INVALID_BUNDLE;
                sceneSelectedNode = -1;
            }
            else if (sceneObjectSelection.valid) SceneDeleteSelectedObject();
            else if (sceneSelectedBundle != INVALID_BUNDLE && sceneSelectedBundle < scene->numBundles)
            {
                if (sceneSelectedNode < 0) SceneEventDeleteBundle(NULL);
                else SceneEventDeleteNode(NULL);
            }
        }

        if (scene && sceneSelectedBundle != INVALID_BUNDLE && sceneSelectedBundle < scene->numBundles)
        {
            if (sceneSelectedNode < 0)
            {
                UIRightClickAddEvent("Duplicate", SceneEventDuplicateBundle, NULL);
                UIRightClickAddEvent("Delete", SceneEventDeleteBundle, NULL);
            }
            else
                UIRightClickAddEvent("Delete", SceneEventDeleteNode, NULL);
        }
        if (sceneSelectedFile >= 0 && sceneSelectedFile < (s32)sceneNumFiles)
            UIRightClickAddEvent("Delete Scene File", SceneEventDeleteFile, NULL);

        CLAY(CLAY_ID("SceneWindowToolbar"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30.0f) },
                .childGap = 8,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            UIPushFloatAdd(UIFloat_TextScale, -0.15f);
            if (UIButton(CLAY_ID("SceneNewButton"), CLAY_STRING("New"), (Clay_Dimensions){ 80.0f, 26.0f }, false))
                EditorNewScene();
            if (scene && UIButton(CLAY_ID("SceneSaveButton"), CLAY_STRING("Save"), (Clay_Dimensions){ 80.0f, 26.0f }, false))
            {
                if (Scene_GetActivePath()[0])
                    sceneSaveConfirmOpen = true;
                else
                {
                    sceneSavePopupOpen = true;
                    sceneSaveName[0] = '\0';
                }
            }
            UIPopFloat(UIFloat_TextScale);
        }

        extern SceneAsyncRequest* sceneAsyncRequest;
        if (sceneAsyncRequest)
        {
            const char* asyncText = "Spawning bundle...";
            if (sceneAsyncRequest->op == SceneAsyncOp_OpenScene) asyncText = "Opening scene...";
            else if (sceneAsyncRequest->op == SceneAsyncOp_ImportMesh) asyncText = "Importing mesh...";
            CLAY_TEXT(UIStr(asyncText), CLAY_TEXT_CONFIG({
                .fontSize = 13,
                .textColor = UIGetClayColor(UIColor_SubText)
            }));
        }

        CLAY(CLAY_ID("SceneFilesPanel"), UIScrollPanelDeclaration(140.0f, 2u)) {
            SceneFilesUI();
        }
        UIDivider(CLAY_ID("SceneFilesDivider"));

        if (!scene)
        {
            CLAY_TEXT(CLAY_STRING("No active scene."), CLAY_TEXT_CONFIG({
                .fontSize = 14,
                .textColor = UIGetClayColor(UIColor_SubText)
            }));
        }
        else
        {
            sceneInfoOpen ^= UICollapsingHeader(CLAY_ID("SceneInfoHeader"), CLAY_STRING("Active Scene"), sceneInfoOpen);
            if (sceneInfoOpen)
            {
                CLAY(CLAY_ID("SceneWindowInfo"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .padding = { 6, 0, 0, 0 },
                        .childGap = 4,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    }
                }) {
                    UITextU32("Bundles", scene->numBundles);
                    UITextU32("Materials", scene->numMaterials);
                    UITextU32("Static entities", scene->surfaceSet.numEntities);
                    UITextU32("Transparent entities", scene->transparentSet.numEntities);
                    UITextU32("Skinned entities", scene->skinnedSet.numEntities);
                    UITextU32("Primitive groups", scene->surfaceSet.numGroups + scene->transparentSet.numGroups + scene->skinnedSet.numGroups);
                    UITextU32("Triangles", RenderSet_CountTriangles(&scene->surfaceSet) + RenderSet_CountTriangles(&scene->transparentSet) + RenderSet_CountTriangles(&scene->skinnedSet));
                }
            }
            UIDivider(CLAY_ID("SceneWindowDivider"));

            SceneLightsUI(scene);
            UIDivider(CLAY_ID("SceneWindowDivider2"));

            SceneInspectorUI(scene);
            UIDivider(CLAY_ID("SceneWindowDivider3"));

            sceneTreeRowBudget = SCENE_TREE_MAX_NODES;
            CLAY(CLAY_ID("SceneWindowTree"), UIScrollPanelDeclaration(UIWindowRemainingHeight(windowID, CLAY_ID("SceneWindowTree"), 0.0f), 2u)) {
                for (u32 b = 0u; b < scene->numBundles; b++)
                    if (scene->bundleRefs[b].bundle)
                        SceneBundleTree(scene, b);
                if (sceneTreeRowBudget == 0u)
                {
                    CLAY_TEXT(CLAY_STRING("... node limit reached, collapse some rows"), CLAY_TEXT_CONFIG({
                        .fontSize = 13,
                        .textColor = UIGetClayColor(UIColor_SubText)
                    }));
                }
            }
        }
        UIEndWindow();
    }

    SceneSavePopup();
    SceneSaveConfirmPopup();
    SceneDeletePopup();
    SceneImportDetailPopup();
}

//------------------------------------------------------------------------
// Textures Window

// texture page inspector, blits the selected 4k page layer into a small preview each frame
#define TEX_PREVIEW_SIZE 1024

static bool texInfoOpen = true;
static u32 texInspectClass = TextureClass_Albedo;
static u32 texInspectLayer;
static SDL_GPUTexture* texPagePreview;
static UIImageData texPagePreviewImage;

static u32 TexturePageMemoryMB(const TextureSystem* ts)
{
    u64 bytes = 0u;
    for (u32 i = 0u; i < TextureClass_Count; i++)
    {
        u64 texelBytes = ts->compressed ? 1u : (i == TextureClass_Albedo ? 4u : 2u);
        bytes += (u64)TEXTURE_PAGE_SIZE * TEXTURE_PAGE_SIZE * ts->classes[i].layerCount * texelBytes;
    }
    return (u32)((bytes * 4u / 3u) >> 20u); // mip chain adds ~1/3
}

static void TexturePageBlitPreview(const TexturePageClass* cls)
{
    if (!texPagePreview)
    {
        texPagePreview = CreateTexture2D(TEX_PREVIEW_SIZE, TEX_PREVIEW_SIZE, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                         SDL_GPU_SAMPLECOUNT_1, 1u, "TexturePagePreview");
        texPagePreviewImage = (UIImageData){ .texture = texPagePreview };
    }
    if (!texPagePreview) return;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    if (!cmd) return;

    SDL_GPUBlitInfo blit;
    SDL_zero(blit);
    blit.source.texture = cls->pages.handle;
    blit.source.layer_or_depth_plane = texInspectLayer;
    blit.source.w = (u32)cls->pages.width;
    blit.source.h = (u32)cls->pages.height;
    blit.destination.texture = texPagePreview;
    blit.destination.w = TEX_PREVIEW_SIZE;
    blit.destination.h = TEX_PREVIEW_SIZE;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter  = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &blit);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void DrawTexturesWindow(bool* open)
{
    Clay_ElementId windowID = (Clay_ElementId) { .id = StringToHash("TexturesWindow", 5381u) };
    if (!UIBeginWindowId(windowID, "Textures", (float2) { 540.0f, 18.0f }, (float2) { 520.0f, 760.0f }, open, 0u)) return;

    Scene* scene = Scene_GetActive();
    if (!scene)
    {
        CLAY_TEXT(CLAY_STRING("No active scene."), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        UIEndWindow();
        return;
    }

    TextureSystem* ts = &scene->textureSystem;

    texInfoOpen ^= UICollapsingHeader(CLAY_ID("TextureInfoHeader"), CLAY_STRING("Texture System"), texInfoOpen);
    if (texInfoOpen)
    {
        CLAY(CLAY_ID("TexturesWindowInfo"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .padding = { 6, 0, 0, 0 },
                .childGap = 4,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            UITextU32("Descriptors", ts->numDescriptors);
            UITextU32("Material watermark", ts->materialWatermark);
            UITextU32("Compressed pages", ts->compressed);
            UITextU32("Page size", TEXTURE_PAGE_SIZE);
            UITextU32("Albedo layers", ts->classes[TextureClass_Albedo].layerCount);
            UITextU32("Normal layers", ts->classes[TextureClass_Normal].layerCount);
            UITextU32("Metallic-roughness layers", ts->classes[TextureClass_MetallicRoughness].layerCount);
            UITextU32("Page memory (MB)", TexturePageMemoryMB(ts));
        }
    }
    UIDivider(CLAY_ID("TexturesWindowDivider"));

    CLAY(CLAY_ID("TextureClassButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        UIPushFloatAdd(UIFloat_TextScale, -0.15f);
        if (UIButton(CLAY_ID("TexClassAlbedo"), CLAY_STRING("Albedo")    , (Clay_Dimensions){ 110.0f, 28.0f }, texInspectClass == TextureClass_Albedo))            texInspectClass = TextureClass_Albedo;
        if (UIButton(CLAY_ID("TexClassNormal"), CLAY_STRING("Normal")    , (Clay_Dimensions){ 110.0f, 28.0f }, texInspectClass == TextureClass_Normal))            texInspectClass = TextureClass_Normal;
        if (UIButton(CLAY_ID("TexClassMR")    , CLAY_STRING("MetalRough"), (Clay_Dimensions){ 110.0f, 28.0f }, texInspectClass == TextureClass_MetallicRoughness)) texInspectClass = TextureClass_MetallicRoughness;
        UIPopFloat(UIFloat_TextScale);
    }

    const TexturePageClass* cls = &ts->classes[texInspectClass];
    if (!cls->pages.handle || cls->layerCount == 0u)
    {
        CLAY_TEXT(CLAY_STRING("No pages for this class."), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        UIEndWindow();
        return;
    }
    if (texInspectLayer >= cls->layerCount) texInspectLayer = 0u;

    CLAY(CLAY_ID("TextureLayerButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28.0f) },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        for (u32 i = 0u; i < cls->layerCount && i < TEXTURE_PAGE_LAYERS; i++)
        {
            Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("TexLayerButton"), i);
            char* buffer = UIFrameStringAlloc(16);
            IntToString(buffer, (s64)i, 0);
            if (UIButton(id, UIStr(buffer), (Clay_Dimensions){ 30.0f, 26.0f }, i == texInspectLayer)) texInspectLayer = i;
        }
    }

    TexturePageBlitPreview(cls);

    UIWindow* window = UIGetWindow(windowID);
    f32 side = window ? window->scale.x - 48.0f : 256.0f;
    side = Maxf32(Minf32(side, UIWindowRemainingHeight(windowID, CLAY_ID("TexturePagePreview"), 0.0f)), 64.0f);
    CLAY(CLAY_ID("TexturePagePreview"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(side), CLAY_SIZING_FIXED(side) } },
        .image = { .imageData = &texPagePreviewImage },
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(1) }
    }) {}

    UIEndWindow();
}
