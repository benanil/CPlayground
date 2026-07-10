# Terrain System — Architecture & Call Flow

The runtime terrain is the C port of the Unity transvoxel library (`TransvoxelUnity*.c`),
driven by `TransvoxelUnityExample.c` as the world manager. The old builtin streaming
runtime in `Terrain.c` is compiled out (`TERRAIN_OLD_RUNTIME_DISABLED`), but `Terrain.c`
still owns the public `Terrain_*` API: its stub branches delegate to the port, the edit
storage, and the grass system.

## File map

| File | Role |
|---|---|
| `TransvoxelUnityExample.c` | World manager: chunk cache, LOD quadtree, builds, draws, physics sync, brush highlight |
| `TransvoxelUnityMesher.c` | Transvoxel mesher: regular cells + 6 transition cell strips + secondary vertices |
| `TransvoxelUnityMesh.c` | `tMeshData` storage, secondary-vertex apply, degenerate index filtering |
| `TransvoxelUnityTables.c` | Lengyel's transvoxel lookup tables |
| `TransvoxelUnity.c/Octree/Jobs/Tasks/Updates/Scheduler/Pool/World/VoxelWorld/Collision.c` | Full async `tVoxelWorld` pipeline from the Unity port — **currently unused** by the example path (kept for a future async upgrade) |
| `Terrain.c` | Public `Terrain_*` API stubs → port + edits + grass. Grass runtime lives here (tiles, jobs, GPU buffers, pipelines) |
| `TerrainDensity.c` | Procedural SDF field (noise, island mask, bedrock), thread safe, pure |
| `TerrainEdit.c` | Sparse sculpt/paint storage: 16³ chunks of s8 density deltas + u16 packed materials, mutex-guarded, `.chunks` persistence |
| `TerrainInternal.h` | Shared constants (`TERRAIN_MAX_VERTICES`, SDF clamp, chunk sizes), density/edit APIs |
| `Editor/TerrainEditor.c` | Editor UI: brush raycast, sculpt/paint calls, gen param UI, save/load — talks only to `Terrain_*` |

## Per-frame flow (who calls who)

```
Main.c game loop
│
├─ tTransvoxelExampleUpdate()                         [TransvoxelUnityExample.c]
│  │   (Terrain_Update is commented out — old runtime disabled)
│  │
│  ├─ tExamplePromotePendingMeshes()      swap 2-frame-old pending meshes live,
│  │                                      free old heap block, mark physicsDirty
│  ├─ tExampleSubmitTerrain(lodFactor, frustum)
│  │  └─ tExampleSubmitNode(x, z, rootLOD)            ← recursive quadtree
│  │     ├─ split test: closest-point dist < lodDistance[lod-1] * lodFactor
│  │     │  ├─ children ready → recurse into 4 children
│  │     │  └─ children building → keep drawing this coarse node (fallback)
│  │     │     (mirror fallback: coarse missing → draw cached finer children)
│  │     ├─ tExampleGetChunk(min, lod)                ← per column Y chunk
│  │     │  ├─ tExampleColumnMask()                   neighboursMask from quadtree
│  │     │  │  └─ tExampleNeighbourFiner() → tExampleNodeSplits()
│  │     │  ├─ mask changed / dirty → tExampleBuildChunk()   (≤16 builds/frame)
│  │     │  │  ├─ tExampleBuildDensity()
│  │     │  │  │  └─ TerrainDensity_SampleChunk()     19³ s8 samples   [TerrainDensity.c]
│  │     │  │  │     └─ TerrainEdit_OverlayChunk()    sculpt deltas    [TerrainEdit.c]
│  │     │  │  ├─ tTransvoxelMesherMesh()                              [TransvoxelUnityMesher.c]
│  │     │  │  │  ├─ tMesherRegular()                 marching cells, secondary verts
│  │     │  │  │  └─ tMesherTransition() ×6           transition strips (all baked)
│  │     │  │  │     (mid-edge refinement re-samples tExampleDensityNoise →
│  │     │  │  │      TerrainDensity_At = SDF + TerrainEdit_DeltaAt)
│  │     │  │  ├─ tMeshDataContainerApplySecondaryVertices(mask)      [TransvoxelUnityMesh.c]
│  │     │  │  │     snap boundary verts inward on faces with finer neighbour
│  │     │  │  ├─ tExampleAppendMeshSlotTriangles(Main + masked transition slots)
│  │     │  │  │  └─ tExampleTerrainColor()           procedural blend + paint palette
│  │     │  │  │     └─ TerrainEdit_MaterialWeights() painted layers  [TerrainEdit.c]
│  │     │  │  ├─ GeometryHeapAlloc(TerrainVertex)    park soup in shared heap
│  │     │  │  ├─ MemCopy → gGFX.TerrainVertexBuffer  CPU mirror      [Graphics.c]
│  │     │  │  └─ Rendering_QueueGeometryUpload()     mark GPU range dirty
│  │     │  │        → stored in chunk's PENDING slot (2-frame promote delay)
│  │     │  └─ frustum cull (CheckAABBCulled, reversed-Z)
│  │     └─ tExampleAppendChunkTriangles(chunk)
│  │        ├─ normal chunk  → push TerrainChunkDraw{heapFirst, vertexCount}
│  │        └─ brush-touched → copy verts from CPU mirror + tExampleBrushTint → soup
│  ├─ tExampleSyncDirtyPhysics()          ≤2 chunks/frame, round robin
│  │  └─ tExampleSyncChunkPhysics() → Scene_PhysicsSyncTerrainChunkMesh()  (box3d, lod ≤ 1)
│  ├─ RendererSetTerrainTriangles(soup)               brush chunks only
│  └─ RendererSetTerrainChunkDraws(ranges)            static chunk heap ranges
│
└─ Render()                                           [Rendering.c]
   ├─ UploadDirtyGeometry()               CPU mirrors → GPU buffers (incl.
   │                                      terrainChunkVertexBuffer, slot 3)
   ├─ Terrain_GPUFlush(cmd)                           [Terrain.c]
   │  └─ port branch: TerrainPortEnsureGrass() lazy init,
   │     TerrainGrassConsume / TerrainGrassDispatch / TerrainGrassBuildDrawArgs
   └─ forward pass                                    [RenderingDraw.c]
      ├─ RenderTerrainTrianglesDepth()    depth prepass (soup + chunk ranges)
      ├─ RenderTerrainTriangles()
      │  ├─ soup draw (brush chunks)
      │  └─ RenderTerrainChunkRanges()    bind terrainChunkVertexBuffer,
      │                                   SDL_DrawGPUPrimitives per chunk range
      └─ Terrain_RenderGrass()            indirect multidraw, instance-rate blades
```

## Chunk lifecycle

```
              tExampleGetChunk (miss)
                     │ build budget ok?
                     ▼
   ┌──────────── BUILD ────────────┐    density 19³ → mesher → color → heap
   │ pending slot: pendingHeapPtr  │    old mesh (if any) keeps drawing
   │ pendingFrames = 2             │
   └──────────────┬────────────────┘
                  ▼  tExamplePromotePendingMeshes (2 frames later, GPU upload landed)
   ┌──────────── LIVE ─────────────┐    heapPtr/heapFirst/vertexCount
   │ drawn as TerrainChunkDraw     │    physicsDirty → box3d sync (lod ≤ 1)
   └──────────────┬────────────────┘
                  │ dirty (sculpt/paint invalidate, neighboursMask change)
                  ▼
              rebuild in place (same slot, pending again) — no flash
```

Chunks are never evicted individually; the cache resets wholesale at
`T_EXAMPLE_MAX_CHUNKS` or on `tTransvoxelExampleInvalidateAll` (gen param change,
world delete).

## LOD & seams

- 4 LODs, world chunk sizes 16/32/64/128 m, split rings at 48/112/240/448 m × `terrainLodFactor`.
- Quadtree per XZ column; Y chunks stack inside `TerrainDensity_GetYRange`.
- Two-way fallback keeps coverage during transitions: parent draws while children
  build; cached children draw while a returning parent builds. `tExampleChunkPresentable`
  gates both (pending-promote chunks are not presentable).
- Seam stitching (transvoxel): transition cells live on the **coarse** chunk in this
  port. `tExampleColumnMask` sets a face bit when that neighbour renders finer; the
  build then snaps boundary vertices to their secondary positions (half-cell inward,
  slid along the surface tangent) and appends that face's transition strip. Mask
  changes mark the chunk dirty for a budgeted rebuild.
- Fixed-area worlds (`genParams.fixedArea`) are lod0-only: no quadtree, masks stay 0.

## Editor path

```
TerrainEditor.c (UI, unmodified)
│
├─ Terrain_RaycastField()        sphere-march TerrainDensity_At → brush hit point
├─ Terrain_SetBrushCursor()    → tTransvoxelExampleSetBrushCursor (per-frame tint)
├─ Terrain_SculptSphere()      → TerrainEdit_SculptSphere (s8 delta grid)
│                                → tTransvoxelExampleInvalidateRegion (dirty chunks)
│                                → dirty touched grass columns
├─ Terrain_PaintSphere()       → TerrainEdit_PaintSphere (layer+1, two-slot blend)
│                                → invalidate region (colors bake at build)
├─ Terrain_CreateWorld/ApplyGenParams → TerrainDensity_SetParams + InvalidateAll
│                                       + free all grass tiles
├─ Terrain_DeleteWorld         → disable + TerrainEdit_Clear + InvalidateAll
└─ Terrain_SaveWorld/LoadWorld → .terrain text params + TerrainEdit_Save/LoadChunks
                                 (.chunks binary sidecar, one record per 16³ region)
```

Edits persist as data, never as meshes: every chunk rebuild re-samples
`TerrainDensity_At = TerrainDensity_SDF + TerrainEdit_DeltaAt`, so sculpt shows up
wherever a chunk remeshes, at any lod.

## GPU data path

- One shared vertex heap: `GeometryHeapAlloc/Free` (tlsf) over the
  `gGFX.TerrainVertexBuffer` CPU mirror (`TERRAIN_MAX_VERTICES = 4M` × 16 B = 64 MB).
  Port meshes are non-indexed triangle soup in `ALineVertex` (pos + color, 16 B,
  same stride as `TerrainVertex`).
- `Rendering_QueueGeometryUpload` marks dirty ranges; `UploadDirtyGeometry` copies
  them to the GPU mirror `g_RenderState.terrainChunkVertexBuffer` once per frame.
- Static chunks draw as `TerrainChunkDraw{first, count}` ranges from the mega buffer —
  no per-frame vertex copies. Only brush-highlighted chunks copy through the per-frame
  soup path (tinted).
- Lighting is baked into vertex color at build time (lambert + sky in
  `tExampleTerrainColor`); the terrain pipeline just draws colored triangles.

## Grass

Self-contained slice in `Terrain.c`, revived for the port:

- Lazy init on first `Terrain_GPUFlush` with world enabled + `grassViewDistance > 0`.
- CPU: job pool marches `TerrainDensity_SurfaceY` per tile → `GrassInstance` blades
  (8 B, chunk-relative fp16). Sculpt dirties touched columns; param changes drop all tiles.
- GPU: consume finished tiles → compute cull/compact → indirect multidraw in the
  forward pass right after the terrain surface. `first_instance` offsets an
  instance-rate vertex buffer (SDL GPU: `first_instance` is not in `SV_InstanceID`).

## Physics

- Only lod 0–1 chunks get colliders (`MAX_TERRAIN_PHYSICS_CHUNKS` slots, keyed by
  chunk index). ≤2 syncs/frame round-robin over `physicsDirty` chunks.
- Vertices read back from the heap CPU mirror; zero-area triangles filtered before
  `Scene_PhysicsSyncTerrainChunkMesh` (box3d static triangle mesh).

## Known gaps

- `tVoxelWorld` async pipeline (octree + job tasks) is ported but not wired; the
  example path builds synchronously on the main thread under a 16-builds/frame budget.
- Heap-full recovery is warn + drop chunk (retries next frame).
- Grass first run pays a PNG decode for the blade texture.
