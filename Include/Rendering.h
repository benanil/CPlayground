#ifndef CP_RENDERING
#define CP_RENDERING

#include "Graphics.h"

typedef struct RenderSettings_
{
    bool enableOcclusion;
    bool enableHBAO;
    bool enableContactShadows;
    bool enableMLAA;
    bool enableBloom;
    u32 msaaSamples;      // requested scene MSAA samples: 1, 2, 4, or 8. UI/post stay single-sample
    bool showMLAAEdges;
    bool enableLocalLights;
    bool enableLightFrustumCulling;
    bool enableLightOcclusionCulling;
    bool showLightRects;
    bool terrainWireframe;  // draws the terrain chunk triangles as a line overlay
    f32 terrainLodFactor;   // scales the terrain lod ring radii, 1 = default reach
    f32 hbaoRadius;
    f32 hbaoBias;
    f32 hbaoIntensity;
    f32 hbaoPower;
    f32 SSSBilinearThreshold;
    f32 SSSThickness;
    f32 SSSIntensity;
    f32 SSSContrast;
    f32 mlaaThreshold;
    f32 bloomThreshold;
    f32 bloomKnee;
    f32 bloomClamp;
    f32 bloomIntensity;
    f32 bloomRadius;
    f32 exposure;
    f32 gamma;
    f32 godRayIntensity;
    f32 godRaySamples;    // ray march steps, 0 disables
    bool enableHeightFog;
    f32 fogColor[3];      // fog tint (linear rgb)
    f32 fogDensity;       // overall opacity scale
    f32 fogHeight;        // world-Y base height of the fog layer
    f32 fogFalloff;       // height falloff rate, larger = thins faster with altitude
    f32 fogSunScatter;    // 0..1 sun-direction tint strength
    f32 hbaoDirections;   // horizon sample directions
    f32 lodDistanceModifier;
    f32 renderScale;      // scene resolution multiplier, ui stays native. < 1 cuts gpu memory and shading cost
    f32 sunYaw;
    f32 sunPitch;
    f32 shadowMaxDistance;
    f32 shadowCameraDistance;
    f32 shadowCasterDepthMargin;
    f32 shadowCascadeOverlap;
    f32 shadowSplitNearDistance;
    f32 shadowPSSMLambda;
    f32 maxVisiblePointShadows;
    f32 maxVisibleSpotShadows;
} RenderSettings;

typedef struct RenderLightDebugInfo_
{
    u32 totalLights;
    u32 submittedLights;
    u32 maxLights;
} RenderLightDebugInfo;

extern RenderSettings g_RenderSettings;

// post processed scene texture of the current frame, displayed by the editor scene view
SDL_GPUTexture* RenderGetFinalTexture(void);

void DestroyPipeline();

void Quit(int rc);

void Render();

void InitBuffers();

void RendererInit();

void RendererSetLights(const LightGPU* lights, u32 count);

#define MAX_OUTLINE_TARGETS 2048u

// one outlined render set entity of the active scene
typedef struct OutlineTarget_
{
    u32 skinned;
    u32 groupIdx;
    u32 entityIdx;
} OutlineTarget;

// editor selection outlines, the gizmo submits every entity of the selection each frame
void RendererSetOutlineTargets(const OutlineTarget* targets, u32 count);

void RendererClearOutlineTarget(void);

#define MAX_GIZMO_VERTICES 2048u
// world space line overlay drawn on top of everything (no depth), the editor gizmo
// submits its lines every frame, count 0 hides it
void RendererSetGizmoLines(const ALineVertex* vertices, u32 count);

#define MAX_TERRAIN_CHUNK_DRAWS 8192u
// static terrain chunk meshes resident in the TerrainVertex geometry heap: each draw is
// a vertex range (ALineVertex layout, non-indexed triangles) drawn straight from the
// heap's GPU mirror, no per-frame copy. the ranges convert to indirect draw commands so
// every chunk renders in one SDL_DrawGPUPrimitivesIndirect. count 0 hides the terrain.
typedef struct TerrainChunkDraw_
{
    u32 first;
    u32 count;
} TerrainChunkDraw;
void RendererSetTerrainChunkDraws(const TerrainChunkDraw* draws, u32 count);

// editor brush highlight, applied in the terrain fragment shader around the cursor.
// radius <= 0 disables it
void RendererSetTerrainBrush(float3 position, f32 radius);

u32 RendererGetLightCount(void);

RenderLightDebugInfo RendererGetLightDebugInfo(void);

#endif
