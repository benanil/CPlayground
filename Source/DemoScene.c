
#include "Include/DemoScene.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"
#include "Include/Rendering.h"
#include "Include/Random.h"
#include "Include/IntFloat.h"
#include "Math/Quaternion.h"

static Scene g_DemoScene;
static u32 g_PaladinBundle = INVALID_BUNDLE;
static u32 g_BistroBundle  = INVALID_BUNDLE;
static LightGPU g_DemoLights[12];

// assigns a random animation and time offset to every instance slot
static void RandomizeAnimInstances(void)
{
    AnimationSystem* anims = &g_DemoScene.animSystem;
    if (anims->numAnimations == 0) return;

    GPUAnimationInstance instances[MAX_ANIM_INSTANCES];
    for (u32 i = 0; i < MAX_ANIM_INSTANCES; i++)
    {
        u32 hash = WangHash(i + 645u);
        u32 ordinal = hash % anims->numAnimations;
        if (ordinal == 0 && anims->numAnimations > 1) ordinal = 1; // skip the bind pose
        u32 animIdx = AnimationSystem_GetNthUsedAnim(anims, ordinal);
        instances[i] = (GPUAnimationInstance){
            .animIdx = animIdx,
            .timeOffset = NextFloat01(hash) * anims->animData[animIdx].duration,
        };
    }
    AnimationSystem_UpdateInstances(anims, instances, MAX_ANIM_INSTANCES);
}

static void UpdateDemoLights(void)
{
    static const f32 colors[8][3] = {
        { 1.00f, 0.35f, 0.20f }, { 0.25f, 0.55f, 1.00f },
        { 0.25f, 0.80f, 0.35f }, { 1.00f, 0.85f, 0.25f },
        { 0.95f, 0.25f, 1.00f }, { 0.25f, 0.85f, 0.75f },
        { 1.00f, 0.55f, 0.25f }, { 0.55f, 0.35f, 1.00f }
    };

    f32 time = (f32)PlatformCtx.FrameCount * 0.012f;
    int numLights = 0;
    for (u32 i = 0; i < 0u; i++)
    {
        u32 seed = WangHash(0x9e3779b9u + i * 0x85ebca6bu);
        f32 x = f32_(i) * 0.7f + RepeatMinMaxF32(WangHash(seed + 1u), -2.0f, 2.0f);
        f32 y = 2.0f + RepeatMinMaxF32(WangHash(seed + 2u), -0.5f, 0.5f);
        f32 z = RepeatMinMaxF32(WangHash(seed + 3u), -2.0f, 2.0f);
        f32 baseYaw = RepeatMinMaxF32(WangHash(seed + 4u), 0.0f, MATH_TwoPI);
        f32 yaw = baseYaw + time * RepeatMinMaxF32(WangHash(seed + 5u), 0.35f, 0.85f);
        f32 pitch = RepeatMinMaxF32(WangHash(seed + 6u), -0.65f, -0.20f) + Sin(time * 0.7f + baseYaw) * 0.25f;
        f32 dx = Cos(yaw) * Cos(pitch);
        f32 dy = Sin(pitch);
        f32 dz = Sin(yaw) * Cos(pitch);
        f32 radius = RepeatMinMaxF32(WangHash(seed + 7u), 10.0f, 18.0f);

        LightGPU* light = &g_DemoLights[i];
        light->positionRadius[0] = x;
        light->positionRadius[1] = y;
        light->positionRadius[2] = z;
        light->positionRadius[3] = radius;
        f32 directionCone[4] = { dx, dy, dz, 0.72f };
        LightGPU_SetDirectionCone(light, directionCone);
        LightGPU_SetColor3(light, colors[i & 7u]);
        LightGPU_SetIntensity(light, RepeatMinMaxF32(WangHash(seed + 8u), 24.0f, 44.0f));
        light->type = LightType_Spot;
        light->flags = LIGHT_FLAG_SHADOWED;
        light->shadowIndex = LIGHT_SHADOW_INDEX_INVALID;
        numLights++;
    }

    for (u32 i = 0; i < 8u && numLights < (int)ARRAY_SIZE(g_DemoLights); i++)
    {
        u32 lightIndex = (u32)numLights;
        f32 fi = (f32)i;
        f32 angle = time * (0.35f + fi * 0.045f) + fi * (MATH_TwoPI / 8.0f);
        f32 orbit = 3.0f + (f32)(i % 4u) * 2.0f;
        f32 x = Cos(angle) * orbit;
        f32 y = 2.5f + Sin(angle * 1.7f + fi) * 1.25f;
        f32 z = Sin(angle) * orbit;
        f32 radius = 7.0f + (f32)(i % 3u) * 2.0f;

        LightGPU* light = &g_DemoLights[lightIndex];
        light->positionRadius[0] = x;
        light->positionRadius[1] = y;
        light->positionRadius[2] = z;
        light->positionRadius[3] = radius;
        f32 directionCone[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
        LightGPU_SetDirectionCone(light, directionCone);
        LightGPU_SetColor3(light, colors[i & 7u]);
        LightGPU_SetIntensity(light, 16.0f + (f32)(i % 4u) * 5.0f);
        light->type = LightType_Point;
        light->flags = LIGHT_FLAG_SHADOWED;
        light->shadowIndex = LIGHT_SHADOW_INDEX_INVALID;
        numLights++;
    }
    RendererSetLights(g_DemoLights, numLights);
}

s32 DemoScene_Create(void)
{
    Scene_Init(&g_DemoScene);
    g_PaladinBundle = Scene_AddBundle(&g_DemoScene, "Assets/Meshes/Paladin/Paladin.gltf", true);
    g_BistroBundle  = Scene_AddBundle(&g_DemoScene, "Assets/Meshes/Bistro/Bistro.glb", false);
    if (g_PaladinBundle == INVALID_BUNDLE || g_BistroBundle == INVALID_BUNDLE)
        return 0;

    RandomizeAnimInstances();

    const int numCharacters = 7;
    const int charGridStride = (int)Ceilf(Sqrtf((float)numCharacters));
    for (s32 i = 0; i < numCharacters; i++)
    {
        u64 hash = MurmurHash((u64)i + 123);
        v128f pos = VecMulf(VecSetR(f32_(i % charGridStride), 0.0f, f32_(i / charGridStride), 4.0f), 1.5f);
        v128f rot = QFromAxisAngle(F3Up(), (float)(NextDouble01(hash) * 2.0 * MATH_PI));
        v128f scale = VecSet1(0.01f);

        if (!Scene_Spawn(&g_DemoScene, g_PaladinBundle, pos, rot, scale))
            break;
    }

    const int numSurface = 4;
    const int surfaceGridStride = (int)Ceilf(Sqrtf((float)numSurface));
    for (s32 i = 0; i < numSurface; i++)
    {
        v128f pos = VecMulf(VecSetR(0.02f+f32_(i % surfaceGridStride), -0.0f, f32_(i / surfaceGridStride) -0.0f, 0.0f), 150.0f);
        v128f rot = QIdentity();
        v128f scale = VecSet1(0.1f);
        if (!Scene_Spawn(&g_DemoScene, g_BistroBundle, pos, rot, scale))
            break;
    }
    return 1;
}

void DemoScene_Update(f32 deltaTime)
{
    // editor scenes own their lights, only drive the demo lights while the demo renders
    if (Scene_GetActive() != &g_DemoScene) return;
    UpdateDemoLights();
}

Scene* DemoScene_Get(void)
{
    return &g_DemoScene;
}
