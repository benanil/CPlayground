// terrain authoring window. backend terrain editing and .terrain serialization are
// intentionally not owned here: Terrain.c is the single owner of save/load and of the
// authoring metadata (name, paint layers, grass). this file only drives the UI and the
// per frame brush interaction, binding widgets straight to Terrain_GetAuthoring().
#include "EditorInternal.h"
#include "Include/Algorithm.h"
#include "Include/FileSystem.h"
#include "Include/Scene.h"
#include "Include/Terrain.h"
#include "Include/Platform.h"

extern WindowState g_WindowState;

typedef enum TerrainEditorMode_
{
    TerrainEditorMode_Manipulate,
    TerrainEditorMode_Paint,
    TerrainEditorMode_Grass
} TerrainEditorMode;

typedef struct TerrainEditorState_
{
    bool initialized;
    bool created;
    bool deleteConfirmOpen;
    bool fixedChunkSize;
    bool island;
    bool editMode;
    TerrainEditorMode mode;

    char savePath[512];
    f32 seed;
    f32 seaLevel;
    f32 baseHeight;
    f32 hillAmplitude;
    f32 hillFrequency;
    f32 ridgeAmplitude;
    f32 ridgeFrequency;
    f32 caveAmplitude;
    f32 caveFrequency;
    f32 fixedWorldSize;
    f32 islandRadius;
    f32 islandFalloff;
    f32 brushRadius;
    f32 brushStrength;
    f32 brushSoftness;

    u32 selectedLayer; // paint-layer index selected in the UI, indexes Terrain authoring
    bool lastSaveOk;
} TerrainEditorState;

static TerrainEditorState terrainUI;
static UITextAreaCustomData terrainTextData[32];
static u32 terrainTextDataCount;

// resolves the active scene's sibling ".terrain" path. the terrain name and the rest
// of the authoring metadata live in Terrain.c; here we only need the file location.
static bool TerrainSyncScenePath(void)
{
	const char* scenePath = Scene_GetActivePath();
    if (!scenePath || !scenePath[0]) return false;
	int len = StringLength(scenePath);
	terrainUI.savePath[len] = '\0';
	MemCopy(terrainUI.savePath, scenePath, len);
	ChangeExtension(terrainUI.savePath, len, "terrain");
    return true;
}

static void TerrainEditorInit(void)
{
    if (terrainUI.initialized) return;
    terrainUI.initialized = true;
    terrainUI.created = Terrain_GetEnabled();
    terrainUI.fixedChunkSize = false;
    terrainUI.island   = true;
    terrainUI.editMode = false;
    terrainUI.mode = TerrainEditorMode_Manipulate;
    TerrainSyncScenePath();
    terrainUI.seed           = 1.0f;
    terrainUI.seaLevel       = 0.0f;
    terrainUI.baseHeight     = -8.0f;
    terrainUI.hillAmplitude  = 0.6f;
    terrainUI.hillFrequency  = 0.7f;
    terrainUI.ridgeAmplitude = 0.5f;
    terrainUI.ridgeFrequency = 0.35f;
    terrainUI.caveAmplitude  = 0.8f;
    terrainUI.caveFrequency  = 0.045f;
    terrainUI.fixedWorldSize = (f32)TERRAIN_FIXED_WORLD_DEFAULT_SIZE;
    terrainUI.islandRadius   = 250.0f;
    terrainUI.islandFalloff  = 120.0f;
    terrainUI.brushRadius    = 10.0f;
    terrainUI.brushStrength  = 1.0f;
    terrainUI.brushSoftness  = 0.5f;
    // paint-layer and grass defaults are owned by Terrain.c (Terrain_GetAuthoring),
    // set up in Terrain_Init; the UI binds straight to that struct.
}

static TerrainGenParams TerrainEditorBuildParams(void)
{
    TerrainGenParams params = Terrain_DefaultGenParams();
    params.seed           = (u32)terrainUI.seed;
    params.seaLevel       = terrainUI.seaLevel;
    params.baseHeight     = terrainUI.baseHeight;
    params.hillAmplitude  = Clampf32(terrainUI.hillAmplitude, 0.1f, 4.0f);
    params.hillFrequency  = Clampf32(terrainUI.hillFrequency, 0.05f, 4.0f);
    params.ridgeAmplitude = Clampf32(terrainUI.ridgeAmplitude, 0.25f, 2.0f);
    params.ridgeFrequency = Clampf32(terrainUI.ridgeFrequency, 0.05f, 2.0f);
    params.carveAmplitude = Clampf32(terrainUI.caveAmplitude, 0.0f, 32.0f);
    params.carveFrequency = Clampf32(terrainUI.caveFrequency, 0.0001f, 0.2f);
    params.fixedWorldSize = (u32)Clamps32((s32)terrainUI.fixedWorldSize, TERRAIN_FIXED_WORLD_MIN_SIZE, TERRAIN_FIXED_WORLD_MAX_SIZE);
    params.island         = terrainUI.island;
    params.islandRadius   = terrainUI.islandRadius;
    params.islandFalloff  = terrainUI.islandFalloff;
    return params;
}

static void TerrainEditorApplyParams(const TerrainGenParams* params)
{
    terrainUI.seed           = (f32)params->seed;
    terrainUI.seaLevel       = params->seaLevel;
    terrainUI.baseHeight     = params->baseHeight;
    terrainUI.hillAmplitude  = params->hillAmplitude;
    terrainUI.hillFrequency  = params->hillFrequency;
    terrainUI.ridgeAmplitude = params->ridgeAmplitude;
    terrainUI.ridgeFrequency = params->ridgeFrequency;
    terrainUI.caveAmplitude  = params->carveAmplitude;
    terrainUI.caveFrequency  = params->carveFrequency;
    terrainUI.fixedWorldSize = (f32)params->fixedWorldSize;
    terrainUI.island         = params->island;
    terrainUI.islandRadius   = params->islandRadius;
    terrainUI.islandFalloff  = params->islandFalloff;
}

// per frame brush interaction, runs from the main loop before gizmo/picking and
// consumes the mouse while terrain edit mode is active over the scene
bool TerrainEditorUpdate(Camera* camera)
{
    bool wantsBrush = terrainUI.initialized && terrainUI.editMode && terrainUI.created &&
                      Terrain_GetEnabled() && EditorSceneInteractAllowed();
    if (!wantsBrush)
    {
        Terrain_SetBrushCursor(F3Zero(), 0.0f, false);
        return false;
    }

    RayV ray = ScreenPointToRay(camera, EditorSceneMouse());
    float3 origin = Vec3Get(ray.origin);
    float3 dir = F3Norm(Vec3Get(ray.dir));
    BVHHit hit;
    // the mesh raycast only covers the near lod rings (cpu copies), distant terrain
    // falls back to tracing the analytic density field so everywhere stays editable
    if (!Terrain_Raycast(origin, dir, 600.0f, 1u, &hit) &&
        !Terrain_RaycastField(origin, dir, 600.0f, &hit))
    {
        Terrain_SetBrushCursor(F3Zero(), 0.0f, false);
        return false;
    }

    float3 hitPos = Vec3Get(BVH_HitPositionV(ray.origin, ray.dir, &hit));
    Terrain_SetBrushCursor(hitPos, terrainUI.brushRadius, true);

    if (GetMouseDown(MouseButton_Left))
    {
        switch (terrainUI.mode)
        {
        case TerrainEditorMode_Manipulate:
        {
            // strength is meters per second, shift inverts to dig
            f32 strength = terrainUI.brushStrength * 18.0f * PlatformCtx.DeltaTime;
            if (GetKeyDown(SDLK_LSHIFT)) strength = -strength;
            Terrain_SculptSphere(hitPos, terrainUI.brushRadius, strength, terrainUI.brushSoftness);
            break;
        }
        case TerrainEditorMode_Paint:
            // strength slider scales how fast the blend weight saturates. only
            // enabled layers paint, disabled list entries do nothing
            if (Terrain_GetAuthoring()->layers[terrainUI.selectedLayer].enabled)
                Terrain_PaintSphere(hitPos, terrainUI.brushRadius, terrainUI.selectedLayer,
                                    Absf32(terrainUI.brushStrength) * 520.0f * PlatformCtx.DeltaTime,
                                    terrainUI.brushSoftness);
            break;
        default: // foliage and grass placement land with their systems
            break;
        }
        return true;
    }
    return false;
}

// Save/Load are owned by Terrain.c (Terrain_SaveWorld/Terrain_LoadWorld handle the
// .terrain params + authoring metadata and the sibling .chunks edits). the editor only
// resolves the active scene's .terrain path and pushes the current UI params into the
// runtime first so the save reflects on-screen settings.
static bool TerrainEditorSave(void)
{
    if (!TerrainSyncScenePath())
    {
        AX_WARN("terrain save skipped: active scene has no saved .scene path");
        return false;
    }
    return Terrain_SaveWorld(terrainUI.savePath);
}

static bool TerrainEditorLoad(void)
{
    if (!TerrainSyncScenePath())
    {
        AX_WARN("terrain load skipped: active scene has no saved .scene path");
        return false;
    }
    if (!Terrain_LoadWorld(terrainUI.savePath)) return false;

    // Terrain_LoadWorld rebuilt the world from the file; mirror its params back into
    // the UI so the editor widgets show the loaded settings
    TerrainEditorApplyParams(Terrain_GetGenParams());
    terrainUI.created = true;
    return true;
}

void TerrainEditorSceneChanged(bool loadSidecar)
{
    (void)loadSidecar;
    TerrainEditorInit();
    terrainUI.created = Terrain_GetEnabled();
    terrainUI.editMode = false;
    terrainUI.lastSaveOk = false;
    TerrainSyncScenePath();
    if (terrainUI.created)
        TerrainEditorApplyParams(Terrain_GetGenParams());
}

static void TerrainTextEdit(Clay_ElementId id, char* buffer, u32 capacity, f32 height)
{
    if (terrainTextDataCount >= (u32)(sizeof(terrainTextData) / sizeof(terrainTextData[0]))) return;
    UITextAreaCustomData* textData = &terrainTextData[terrainTextDataCount++];
    textData->type = UICustomType_TextArea;
    textData->buffer = buffer;
    textData->capacity = capacity;
    textData->flags = UITextAreaFlags_CenterY | UITextAreaFlags_NoWrap | UITextAreaFlags_Clip;
    textData->edited = 0u;
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(height) } },
        .custom = { .customData = textData }
    }) {}
}

static void TerrainTextLabel(const char* label)
{
    CLAY_TEXT(UIStr(label), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));
}

static void TerrainLabeledText(Clay_ElementId id, const char* label, char* buffer, u32 capacity)
{
    TerrainTextLabel(label);
    TerrainTextEdit(id, buffer, capacity, 26.0f);
}

static void TerrainToolbar(void)
{
    CLAY(CLAY_ID("TerrainToolbar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        UIPushFloatAdd(UIFloat_TextScale, -0.15f);
        if (UIButton(CLAY_ID("TerrainCreate"), CLAY_STRING("Create"), (Clay_Dimensions){ 78.0f, 26.0f }, false))
        {
            TerrainSyncScenePath();
            // applies the current noise settings, also regenerates an existing world
            TerrainGenParams params = TerrainEditorBuildParams();
            Terrain_CreateWorld(&params);
            terrainUI.created = true;
        }
        if (UIButton(CLAY_ID("TerrainDelete"), CLAY_STRING("Delete"), (Clay_Dimensions){ 78.0f, 26.0f }, false))
            terrainUI.deleteConfirmOpen = true;
        if (UIButton(CLAY_ID("TerrainSave"), CLAY_STRING("Save"), (Clay_Dimensions){ 78.0f, 26.0f }, false))
            terrainUI.lastSaveOk = TerrainEditorSave();
        if (UIButton(CLAY_ID("TerrainLoad"), CLAY_STRING("Load"), (Clay_Dimensions){ 78.0f, 26.0f }, false))
            terrainUI.lastSaveOk = TerrainEditorLoad();
        UIPopFloat(UIFloat_TextScale);
    }
}

static void TerrainNoiseUI(void)
{
    UISectionHeader("Noise Settings");
    bool hasScenePath = TerrainSyncScenePath();
    TerrainTextLabel("Terrain file (from active scene)");
    if (hasScenePath)
    {
        CLAY_TEXT(UIStr(terrainUI.savePath), CLAY_TEXT_CONFIG({
            .fontSize = 13,
            .textColor = UIGetClayColor(UIColor_Text)
        }));
    }
    else
    {
        CLAY_TEXT(CLAY_STRING("Save the scene first to create ScenePathAndName.terrain"), CLAY_TEXT_CONFIG({
            .fontSize = 13,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
    }
    bool edited = false;
    edited |= UICheckbox(CLAY_ID("TerrainFixedChunks"), CLAY_STRING("Fixed chunk size, do not stream with movement"), &terrainUI.fixedChunkSize);
    if (terrainUI.fixedChunkSize)
        edited |= UIEditFloat(CLAY_ID("TerrainFixedWorldSize"), CLAY_STRING("Fixed world size"), &terrainUI.fixedWorldSize, (f32)TERRAIN_FIXED_WORLD_MIN_SIZE, (f32)TERRAIN_FIXED_WORLD_MAX_SIZE, 16.0f, 0);
    edited |= UICheckbox(CLAY_ID("TerrainIsland"), CLAY_STRING("Island mask, center area above sea level"), &terrainUI.island);
    edited |= UIEditFloat(CLAY_ID("TerrainSeed"), CLAY_STRING("Seed"), &terrainUI.seed, 0.0f, 999999.0f, 1.0f, 0);
    edited |= UIEditFloat(CLAY_ID("TerrainSeaLevel"), CLAY_STRING("Sea level"), &terrainUI.seaLevel, -200.0f, 200.0f, 1.0f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainBaseHeight"), CLAY_STRING("Base height"), &terrainUI.baseHeight, -200.0f, 200.0f, 1.0f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainHillAmp"), CLAY_STRING("Lowland scale"), &terrainUI.hillAmplitude, 0.25f, 8.0f, 0.1f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainHillFreq"), CLAY_STRING("Noise scale"), &terrainUI.hillFrequency, 0.05f, 4.0f, 0.05f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainRidgeAmp"), CLAY_STRING("Height scale"), &terrainUI.ridgeAmplitude, 0.25f, 2.0f, 0.05f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainRidgeFreq"), CLAY_STRING("Mountain scale"), &terrainUI.ridgeFrequency, 0.05f, 2.0f, 0.05f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainCaveAmp"), CLAY_STRING("Carve amplitude"), &terrainUI.caveAmplitude, 0.0f, 32.0f, 0.5f, 2);
    edited |= UIEditFloat(CLAY_ID("TerrainCaveFreq"), CLAY_STRING("Carve frequency"), &terrainUI.caveFrequency, 0.0001f, 0.2f, 0.001f, 5);
    if (edited)
    {
        AX_LOG("terrain edited");
        // applies the current noise settings, also regenerates an existing world
        TerrainGenParams params = TerrainEditorBuildParams();
        Terrain_CreateWorld(&params);
        terrainUI.created = true;
    }
    if (terrainUI.island)
    {
        UIEditFloat(CLAY_ID("TerrainIslandRadius"), CLAY_STRING("Island radius"), &terrainUI.islandRadius, 1.0f, 10000.0f, 10.0f, 1);
        UIEditFloat(CLAY_ID("TerrainIslandFalloff"), CLAY_STRING("Island falloff"), &terrainUI.islandFalloff, 1.0f, 5000.0f, 10.0f, 1);
    }
}

static void TerrainEditModeUI(void)
{
    UISectionHeader("Edit Mode");
    UICheckbox(CLAY_ID("TerrainEditMode"), CLAY_STRING("Terrain edit mode"), &terrainUI.editMode);
    CLAY(CLAY_ID("TerrainModeButtons"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30.0f) }, .childGap = 6, .layoutDirection = CLAY_LEFT_TO_RIGHT }
    }) {
        if (UIButtonFlags(CLAY_ID("TerrainModeManipulate"), CLAY_STRING("Manipulate"), (Clay_Dimensions){ 72.0f, 26.0f }, terrainUI.mode == TerrainEditorMode_Manipulate, UIButtonFlag_FitText)) terrainUI.mode = TerrainEditorMode_Manipulate;
        if (UIButtonFlags(CLAY_ID("TerrainModePaint"), CLAY_STRING("Paint"), (Clay_Dimensions){ 58.0f, 26.0f }, terrainUI.mode == TerrainEditorMode_Paint, UIButtonFlag_FitText)) terrainUI.mode = TerrainEditorMode_Paint;
        if (UIButtonFlags(CLAY_ID("TerrainModeGrass"), CLAY_STRING("Grass"), (Clay_Dimensions){ 58.0f, 26.0f }, terrainUI.mode == TerrainEditorMode_Grass, UIButtonFlag_FitText)) terrainUI.mode = TerrainEditorMode_Grass;
    }
    UIEditFloat(CLAY_ID("TerrainBrushRadius"), CLAY_STRING("Brush radius"), &terrainUI.brushRadius, 0.1f, 200.0f, 1.0f, 2);
    UIEditFloat(CLAY_ID("TerrainBrushStrength"), CLAY_STRING("Brush strength"), &terrainUI.brushStrength, -10.0f, 10.0f, 0.1f, 2);
    UIEditFloat(CLAY_ID("TerrainBrushSoftness"), CLAY_STRING("Brush softness"), &terrainUI.brushSoftness, 0.0f, 1.0f, 0.05f, 2);
    CLAY_TEXT(CLAY_STRING("Cursor preview: terrain hit point whitens while edit mode is active."), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));
}

static void TerrainLayersUI(void)
{
    UISectionHeader("Paint Layers");
    static const char* layerOptions[] = { "Layer 0", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6", "Layer 7" };
    UIDropdown(CLAY_ID("TerrainLayerSelect"), CLAY_STRING("Selected layer"), layerOptions, TERRAIN_MAX_LAYERS, &terrainUI.selectedLayer);
    TerrainLayerDesc* layer = &Terrain_GetAuthoring()->layers[terrainUI.selectedLayer];
    UICheckbox(CLAY_ID("TerrainLayerEnabled"), CLAY_STRING("Layer enabled"), &layer->enabled);
    TerrainLabeledText(CLAY_ID("TerrainLayerAlbedo"), "Albedo texture path", layer->albedo, sizeof(layer->albedo));
    TerrainLabeledText(CLAY_ID("TerrainLayerNormal"), "Normal texture path", layer->normal, sizeof(layer->normal));
}

static void TerrainGrassUI(void)
{
    UISectionHeader("Grass Blades");
    TerrainAuthoring* authoring = Terrain_GetAuthoring();
    UIEditFloat(CLAY_ID("TerrainGrassDensity"), CLAY_STRING("Density"), &authoring->grassDensity, 0.0f, 1000.0f, 1.0f, 3);
    UIEditFloat(CLAY_ID("TerrainGrassViewDist"), CLAY_STRING("View distance"), &authoring->grassViewDistance, 0.0f, 2000.0f, 10.0f, 1);
    UIEditFloat(CLAY_ID("TerrainGrassScaleMin"), CLAY_STRING("Scale min"), &authoring->grassScaleMin, 0.01f, 10.0f, 0.1f, 3);
    UIEditFloat(CLAY_ID("TerrainGrassScaleMax"), CLAY_STRING("Scale max"), &authoring->grassScaleMax, 0.01f, 10.0f, 0.1f, 3);
    TerrainLabeledText(CLAY_ID("TerrainGrassColor"), "Blade color hex AABBGGRR", authoring->grassColorHex, sizeof(authoring->grassColorHex));
    UIButtonFlags(CLAY_ID("TerrainGrassGenerate"), CLAY_STRING("Generate grass"), (Clay_Dimensions){ 110.0f, 28.0f }, false, UIButtonFlag_FitText);
}

static void TerrainStatsUI(void)
{
    UISectionHeader("Runtime Stats");
    TerrainStats stats = Terrain_GetStats();
    UITextU32("Live chunks", stats.liveChunks);
    UITextU32("Empty chunks", stats.emptyChunks);
    UITextU32("Queued chunks", stats.queuedChunks);
    UITextU32("Jobs in flight", stats.jobsInFlight);
    UITextU32("Drawn chunks", stats.drawnChunks);
    UITextU32("Vertices", stats.numVertices);
    UITextU32("Indices", stats.numIndices);
}

static void TerrainDeletePopup(void)
{
    if (!terrainUI.deleteConfirmOpen) return;
    float2 center = { g_WindowState.prev_width * 0.5f - 220.0f, g_WindowState.prev_height * 0.5f - 85.0f };
    if (!UIBeginWindow("Delete Terrain?", center, (float2){ 440.0f, 170.0f }, &terrainUI.deleteConfirmOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Delete the active terrain from the scene?"), CLAY_TEXT_CONFIG({
        .fontSize = 15,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    CLAY_TEXT(CLAY_STRING("Saved .terrain files are not removed."), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));
    CLAY(CLAY_ID("TerrainDeleteButtons"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) }, .childGap = 10, .layoutDirection = CLAY_LEFT_TO_RIGHT }
    }) {
        if (UIButton(CLAY_ID("TerrainDeleteYes"), CLAY_STRING("Delete"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            terrainUI.created = false;
            terrainUI.editMode = false;
            Terrain_DeleteWorld();
            terrainUI.deleteConfirmOpen = false;
        }
        if (UIButton(CLAY_ID("TerrainDeleteNo"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
            terrainUI.deleteConfirmOpen = false;
    }
    UIEndWindow();
}

void DrawTerrainWindow(bool* open)
{
    TerrainEditorInit();
    terrainTextDataCount = 0u;
    terrainUI.created = terrainUI.created || Terrain_GetEnabled();

    Clay_ElementId windowID = CLAY_ID("TerrainWindow");
    if (UIBeginWindowId(windowID, "Terrain", (float2){ 540.0f, 80.0f }, (float2){ 520.0f, 760.0f }, open, 0u))
    {
        TerrainToolbar();
        CLAY(CLAY_ID("TerrainEditorScroll"), UIScrollPanelDeclaration(UIWindowRemainingHeight(windowID, CLAY_ID("TerrainEditorScroll"), 0.0f), 12u)) {
            TerrainNoiseUI();
            UIDivider(CLAY_ID("TerrainNoiseDivider"));
            TerrainEditModeUI();
            UIDivider(CLAY_ID("TerrainEditDivider"));
            TerrainLayersUI();
            UIDivider(CLAY_ID("TerrainLayerDivider"));
            TerrainGrassUI();
            UIDivider(CLAY_ID("TerrainGrassDivider"));
            TerrainStatsUI();
            CLAY_TEXT(terrainUI.lastSaveOk ? CLAY_STRING("Last save: ok") : CLAY_STRING("Last save: pending"), CLAY_TEXT_CONFIG({
                .fontSize = 13,
                .textColor = UIGetClayColor(UIColor_SubText)
            }));
        }
        UIEndWindow();
    }
    TerrainDeletePopup();
}
