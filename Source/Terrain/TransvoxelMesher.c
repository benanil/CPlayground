// Transvoxel mesher (Eric Lengyel, https://transvoxel.org/): regular marching cubes
// cells with the modified tables plus transition cells stitching faces toward coarser
// neighbors. runs on worker threads: only touches its inputs, the scratch buffers and
// SDL_malloc. vertices come out in the compact chunk relative TerrainVertex format.
//
// vertex reuse is done with direct lookup arrays keyed by global edge/corner ids instead
// of Lengyel's per-deck history nibbles: same dedup result, much simpler to validate.
//
// crack free seams: edge interpolation runs in 8.8 fixed point (t256) and undisplaced
// vertices quantize with pure integer math, so the two chunks sharing an edge (same or
// neighboring lod) produce bit-identical packed positions. only vertices moved by the
// transition shrink go through the float path, and those never need cross-chunk matching.
#include "TerrainInternal.h"
#include "TransvoxelTables.h"
#include "Include/Terrain.h"  // TerrainGenParams (sea level for the beach layer)
#include "Include/Platform.h" // AX_LOG / AX_ERROR
#include "Math/Vector.h"
#include "Math/Quaternion.h" // Bitpack.h needs QuaternionFromM33Vec
#include "Math/Bitpack.h"
#include <SDL3/SDL_stdinc.h>

#define TV_CORNERS_AXIS  (TERRAIN_CHUNK_CORNERS)                       // 17
#define TV_CORNER_COUNT  (TV_CORNERS_AXIS * TV_CORNERS_AXIS * TV_CORNERS_AXIS)
#define TV_EDGE_COUNT    (3 * TV_CORNER_COUNT)
#define TV_FACEHASH_SIZE 2048u // power of two, > 4x worst case face vertex count
#define TV_NO_VERTEX     0xFFFFFFFFu

// terrain material layers (texture array indices): grass on flat, dirt on slope / low
// ground, high rock up top, sand at the water line. the stack low->high is sand -> dirt ->
// dirt/grass, so sand only ever blends into dirt and grass stays off the coast.
#define TV_GRASS_LAYER   0u
#define TV_DIRT_LAYER    1u
#define TV_SAND_LAYER    3u
#define TV_SAND_TOP      2.0f  // meters above sea level where sand fully fades to dirt
#define TV_SAND_FADE     2.0f  // sand->dirt vertical blend band, meters
#define TV_GRASS_START   3.0f  // meters above sea level where grass starts fading in
#define TV_GRASS_FADE    8.0f  // dirt->grass vertical blend band, meters

static s32 TVSampleIdx(s32 cx, s32 cy, s32 cz) // corner coords -1..17
{
    return ((cz + 1) * TERRAIN_SAMPLES_AXIS + (cy + 1)) * TERRAIN_SAMPLES_AXIS + (cx + 1);
}

static s32 TVDensity(const s8* d, s32 cx, s32 cy, s32 cz)
{
    return (s32)d[TVSampleIdx(cx, cy, cz)];
}

// central differences on the sample grid, points from solid (negative) to empty
static v128f TVGradient(const s8* d, const s32 p[3])
{
    return VecSetR(
        (f32)(TVDensity(d, p[0] + 1, p[1], p[2]) - TVDensity(d, p[0] - 1, p[1], p[2])),
        (f32)(TVDensity(d, p[0], p[1] + 1, p[2]) - TVDensity(d, p[0], p[1] - 1, p[2])),
        (f32)(TVDensity(d, p[0], p[1], p[2] + 1) - TVDensity(d, p[0], p[1], p[2] - 1)),
        0.0f);
}

static v128f TVNormalSafe(v128f g)
{
    f32 lenSq = Vec3DotfV(g, g);
    if (lenSq < 1e-12f) return VecSetR(0.0f, 1.0f, 0.0f, 0.0f);
    return VecMulf(g, 1.0f / Sqrtf(lenSq));
}

// ---------------------------------------------------------------------------------
// output buffers
// ---------------------------------------------------------------------------------

static s32 TVEnsure(void** buf, u32* capacity, u32 needed, u32 stride)
{
    if (needed <= *capacity) return 1;
    u32 newCap = *capacity ? *capacity * 2u : 1024u;
    while (newCap < needed) newCap *= 2u;
    void* grown = SDL_realloc(*buf, (size_t)newCap * stride);
    if (!grown) return 0;
    *buf = grown;
    *capacity = newCap;
    return 1;
}

// octahedral encode to 16+16 unorm, matching TerrainDecodeNormal in TerrainForward.hlsl
static u32 TVOctEncode16(v128f n)
{
    v128f oct = OctEncode(n);
    u32 qx = (u32)(Saturatef32(VecGetX(oct) * 0.5f + 0.5f) * 65535.0f + 0.5f);
    u32 qy = (u32)(Saturatef32(VecGetY(oct) * 0.5f + 0.5f) * 65535.0f + 0.5f);
    return qx | (qy << 16);
}

// q is the fixed point position, TERRAIN_POS_PER_CELL steps per cell
static u32 TVEmitVertex(TerrainMeshOut* out, const u32 q[3], v128f normal, f32 voxelSize)
{
    if (!TVEnsure((void**)&out->vertices, &out->vertexCapacity, out->numVertices + 1u, sizeof(TerrainVertex)))
        return TV_NO_VERTEX;

    TerrainVertex* v = &out->vertices[out->numVertices];
    v->posA      = q[0] | (q[1] << 21);            // y bits above 11 fall off intentionally
    v->posB      = (q[1] >> 11) | (q[2] << 10);
    v->octNormal = TVOctEncode16(normal);

    // material selection happens here once, the shader only blends what the vertex
    // says: spare = layerA | layerB<<8 | weightA<<16 | weightB<<24 with actual
    // texture array layers. unpainted ground resolves the procedural slope/height
    // choice (formerly per pixel in TerrainForward.hlsl) from the vertex normal and height
    f32 metersPerStep = (f32)(1 << out->lod) * (1.0f / (f32)TERRAIN_POS_PER_CELL);
    float3 wpos = {
        (f32)out->worldOrigin[0] + (f32)q[0] * metersPerStep,
        (f32)out->worldOrigin[1] + (f32)q[1] * metersPerStep,
        (f32)out->worldOrigin[2] + (f32)q[2] * metersPerStep
    };

    // procedural land pick: dirt is the default ground; grass only takes over where the
    // surface is flat AND well above the water line, high rock up top. so the coast is bare
    // dirt (which sand blends into) and grass never touches sea level.
    f32 seaLevel = TerrainDensity_GetParams()->seaLevel;
    f32 slope = VecGetY(normal);
    f32 rockBlend = 1.0f - Saturatef32((slope - 0.55f) * 4.0f);           // 0 flat, 1 steep
    f32 highBlend = Saturatef32((wpos.y - 34.0f) * 0.08f);
    f32 grassHeight = Saturatef32((wpos.y - seaLevel - TV_GRASS_START) * (1.0f / TV_GRASS_FADE));
    f32 grassAmount = (1.0f - rockBlend) * grassHeight;                   // flat AND high -> grass
    float3 layerWeight = {
        grassAmount * (1.0f - highBlend),
        (1.0f - grassAmount) * (1.0f - highBlend), // dirt is the land default
        highBlend
    };
    u32 procA = layerWeight.y > layerWeight.x ? TV_DIRT_LAYER : TV_GRASS_LAYER;
    u32 procB = procA == TV_DIRT_LAYER ? (layerWeight.z > layerWeight.x ? 2u : TV_GRASS_LAYER)
                                       : (layerWeight.z > layerWeight.y ? 2u : TV_DIRT_LAYER);
    f32 procWeightAF = procA == 0u ? layerWeight.x : procA == 1u ? layerWeight.y : layerWeight.z;
    f32 procWeightBF = procB == 0u ? layerWeight.x : procB == 1u ? layerWeight.y : layerWeight.z;
    f32 procTotal = procWeightAF + procWeightBF;
    u32 procWeightA = (u32)Clampf32(procWeightAF / Maxf32(procTotal, 1e-4f) * 255.0f + 0.5f, 0.0f, 255.0f);

    // sand takes over at and below the water line, blending up into the dirt band (never
    // straight into grass, which is already gated above the coast)
    f32 sandBlend = Saturatef32((seaLevel + TV_SAND_TOP - wpos.y) * (1.0f / TV_SAND_FADE));

    u8 matIndex[2], matWeight[2];
    TerrainEdit_MaterialWeights(wpos, matIndex, matWeight);

    // paint slots: index 0 means "procedural here", resolve it to the dominant
    // procedural layer. painted indices are layer + 1, clamp to the loaded array
    u32 procDominant = procWeightA >= 128u ? procA : procB;
    u32 idxA, idxB, wA;
    if (matIndex[0] == 0 && matIndex[1] == 0)
    {
        if (sandBlend >= 0.998f)      { idxA = TV_SAND_LAYER; idxB = TV_SAND_LAYER; wA = 255u; }
        else if (sandBlend > 0.002f)  { idxA = TV_SAND_LAYER; idxB = TV_DIRT_LAYER;
                                        wA = (u32)(sandBlend * 255.0f + 0.5f); }
        else                          { idxA = procA; idxB = procB; wA = procWeightA; }
    }
    else
    {
        idxA = matIndex[0] == 0 ? procDominant : Minu32((u32)matIndex[0] - 1u, 3u);
        idxB = matIndex[1] == 0 ? procDominant : Minu32((u32)matIndex[1] - 1u, 3u);
        wA = matWeight[0];
    }
    v->spare = idxA | (idxB << 8) | (wA << 16) | ((255u - wA) << 24);

    const f32 toMeters = voxelSize / (f32)TERRAIN_POS_PER_CELL;
    f32 mx = (f32)q[0] * toMeters, my = (f32)q[1] * toMeters, mz = (f32)q[2] * toMeters;
    if (out->numVertices == 0)
    {
        out->aabbMin = out->aabbMax = (float3){ mx, my, mz };
    }
    else
    {
        out->aabbMin.x = Minf32(out->aabbMin.x, mx); out->aabbMax.x = Maxf32(out->aabbMax.x, mx);
        out->aabbMin.y = Minf32(out->aabbMin.y, my); out->aabbMax.y = Maxf32(out->aabbMax.y, my);
        out->aabbMin.z = Minf32(out->aabbMin.z, mz); out->aabbMax.z = Maxf32(out->aabbMax.z, mz);
    }
    return out->numVertices++;
}

static s32 TVEmitTriangle(TerrainMeshOut* out, u32 a, u32 b, u32 c)
{
    if (a == b || b == c || a == c) return 1; // degenerate after dedup, drop
    if (!TVEnsure((void**)&out->indices, &out->indexCapacity, out->numIndices + 3u, sizeof(u32)))
        return 0;
    out->indices[out->numIndices + 0] = a;
    out->indices[out->numIndices + 1] = b;
    out->indices[out->numIndices + 2] = c;
    out->numIndices += 3u;
    return 1;
}

// ---------------------------------------------------------------------------------
// secondary positions: vertices on an active transition face move inward so the
// transition cells can fill the gap. the offset is projected onto the plane
// perpendicular to the vertex normal to avoid shading distortion (Lengyel 4.4).
// out: true when the position moved (such vertices take the float quantize path)
// ---------------------------------------------------------------------------------

static bool TVSecondary(v128f* ioPos, v128f n, u8 transitionMask, f32 scale)
{
    if (scale == 0.0f || transitionMask == 0) return false;
    const f32 maxCoord = (f32)TERRAIN_CHUNK_CELLS;
    f32 px = VecGetX(*ioPos), py = VecGetY(*ioPos), pz = VecGetZ(*ioPos);
    f32 dx = 0.0f, dy = 0.0f, dz = 0.0f;
    if ((transitionMask & 0x01) && px == 0.0f)     dx += TERRAIN_TRANSITION_SHRINK;
    if ((transitionMask & 0x02) && px == maxCoord) dx -= TERRAIN_TRANSITION_SHRINK;
    if ((transitionMask & 0x04) && py == 0.0f)     dy += TERRAIN_TRANSITION_SHRINK;
    if ((transitionMask & 0x08) && py == maxCoord) dy -= TERRAIN_TRANSITION_SHRINK;
    if ((transitionMask & 0x10) && pz == 0.0f)     dz += TERRAIN_TRANSITION_SHRINK;
    if ((transitionMask & 0x20) && pz == maxCoord) dz -= TERRAIN_TRANSITION_SHRINK;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return false;
    v128f delta = VecMulf(VecSetR(dx, dy, dz, 0.0f), scale);
    delta  = VecSub(delta, VecMulf(n, Vec3DotfV(n, delta))); // project onto the surface tangent plane
    *ioPos = VecAdd(*ioPos, delta);
    return true;
}

// builds the vertex on the edge p0-p1 with 8.8 fixed point parameter t256. undisplaced
// vertices quantize exactly from integers, displaced ones round the float position
static u32 TVBuildVertex(TerrainMeshOut* out, const s8* density,
                         const s32 p0[3], const s32 p1[3], s32 t256,
                         u8 transitionMask, f32 displaceScale, f32 voxelSize)
{
    f32 t = (f32)t256 * (1.0f / 256.0f);
    v128f a = VecSetR((f32)p0[0], (f32)p0[1], (f32)p0[2], 0.0f);
    v128f b = VecSetR((f32)p1[0], (f32)p1[1], (f32)p1[2], 0.0f);
    v128f pos = VecLerp(a, b, t);
    v128f normal = TVNormalSafe(VecLerp(TVGradient(density, p0), TVGradient(density, p1), t));

    u32 q[3];
    if (TVSecondary(&pos, normal, transitionMask, displaceScale))
    {
        f32 buf[4];
        Vec3Store(buf, pos);
        for (s32 i = 0; i < 3; i++)
        {
            f32 v = buf[i] * (f32)TERRAIN_POS_PER_CELL + 0.5f;
            q[i] = v <= 0.0f ? 0u : (v >= (f32)TERRAIN_POS_MAX ? (u32)TERRAIN_POS_MAX : (u32)v);
        }
    }
    else
    {
        for (s32 i = 0; i < 3; i++)
            q[i] = (u32)(p0[i] * TERRAIN_POS_PER_CELL + (p1[i] - p0[i]) * t256 * (TERRAIN_POS_PER_CELL / 256));
    }
    return TVEmitVertex(out, q, normal, voxelSize);
}

// ---------------------------------------------------------------------------------
// regular cells
// ---------------------------------------------------------------------------------

// corner numbering per figure 3.7: x fastest
static const s32 TVCornerOffset[8][3] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
    { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
};

static s32 TVCornerId(s32 x, s32 y, s32 z)
{
    return (z * TV_CORNERS_AXIS + y) * TV_CORNERS_AXIS + x;
}

// builds (or reuses) the vertex on the edge between integer corners p0 and p1
static u32 TVRegularVertex(TerrainMeshOut* out, TerrainMeshScratch* scratch, const s8* density,
                           const s32 p0[3], const s32 p1[3], s32 d0, s32 d1,
                           u8 transitionMask, f32 voxelSize)
{
    u32* slot;
    s32 t256;

    if (d0 == 0 || d1 == 0)
    {
        const s32* p = (d0 == 0) ? p0 : p1;
        slot = &scratch->cornerVertex[TVCornerId(p[0], p[1], p[2])];
        t256 = (d0 == 0) ? 0 : 256;
    }
    else
    {
        s32 axis = (p0[0] != p1[0]) ? 0 : ((p0[1] != p1[1]) ? 1 : 2);
        slot = &scratch->edgeVertex[axis * TV_CORNER_COUNT + TVCornerId(p0[0], p0[1], p0[2])];
        t256 = (d0 << 8) / (d0 - d1);
    }
    if (*slot != TV_NO_VERTEX) return *slot;

    u32 idx = TVBuildVertex(out, density, p0, p1, t256, transitionMask, 1.0f, voxelSize);
    *slot = idx;
    return idx;
}

static s32 TVMeshRegularCells(TerrainMeshOut* out, TerrainMeshScratch* scratch,
                              const s8* density, u8 transitionMask, f32 voxelSize)
{
    for (s32 cz = 0; cz < TERRAIN_CHUNK_CELLS; cz++)
    for (s32 cy = 0; cy < TERRAIN_CHUNK_CELLS; cy++)
    for (s32 cx = 0; cx < TERRAIN_CHUNK_CELLS; cx++)
    {
        s32 corner[8][3];
        s32 d[8];
        u32 caseCode = 0;
        for (s32 i = 0; i < 8; i++)
        {
            corner[i][0] = cx + TVCornerOffset[i][0];
            corner[i][1] = cy + TVCornerOffset[i][1];
            corner[i][2] = cz + TVCornerOffset[i][2];
            d[i] = TVDensity(density, corner[i][0], corner[i][1], corner[i][2]);
            caseCode |= (u32)(d[i] < 0) << i;
        }
        if (caseCode == 0u || caseCode == 255u) continue;

        const RegularCellData* cell = &regularCellData[regularCellClass[caseCode]];
        s32 numVerts = TVCellVertexCount(*cell);
        s32 numTris  = TVCellTriangleCount(*cell);

        u32 cellVertex[12];
        for (s32 i = 0; i < numVerts; i++)
        {
            u16 edge = regularVertexData[caseCode][i];
            s32 c0 = (edge >> 4) & 0x0F;
            s32 c1 = edge & 0x0F;
            cellVertex[i] = TVRegularVertex(out, scratch, density, corner[c0], corner[c1],
                                            d[c0], d[c1], transitionMask, voxelSize);
            if (cellVertex[i] == TV_NO_VERTEX) return 0;
        }
        for (s32 t = 0; t < numTris; t++)
        {
            if (!TVEmitTriangle(out, cellVertex[cell->vertexIndex[t * 3 + 0]],
                                     cellVertex[cell->vertexIndex[t * 3 + 1]],
                                     cellVertex[cell->vertexIndex[t * 3 + 2]]))
                return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------------
// transition cells: one 8x8 grid of cells per face adjacent to a coarser neighbor.
// the full resolution side (samples 0-8) connects to the shrunk regular mesh, the
// half resolution side (corners 9-C) stays on the boundary plane and matches the
// coarse neighbor exactly because density quantization is world fixed
// ---------------------------------------------------------------------------------

// figure 4.16 sample layout on the face, in fine half-steps of the 2x2 cell
static const s32 TVTransSampleUV[9][2] = {
    { 0, 0 }, { 1, 0 }, { 2, 0 },
    { 0, 1 }, { 1, 1 }, { 2, 1 },
    { 0, 2 }, { 1, 2 }, { 2, 2 }
};
// corners 9, A, B, C map onto samples 0, 2, 6, 8
static const s32 TVTransCornerSample[4] = { 0, 2, 6, 8 };

// face u/v/w axes: w is the face normal axis, wCoord the boundary plane in cell units
typedef struct TVFace_
{
    s32 uAxis, vAxis, wAxis;
    s32 wCoord;
} TVFace;

static const TVFace TVFaces[6] = {
    { 1, 2, 0, 0  }, // -x
    { 1, 2, 0, 16 }, // +x
    { 0, 2, 1, 0  }, // -y
    { 0, 2, 1, 16 }, // +y
    { 0, 1, 2, 0  }, // -z
    { 0, 1, 2, 16 }, // +z
};

static void TVFaceToCorner(const TVFace* face, s32 u, s32 v, s32 p[3])
{
    p[face->uAxis] = u;
    p[face->vAxis] = v;
    p[face->wAxis] = face->wCoord;
}

// face-global ids for dedup: full resolution grid corners get 0..288, the half
// resolution (coarse) corners a distinct range so their undisplaced vertices stay separate
static u32 TVTransCornerKey(s32 cellU, s32 cellV, s32 k)
{
    if (k <= 8)
    {
        s32 u = cellU * 2 + TVTransSampleUV[k][0];
        s32 v = cellV * 2 + TVTransSampleUV[k][1];
        return (u32)(v * TV_CORNERS_AXIS + u);
    }
    s32 s = TVTransCornerSample[k - 9];
    s32 u = cellU + TVTransSampleUV[s][0] / 2;
    s32 v = cellV + TVTransSampleUV[s][1] / 2;
    return 512u + (u32)(v * 9 + u);
}

static u32* TVFaceHashSlot(u32* table, u32 key)
{
    u32 mask = TV_FACEHASH_SIZE - 1u;
    u32 h = (key * 2654435761u) & mask;
    for (;;)
    {
        u32* entry = &table[h * 2u];
        if (entry[0] == 0u || entry[0] == key + 1u) return entry;
        h = (h + 1u) & mask;
    }
}

static s32 TVMeshTransitionFace(TerrainMeshOut* out, TerrainMeshScratch* scratch,
                                const s8* density, u8 transitionMask, s32 faceIdx, f32 voxelSize)
{
    const TVFace* face = &TVFaces[faceIdx];
    SDL_memset(scratch->faceHash, 0, TV_FACEHASH_SIZE * 2u * sizeof(u32));

    for (s32 cellV = 0; cellV < TERRAIN_CHUNK_CELLS / 2; cellV++)
    for (s32 cellU = 0; cellU < TERRAIN_CHUNK_CELLS / 2; cellU++)
    {
        s32 d[13];
        s32 p[13][3];
        for (s32 i = 0; i < 9; i++)
        {
            TVFaceToCorner(face, cellU * 2 + TVTransSampleUV[i][0], cellV * 2 + TVTransSampleUV[i][1], p[i]);
            d[i] = TVDensity(density, p[i][0], p[i][1], p[i][2]);
        }
        for (s32 i = 0; i < 4; i++)
        {
            s32 s = TVTransCornerSample[i];
            p[9 + i][0] = p[s][0]; p[9 + i][1] = p[s][1]; p[9 + i][2] = p[s][2];
            d[9 + i] = d[s];
        }

        // 9 bit case code with Lengyel's bit order: perimeter 0,1,2,5,8,7,6,3 then center 4
        u32 caseCode = (u32)(d[0] < 0)        | ((u32)(d[1] < 0) << 1) | ((u32)(d[2] < 0) << 2)
                     | ((u32)(d[5] < 0) << 3) | ((u32)(d[8] < 0) << 4) | ((u32)(d[7] < 0) << 5)
                     | ((u32)(d[6] < 0) << 6) | ((u32)(d[3] < 0) << 7) | ((u32)(d[4] < 0) << 8);
        if (caseCode == 0u || caseCode == 511u) continue;

        u8 cellClass = transitionCellClass[caseCode];
        bool inverseClass = (cellClass & 0x80u) != 0;
        const TransitionCellData* cell = &transitionCellData[cellClass & 0x7F];
        s32 numVerts = TVCellVertexCount(*cell);
        s32 numTris  = TVCellTriangleCount(*cell);

        u32 cellVertex[12];
        for (s32 i = 0; i < numVerts; i++)
        {
            u16 edge = transitionVertexData[caseCode][i];
            s32 c0 = (edge >> 4) & 0x0F;
            s32 c1 = edge & 0x0F;
            s32 d0 = d[c0], d1 = d[c1];

            u32 key;
            s32 t256;
            if (d1 == 0)      { key = (TVTransCornerKey(cellU, cellV, c1) << 2) | 1u; t256 = 256; }
            else if (d0 == 0) { key = (TVTransCornerKey(cellU, cellV, c0) << 2) | 1u; t256 = 0; }
            else
            {
                u32 k0 = TVTransCornerKey(cellU, cellV, c0);
                u32 k1 = TVTransCornerKey(cellU, cellV, c1);
                u32 lo = k0 < k1 ? k0 : k1, hi = k0 < k1 ? k1 : k0;
                key = ((lo * 1024u + hi) << 2) | 2u;
                t256 = (d0 << 8) / (d0 - d1);
            }

            // weight of the displaced (full resolution) side: exact 1/0 when both
            // endpoints sit on the same side so the float paths match bit for bit
            bool full0 = c0 <= 8, full1 = c1 <= 8;
            f32 fullRes;
            if (full0 == full1) fullRes = full0 ? 1.0f : 0.0f;
            else
            {
                f32 t = (f32)t256 * (1.0f / 256.0f);
                fullRes = full0 ? (1.0f - t) : t;
            }

            u32* slot = TVFaceHashSlot(scratch->faceHash, key);
            if (slot[0] == key + 1u) { cellVertex[i] = slot[1]; continue; }

            u32 idx = TVBuildVertex(out, density, p[c0], p[c1], t256, transitionMask, fullRes, voxelSize);
            if (idx == TV_NO_VERTEX) return 0;
            slot[0] = key + 1u;
            slot[1] = idx;
            cellVertex[i] = idx;
        }
        for (s32 t3 = 0; t3 < numTris; t3++)
        {
            u32 i0 = cellVertex[cell->vertexIndex[t3 * 3 + 0]];
            u32 i1 = cellVertex[cell->vertexIndex[t3 * 3 + (inverseClass ? 2 : 1)]];
            u32 i2 = cellVertex[cell->vertexIndex[t3 * 3 + (inverseClass ? 1 : 2)]];
            if (!TVEmitTriangle(out, i0, i1, i2))
                return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------------

void Transvoxel_ScratchInit(TerrainMeshScratch* scratch)
{
    scratch->edgeVertex   = (u32*)SDL_malloc(TV_EDGE_COUNT * sizeof(u32));
    scratch->cornerVertex = (u32*)SDL_malloc(TV_CORNER_COUNT * sizeof(u32));
    scratch->faceHash     = (u32*)SDL_malloc(TV_FACEHASH_SIZE * 2u * sizeof(u32));
}

void Transvoxel_ScratchDestroy(TerrainMeshScratch* scratch)
{
    SDL_free(scratch->edgeVertex);
    SDL_free(scratch->cornerVertex);
    SDL_free(scratch->faceHash);
    SDL_memset(scratch, 0, sizeof(*scratch));
}

void Transvoxel_MeshOutDestroy(TerrainMeshOut* out)
{
    SDL_free(out->vertices);
    SDL_free(out->indices);
    SDL_memset(out, 0, sizeof(*out));
}

s32 Transvoxel_MeshChunk(const s8* density, u32 lod, u8 transitionMask,
                         s32 cx, s32 cy, s32 cz,
                         TerrainMeshOut* out, TerrainMeshScratch* scratch)
{
    if (!scratch->edgeVertex || !scratch->cornerVertex || !scratch->faceHash) return 0;

    const f32 voxelSize = TERRAIN_VOXEL_SIZE * (f32)(1u << lod);
    out->numVertices = 0;
    out->numIndices  = 0;
    out->aabbMin = F3Zero();
    out->aabbMax = F3Zero();
    out->worldOrigin[0] = cx * (TERRAIN_CHUNK_CELLS << lod);
    out->worldOrigin[1] = cy * (TERRAIN_CHUNK_CELLS << lod);
    out->worldOrigin[2] = cz * (TERRAIN_CHUNK_CELLS << lod);
    out->lod = lod;

    SDL_memset(scratch->edgeVertex,   0xFF, TV_EDGE_COUNT * sizeof(u32));
    SDL_memset(scratch->cornerVertex, 0xFF, TV_CORNER_COUNT * sizeof(u32));

    if (!TVMeshRegularCells(out, scratch, density, transitionMask, voxelSize)) return 0;

    for (s32 f = 0; f < 6; f++)
    {
        if (!(transitionMask & (1u << f))) continue;
        if (!TVMeshTransitionFace(out, scratch, density, transitionMask, f, voxelSize)) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------------
// self test: meshes an analytic sphere and validates vertex distance, watertightness
// and normal agreement. logs results, called once from Terrain_Init
// ---------------------------------------------------------------------------------

static s8 TVQuantizeSDF(f32 sdf)
{
    f32 q = Clampf32(sdf * (127.0f / TERRAIN_SDF_CLAMP), -127.0f, 127.0f);
    return (s8)(q >= 0.0f ? q + 0.5f : q - 0.5f);
}

static float3 TVUnpackPositionCells(TerrainVertex v)
{
    u32 qx = v.posA & 0x1FFFFFu;
    u32 qy = (v.posA >> 21) | ((v.posB & 0x3FFu) << 11);
    u32 qz = (v.posB >> 10) & 0x1FFFFFu;
    const f32 s = 1.0f / (f32)TERRAIN_POS_PER_CELL;
    return (float3){ (f32)qx * s, (f32)qy * s, (f32)qz * s };
}

void Transvoxel_SelfTest(void)
{
    s8* density = (s8*)SDL_malloc(TERRAIN_SAMPLES_TOTAL);
    TerrainMeshScratch scratch = {0};
    Transvoxel_ScratchInit(&scratch);
    TerrainMeshOut mesh = {0};
    u64* edges = NULL;

    const float3 center = { 8.0f, 8.0f, 8.0f };
    const f32 radius = 6.0f;
    for (s32 z = 0; z < TERRAIN_SAMPLES_AXIS; z++)
    for (s32 y = 0; y < TERRAIN_SAMPLES_AXIS; y++)
    for (s32 x = 0; x < TERRAIN_SAMPLES_AXIS; x++)
    {
        float3 p = { (f32)(x - 1), (f32)(y - 1), (f32)(z - 1) };
        density[(z * TERRAIN_SAMPLES_AXIS + y) * TERRAIN_SAMPLES_AXIS + x] =
            TVQuantizeSDF(F3Len(F3Sub(p, center)) - radius);
    }

    if (!Transvoxel_MeshChunk(density, 0, 0, 0, 0, 0, &mesh, &scratch))
    {
        AX_ERROR("terrain selftest: mesher failed");
        goto cleanup;
    }

    // vertex distance from the analytic surface
    f32 maxDist = 0.0f;
    for (u32 i = 0; i < mesh.numVertices; i++)
    {
        float3 p = TVUnpackPositionCells(mesh.vertices[i]);
        maxDist = Maxf32(maxDist, Absf32(F3Len(F3Sub(p, center)) - radius));
    }

    // watertight: every undirected edge of a closed surface is referenced exactly twice
    u32 numEdges = mesh.numIndices; // 3 edges per triangle
    u32 numTris = mesh.numIndices / 3u;
    edges = (u64*)SDL_malloc(numEdges * sizeof(u64));
    for (u32 t = 0; t < numTris; t++)
    {
        for (u32 e = 0; e < 3u; e++)
        {
            u64 a = mesh.indices[t * 3u + e];
            u64 b = mesh.indices[t * 3u + ((e + 1u) % 3u)];
            edges[t * 3u + e] = a < b ? (a << 32) | b : (b << 32) | a;
        }
    }
    for (u32 gap = numEdges / 2u; gap > 0u; gap /= 2u) // shell sort, no qsort dependency
    {
        for (u32 i = gap; i < numEdges; i++)
        {
            u64 tmp = edges[i];
            u32 j = i;
            for (; j >= gap && edges[j - gap] > tmp; j -= gap) edges[j] = edges[j - gap];
            edges[j] = tmp;
        }
    }
    u32 badEdges = 0;
    for (u32 i = 0; i < numEdges; )
    {
        u32 run = 1;
        while (i + run < numEdges && edges[i + run] == edges[i]) run++;
        if (run != 2u) badEdges++;
        i += run;
    }

    // triangle winding vs outward direction agreement (informational, terrain draws cull-off)
    u32 agree = 0;
    for (u32 t = 0; t < numTris; t++)
    {
        float3 a = TVUnpackPositionCells(mesh.vertices[mesh.indices[t * 3 + 0]]);
        float3 b = TVUnpackPositionCells(mesh.vertices[mesh.indices[t * 3 + 1]]);
        float3 c = TVUnpackPositionCells(mesh.vertices[mesh.indices[t * 3 + 2]]);
        float3 ab = F3Sub(b, a), ac = F3Sub(c, a);
        float3 geoN = F3Cross(&ab, &ac);
        float3 outward = F3Sub(a, center);
        if (F3Dot(geoN, outward) > 0.0f) agree++;
    }

    AX_LOG("terrain selftest: verts=%u tris=%u maxSurfaceErr=%.3f cells, nonManifoldEdges=%u, outwardWinding=%u/%u",
           mesh.numVertices, numTris, maxDist, badEdges, agree, numTris);
    if (mesh.numVertices == 0 || badEdges > 0 || maxDist > 0.2f)
        AX_ERROR("terrain selftest FAILED");

    // exercise the transition path with a plane crossing the side faces
    for (s32 z = 0; z < TERRAIN_SAMPLES_AXIS; z++)
    for (s32 y = 0; y < TERRAIN_SAMPLES_AXIS; y++)
    for (s32 x = 0; x < TERRAIN_SAMPLES_AXIS; x++)
    {
        density[(z * TERRAIN_SAMPLES_AXIS + y) * TERRAIN_SAMPLES_AXIS + x] =
            TVQuantizeSDF((f32)(y - 1) - 8.3f);
    }
    u32 regularOnly;
    Transvoxel_MeshChunk(density, 0, 0, 0, 0, 0, &mesh, &scratch);
    regularOnly = mesh.numIndices;
    Transvoxel_MeshChunk(density, 0, 0x33, 0, 0, 0, &mesh, &scratch); // transitions on +-x +-z
    u32 outOfRange = 0;
    for (u32 i = 0; i < mesh.numIndices; i++)
        if (mesh.indices[i] >= mesh.numVertices) outOfRange++;
    AX_LOG("terrain selftest transitions: plane indices %u -> %u with side transitions, badIndices=%u",
           regularOnly, mesh.numIndices, outOfRange);
    if (mesh.numIndices <= regularOnly || outOfRange)
        AX_ERROR("terrain selftest transition FAILED");

cleanup:
    SDL_free(edges);
    Transvoxel_MeshOutDestroy(&mesh);
    Transvoxel_ScratchDestroy(&scratch);
    SDL_free(density);
}
