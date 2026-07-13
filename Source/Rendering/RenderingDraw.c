#include "RenderingInternal.h"
#include "Include/TextureSystem.h"
#include "Include/Terrain.h"
#include "Math/Bitpack.h"

float3 GetRenderSunDirection(void)
{
    f32 yaw = g_RenderSettings.sunYaw * MATH_DegToRad;
    f32 pitch = g_RenderSettings.sunPitch * MATH_DegToRad;
    float3 direction = {
        Cos(yaw) * Cos(pitch),
        Sin(pitch),
        Sin(yaw) * Cos(pitch)
    };
    return F3NormSafe(direction);
}

static void DrawRenderBufferDepth(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, bool isSkinned, Scene* scene,
                                  const SDL_GPUBufferBinding vertex_binding,
                                  const SDL_GPUTextureSamplerBinding albedoSampler,
                                  const DepthPassContext* ctx,
                                  SDL_GPUBuffer* const fragmentBuffers[2])
{
    bool useShadow  = ctx->flags & DepthPassFlag_AnyShadow;
    bool alphaClip  = ctx->flags & DepthPassFlag_AlphaClip;
    const SDL_GPUBufferBinding index_binding = { g_RenderState.indexBuffer, 0 };
    
    const RenderSetBuffers*  buffers   = isSkinned ? &scene->skinnedBuffers : &scene->surfaceBuffers;
    const RenderSet*         renderSet = isSkinned ? &scene->skinnedSet     : &scene->surfaceSet;
    SDL_GPUGraphicsPipeline* pipeline  = isSkinned ? ctx->skinnedPipeline   : ctx->surfacePipeline;
    if (renderSet->numGroups == 0) return;

    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // skinned meshes bind animated vertices ahead of the shadow matrices, so their shadow
    // matrix buffer lands one slot later than the surface layout
    SDL_GPUBuffer* storageBuffers[6];
    u32 count = 0;
    storageBuffers[count++] = buffers->entity;
    storageBuffers[count++] = buffers->primitiveGroup;
    storageBuffers[count++] = buffers->drawSparseIndices;
    if (isSkinned) storageBuffers[count++] = g_RenderState.skinned.animatedVertices;
    if (useShadow) storageBuffers[count++] = ctx->shadowMatrixBuffer;
    if (isSkinned) storageBuffers[count++] = buffers->primitiveGroupLOD;
    SDL_BindGPUVertexStorageBuffers(pass, 0, storageBuffers, count);

    if (alphaClip)
    {
        SDL_BindGPUFragmentSamplers(pass, 0, &albedoSampler, 1);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 2);
    }
    if (useShadow) SDL_PushGPUVertexUniformData(cmd, 0, &ctx->cascadeIndex, sizeof(u32));
    else SDL_PushGPUVertexUniformData(cmd, 0, &ctx->viewProj, sizeof(mat4x4));
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, buffers->drawArgs, 0, renderSet->numGroups * MESH_LOD_COUNT);
}

void RenderDepth(SDL_GPUCommandBuffer* cmd, const DepthPassContext* ctx)
{
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, ctx->colorTarget, ctx->colorTarget ? 1 : 0, ctx->depthTarget);
    if (ctx->viewport) SDL_SetGPUViewport(pass, ctx->viewport);
    if (ctx->scissor) SDL_SetGPUScissor(pass, ctx->scissor);

    Scene* scene = g_ActiveScene;
    SDL_GPUTextureSamplerBinding albedoSampler = { .texture = scene->textureSystem.classes[TextureClass_Albedo].pages.handle, .sampler = g_RenderState.sampler };
    SDL_GPUBuffer* fragmentBuffers[2] = {
        scene->textureSystem.materialBuffer,
        scene->textureSystem.descriptorBuffer
    };

    const SDL_GPUBufferBinding skinnedVertex = { g_RenderState.skinned.vertexBuffer, 0 };
    DrawRenderBufferDepth(cmd, pass, true, scene, skinnedVertex, albedoSampler, ctx, fragmentBuffers);

    const SDL_GPUBufferBinding surfaceVertex = { g_RenderState.surface.vertexBuffer, 0 };
    DrawRenderBufferDepth(cmd, pass, false, scene, surfaceVertex, albedoSampler, ctx, fragmentBuffers);

    // terrain draws into the main depth prepass only, it does not cast shadows yet
    if ((ctx->flags & DepthPassFlag_AnyShadow) == 0)
    {
        RenderTerrainTrianglesDepth(cmd, pass, ctx->viewProj);
    }

    SDL_EndGPURenderPass(pass);
}

static void DrawRenderBufferScene(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, bool isSkinned, Scene* scene,
                                  const SDL_GPUBufferBinding vertex_binding,
                                  const SDL_GPUTextureSamplerBinding pageSamplers[4],
                                  SDL_GPUBuffer* const fragmentBuffers[2],
                                  const void* vertexParams, u32 vertexParamsSize,
                                  const void* fragmentParams, u32 fragmentParamsSize)
{
    const SDL_GPUBufferBinding index_binding = { g_RenderState.indexBuffer, 0 };
    const RenderSetBuffers*  buffers   = isSkinned ? &scene->skinnedBuffers : &scene->surfaceBuffers;
    const RenderSet*         renderSet = isSkinned ? &scene->skinnedSet     : &scene->surfaceSet;
    SDL_GPUGraphicsPipeline* pipeline  = isSkinned ? g_RenderState.skinned.forwardPipeline : g_RenderState.surface.forwardPipeline;
    if (renderSet->numGroups == 0) return;
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // skinned meshes bind animated vertices ahead of the shadow cascades, so the cascade
    // buffer lands one slot later than the surface layout. The forward pass also needs the bone
    // matrices (t5) because it re-skins the tangent frame (not cached in the position-only buffer).
    SDL_GPUBuffer* storageBuffers[7];
    u32 count = 0;
    storageBuffers[count++] = buffers->entity;
    storageBuffers[count++] = buffers->primitiveGroup;
    storageBuffers[count++] = buffers->drawSparseIndices;
    if (isSkinned) storageBuffers[count++] = g_RenderState.skinned.animatedVertices;
    storageBuffers[count++] = g_RenderState.shadowCascadeBuffer;
    if (isSkinned) storageBuffers[count++] = scene->animSystem.boneBuffer;
    if (isSkinned) storageBuffers[count++] = buffers->primitiveGroupLOD;
    SDL_BindGPUVertexStorageBuffers(pass, 0, storageBuffers, count);

    SDL_BindGPUFragmentSamplers(pass, 0, pageSamplers, 4);
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 2);
    SDL_PushGPUVertexUniformData(cmd, 0, vertexParams, vertexParamsSize);
    SDL_PushGPUFragmentUniformData(cmd, 0, fragmentParams, fragmentParamsSize);
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, buffers->drawArgs, 0, renderSet->numGroups * MESH_LOD_COUNT);
}

// Forward+ opaque draw: same geometry/instancing, binds the forward
// pipeline (single HDR target, depth test only) and the extra fragment resources the
// forward shader needs (shadow atlases, AO, light buffer + tile grid/index).
static void DrawRenderBufferForward(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, bool isSkinned, Scene* scene,
                                    const RenderSet* renderSet, const RenderSetBuffers* buffers,
                                    SDL_GPUGraphicsPipeline* pipeline,
                                    const SDL_GPUBufferBinding vertex_binding,
                                    const SDL_GPUTextureSamplerBinding fragmentSamplers[8],
                                    SDL_GPUBuffer* const fragmentBuffers[7],
                                    const void* vertexParams, u32 vertexParamsSize,
                                    const void* fragmentParams, u32 fragmentParamsSize)
{
    const SDL_GPUBufferBinding index_binding = { g_RenderState.indexBuffer, 0 };
    if (renderSet->numGroups == 0 || !pipeline) return;
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* storageBuffers[7];
    u32 count = 0;
    storageBuffers[count++] = buffers->entity;
    storageBuffers[count++] = buffers->primitiveGroup;
    storageBuffers[count++] = buffers->drawSparseIndices;
    if (isSkinned) storageBuffers[count++] = g_RenderState.skinned.animatedVertices;
    storageBuffers[count++] = g_RenderState.shadowCascadeBuffer;
    if (isSkinned) storageBuffers[count++] = scene->animSystem.boneBuffer;
    if (isSkinned) storageBuffers[count++] = buffers->primitiveGroupLOD;
    SDL_BindGPUVertexStorageBuffers(pass, 0, storageBuffers, count);

    SDL_BindGPUFragmentSamplers(pass, 0, fragmentSamplers, 8);
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 7);
    SDL_PushGPUVertexUniformData(cmd, 0, vertexParams, vertexParamsSize);
    SDL_PushGPUFragmentUniformData(cmd, 0, fragmentParams, fragmentParamsSize);
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, buffers->drawArgs, 0, renderSet->numGroups * MESH_LOD_COUNT);
}

void RenderSceneForward(SDL_GPUCommandBuffer* cmd, const ScenePassContext* ctx, u32 width, u32 height, u32 tilesX, bool localLightsEnabled)
{
    Scene* scene = g_ActiveScene;
    u32 totalGroups = scene->skinnedSet.numGroups + scene->surfaceSet.numGroups + scene->transparentSet.numGroups;
    if (totalGroups == 0 && g_NumTerrainChunkDraws == 0)
        return;

    struct {
        mat4x4 viewProj;
        float cameraPosition[4];
        float cameraForward[4];
    } vertexParams;
    vertexParams.viewProj = ctx->viewProj;
    vertexParams.cameraPosition[0] = g_Camera.position.x;
    vertexParams.cameraPosition[1] = g_Camera.position.y;
    vertexParams.cameraPosition[2] = g_Camera.position.z;
    vertexParams.cameraPosition[3] = 0.0f;
    vertexParams.cameraForward[0] = g_Camera.Front.x;
    vertexParams.cameraForward[1] = g_Camera.Front.y;
    vertexParams.cameraForward[2] = g_Camera.Front.z;
    vertexParams.cameraForward[3] = 0.0f;

    float3 sunDirection = GetRenderSunDirection();
    struct {
        f32 sunDirection[4];
        f32 cameraPosition[4];
        u32 outputSize[2];
        u32 tilesX;
        u32 tileSize;
        u32 localLightsEnabled;
        u32 pad0[3];
    } fragmentParams = {0};
    fragmentParams.sunDirection[0] = sunDirection.x;
    fragmentParams.sunDirection[1] = sunDirection.y;
    fragmentParams.sunDirection[2] = sunDirection.z;
    fragmentParams.cameraPosition[0] = g_Camera.position.x;
    fragmentParams.cameraPosition[1] = g_Camera.position.y;
    fragmentParams.cameraPosition[2] = g_Camera.position.z;
    fragmentParams.outputSize[0] = width;
    fragmentParams.outputSize[1] = height;
    fragmentParams.tilesX = tilesX;
    fragmentParams.tileSize = FORWARD_TILE_SIZE;
    fragmentParams.localLightsEnabled = localLightsEnabled ? 1u : 0u;

    SDL_GPUTextureSamplerBinding fragmentSamplers[8] = {
        { .texture = scene->textureSystem.classes[TextureClass_Albedo].pages.handle, .sampler = g_RenderState.sampler },
        { .texture = scene->textureSystem.classes[TextureClass_Normal].pages.handle, .sampler = g_RenderState.sampler },
        { .texture = scene->textureSystem.classes[TextureClass_MetallicRoughness].pages.handle, .sampler = g_RenderState.sampler },
        { .texture = g_WindowState.tex_shadow_color, .sampler = g_RenderState.shadowSampler },
        { .texture = g_WindowState.tex_point_shadow_color, .sampler = g_RenderState.shadowSampler },
        { .texture = g_WindowState.tex_spot_shadow_color, .sampler = g_RenderState.shadowSampler },
        { .texture = g_WindowState.tex_hbao_blur, .sampler = g_RenderState.sampler },
        { .texture = g_WindowState.tex_contact_shadow, .sampler = g_RenderState.sampler }
    };
    SDL_GPUBuffer* fragmentBuffers[7] = {
        scene->textureSystem.materialBuffer,
        scene->textureSystem.descriptorBuffer,
        g_RenderState.lightBuffer,
        g_RenderState.lightGridBuffer,
        g_RenderState.lightIndexBuffer,
        g_RenderState.pointShadowMatrixBuffer,
        g_RenderState.spotShadowMatrixBuffer
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, ctx->colorTargets, ctx->numColorTargets, ctx->depthTarget);

    const SDL_GPUBufferBinding skinnedVertex = { g_RenderState.skinned.vertexBuffer, 0 };
    DrawRenderBufferForward(cmd, pass, true, scene, &scene->skinnedSet, &scene->skinnedBuffers,
                            g_RenderState.skinned.forwardPipeline, skinnedVertex, fragmentSamplers, fragmentBuffers,
                            &vertexParams, sizeof(vertexParams), &fragmentParams, sizeof(fragmentParams));

    const SDL_GPUBufferBinding surfaceVertex = { g_RenderState.surface.vertexBuffer, 0 };
    DrawRenderBufferForward(cmd, pass, false, scene, &scene->surfaceSet, &scene->surfaceBuffers,
                            g_RenderState.surface.forwardPipeline, surfaceVertex, fragmentSamplers, fragmentBuffers,
                            &vertexParams, sizeof(vertexParams), &fragmentParams, sizeof(fragmentParams));

    RenderTerrain(cmd, pass, ctx->viewProj);
    Terrain_RenderGrass(cmd, pass);

    DrawRenderBufferForward(cmd, pass, false, scene, &scene->transparentSet, &scene->transparentBuffers,
                            g_RenderState.surface.transparentForwardPipeline, surfaceVertex, fragmentSamplers, fragmentBuffers,
                            &vertexParams, sizeof(vertexParams), &fragmentParams, sizeof(fragmentParams));

    SDL_EndGPURenderPass(pass);
}

void RenderLines(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget, SDL_GPUDepthStencilTargetInfo* depthTarget, mat4x4 viewProj)
{
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, colorTarget, 1, depthTarget);
    SDL_GPUBufferBinding vertex_binding = { g_RenderState.lineBuffer, 0 };
    SDL_BindGPUGraphicsPipeline(pass, g_RenderState.linePipeline);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &viewProj, sizeof(viewProj));
    SDL_DrawGPUPrimitivesIndirect(pass, g_RenderState.lineDrawArgsBuffer, sizeof(int) * 4, 1);
    SDL_EndGPURenderPass(pass);
}

// editor gizmo overlay, draws the lines the editor submitted this frame on top of
// everything (no depth target)
void RenderGizmo(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget, mat4x4 viewProj)
{
    if (g_NumGizmoVertices == 0 || !g_GizmoLinePipeline) return;

    UpdateGPUBufferCycle(g_RenderState.gizmoLineBuffer, g_GizmoVertices, g_NumGizmoVertices * sizeof(ALineVertex), 0, true);

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, colorTarget, 1, NULL);
    SDL_GPUBufferBinding vertex_binding = { g_RenderState.gizmoLineBuffer, 0 };
    SDL_BindGPUGraphicsPipeline(pass, g_GizmoLinePipeline);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &viewProj, sizeof(viewProj));
    SDL_DrawGPUPrimitives(pass, g_NumGizmoVertices, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

// every visible chunk in one indirect multidraw straight from the geometry heap's GPU
// mirror; the caller has already bound the terrain pipeline and pushed its uniforms
static void RenderTerrainChunkRanges(SDL_GPURenderPass* pass)
{
    if (g_NumTerrainChunkDraws == 0 || !g_RenderState.terrainVertexBuffer ||
        !g_RenderState.terrainIndexBuffer || !g_RenderState.terrainDrawArgsBuffer) return;

    SDL_GPUBufferBinding heapBinding = { g_RenderState.terrainVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &heapBinding, 1);
    SDL_GPUBufferBinding indexBinding = { g_RenderState.terrainIndexBuffer, 0 };
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, g_RenderState.terrainDrawArgsBuffer, 0, g_NumTerrainChunkDraws);
}

void RenderTerrain(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj)
{
    if (g_NumTerrainChunkDraws == 0 || !g_TerrainTrianglePipeline) return;
    SDL_GPUTexture* albedo = NULL;
    SDL_GPUTexture* normal = NULL;
    SDL_GPUTexture* arm = NULL;
    if (!Terrain_GetMaterialTextures(&albedo, &normal, &arm)) return;

    SDL_BindGPUGraphicsPipeline(pass, g_TerrainTrianglePipeline);
    SDL_GPUTextureSamplerBinding samplers[3] = {
        { .texture = albedo, .sampler = g_RenderState.sampler },
        { .texture = normal, .sampler = g_RenderState.sampler },
        { .texture = arm,    .sampler = g_RenderState.sampler }
    };
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, SDL_arraysize(samplers));
    SDL_PushGPUVertexUniformData(cmd, 0, &viewProj, sizeof(viewProj));
    float3 sunDirection = GetRenderSunDirection();
    struct {
        f32 brushPosRadius[4];
        f32 sunDirection[4];
    } fragmentParams = {0};
    fragmentParams.brushPosRadius[0] = g_TerrainBrushPosRadius[0];
    fragmentParams.brushPosRadius[1] = g_TerrainBrushPosRadius[1];
    fragmentParams.brushPosRadius[2] = g_TerrainBrushPosRadius[2];
    fragmentParams.brushPosRadius[3] = g_TerrainBrushPosRadius[3];
    fragmentParams.sunDirection[0] = sunDirection.x;
    fragmentParams.sunDirection[1] = sunDirection.y;
    fragmentParams.sunDirection[2] = sunDirection.z;
    SDL_PushGPUFragmentUniformData(cmd, 0, &fragmentParams, sizeof(fragmentParams));
    RenderTerrainChunkRanges(pass);
}

void RenderTerrainTrianglesDepth(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, mat4x4 viewProj)
{
    if (g_NumTerrainChunkDraws == 0 || !g_TerrainTriangleDepthPipeline) return;

    SDL_BindGPUGraphicsPipeline(pass, g_TerrainTriangleDepthPipeline);
    SDL_PushGPUVertexUniformData(cmd, 0, &viewProj, sizeof(viewProj));
    RenderTerrainChunkRanges(pass);
}

// re-draws every selected primitive as a grown inverted hull on top of the lit scene
void RenderOutline(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget, SDL_GPUDepthStencilTargetInfo* depthTarget, mat4x4 viewProj)
{
    if (g_NumOutlineTargets == 0 || !g_OutlinePipeline) return;
    Scene* scene = Scene_GetActive();
    if (!scene) return;

    SDL_GPURenderPass* pass = NULL;
    for (u32 t = 0; t < g_NumOutlineTargets; t++)
    {
        const OutlineTarget* target = &g_OutlineTargets[t];
        if (target->skinned) continue; // skinned hull needs the animated vertices, not supported yet

        const RenderSet* set = &scene->surfaceSet;
        if (target->groupIdx >= set->numGroups) continue;
        const PrimitiveGroup* group = &set->primitiveGroups[target->groupIdx];
        if (target->entityIdx >= group->numEntities || group->lodNumIndices[0] == 0) continue;

        const Entity* entity = &set->entities[group->entityOffset + target->entityIdx];
        v128f rotation = VecNorm(UnpackQuaternionS16Norm1(entity->rotation));
        v128f scale = EntityUnpackWorldScale(entity->scale);

        struct { mat4x4 viewProj; float position[4]; float rotationQ[4]; float scaleBias[4]; float aabbMin[4]; float aabbMax[4]; } params;
        params.viewProj = viewProj;
        VecStore(params.position, entity->position);
        VecStore(params.rotationQ, rotation);
        params.scaleBias[0] = VecGetX(scale);
        params.scaleBias[1] = VecGetY(scale);
        params.scaleBias[2] = VecGetZ(scale);
        // constant world thickness like the old engine's 0.04 normal bias
        params.scaleBias[3] = 0.04f / Maxf32(VecGetX(scale), 1.0e-4f);
        // primitive AABB to de-quantize the unorm16 vertex position in the shader
        VecStore(params.aabbMin, group->aabbMin);
        VecStore(params.aabbMax, group->aabbMax);

        if (!pass)
        {
            pass = SDL_BeginGPURenderPass(cmd, colorTarget, 1, depthTarget);
            SDL_GPUBufferBinding vertex_binding = { g_RenderState.surface.vertexBuffer, 0 };
            SDL_GPUBufferBinding index_binding = { g_RenderState.indexBuffer, 0 };
            SDL_BindGPUGraphicsPipeline(pass, g_OutlinePipeline);
            SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
            SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        SDL_PushGPUVertexUniformData(cmd, 0, &params, sizeof(params));
        SDL_DrawGPUIndexedPrimitives(pass, group->lodNumIndices[0], 1, group->lodIndexOffset[0], 0, 0);
    }
    if (pass) SDL_EndGPURenderPass(pass);
}
