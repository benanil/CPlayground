// procedural density field for the terrain: a 2D fbm + ridge heightfield carved by a 3D
// noise term so overhangs and caves exist (the transvoxel mesher is fully 3D). pure
// functions of world position and the active gen params, safe to call from worker
// threads: params only change while in flight jobs are already condemned (see
// Terrain_ApplyGenParams). noise functions below are ported from Dodisoft.NewTerrain.
#include "TerrainInternal.h"
#include "Include/Terrain.h"

#define TD_START_HEIGHT 0.38f
#define TD_WEIGHT       0.5f
#define TD_MULT         0.45f

// TERRAIN_BEDROCK_Y is deliberately NOT a multiple of the lod chunk sizes (16/32/64/128):
// if the floor landed on a chunk boundary the surface would fall on the seam with the
// all-solid chunk below it and mesh degenerately, leaving a hole at the bottom of a deep dig.
// extra mountain height toward the island center (island mode only). elevation above the
// base is scaled by up to (1 + this) at the center, tapering to 1 at the coast.
#define TD_CENTER_GAIN  0.75f

static TerrainGenParams td_Params = {
    .seed = 0,
    .seaLevel = 0.0f,
    .baseHeight = -8.0f,
    .hillAmplitude  = 0.3f,  .hillFrequency  = 0.5f,
    .ridgeAmplitude = 0.5f,  .ridgeFrequency = 0.45f,
    .carveAmplitude = 1.0f,  .carveFrequency = 0.045f,
    .island = false,
    .islandRadius = 250.0f, .islandFalloff = 110.0f,
    .fixedArea = false,
    .fixedWorldSize = TERRAIN_FIXED_WORLD_DEFAULT_SIZE,
};
// seed turns into a large noise domain offset, world coords stay near the origin
// so the s8 quantization and chunk keys are unaffected
static f32 td_SeedOffsetX, td_SeedOffsetZ;

static f32 TerrainNoisePerlin2(f32 x, f32 y)
{
    f32 fx = Fractf32(x);
    f32 fy = Fractf32(y);
    f32 px = Floorf32(x);
    f32 py = Floorf32(y);
    f32 v = px + py * 1000.0f;
    v128f r = VecAdd(VecSet1(v), VecSetR(0.0f, 1.0f, 1000.0f, 1001.0f));
    r = VecFract(VecMulf(VecSin(VecMulf(r, 0.001f)), 10000.0f));
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    f32 r0 = VecGetX(r); f32 r1 = VecGetY(r);
    f32 r2 = VecGetZ(r); f32 r3 = VecGetW(r);
    f32 a = r0 + (r1 - r0) * fx;
    f32 b = r2 + (r3 - r2) * fx;
    return 2.0f * (a + (b - a) * fy) - 1.0f;
}

static v128f TerrainNoisePerlin2V(v128f x, v128f y)
{
    v128f fx = VecFract(x);
    v128f fy = VecFract(y);
    v128f px = VecFloor(x);
    v128f py = VecFloor(y);
    v128f v  = VecFmadd(py, VecSet1(1000.0f), px);
    v128f r0 = VecFract(VecMulf(VecSin(VecMulf(VecAddf(v, 0.0f), 0.001f)), 10000.0f));
    v128f r1 = VecFract(VecMulf(VecSin(VecMulf(VecAddf(v, 1.0f), 0.001f)), 10000.0f));
    v128f r2 = VecFract(VecMulf(VecSin(VecMulf(VecAddf(v, 1000.0f), 0.001f)), 10000.0f));
    v128f r3 = VecFract(VecMulf(VecSin(VecMulf(VecAddf(v, 1001.0f), 0.001f)), 10000.0f));
    fx = VecMul(VecMul(fx, fx), VecSub(VecSet1(3.0f), VecMulf(fx, 2.0f)));
    fy = VecMul(VecMul(fy, fy), VecSub(VecSet1(3.0f), VecMulf(fy, 2.0f)));
    v128f a = VecFmadd(VecSub(r1, r0), fx, r0);
    v128f b = VecFmadd(VecSub(r3, r2), fx, r2);
    return VecSub(VecMulf(VecFmadd(VecSub(b, a), fy, a), 2.0f), VecOne());
}

static void TerrainNoiseHash2V(v128f x, v128f y, v128f* outX, v128f* outY)
{
    v128f p3x = VecFract(VecMulf(x, 0.1031f));
    v128f p3y = VecFract(VecMulf(y, 0.1031f));
    v128f p3z = VecFract(VecMulf(x, 0.0973f));
    v128f d = VecAdd(VecAdd(VecMul(p3x, VecAddf(p3y, 33.33f)),
                            VecMul(p3y, VecAddf(p3z, 33.33f))),
                            VecMul(p3z, VecAddf(p3x, 33.33f)));
    p3x = VecAdd(p3x, d); p3y = VecAdd(p3y, d); p3z = VecAdd(p3z, d);
    *outX = VecSub(VecMulf(VecFract(VecMul(VecAdd(p3x, p3y), p3z)), 2.0f), VecOne());
    *outY = VecSub(VecMulf(VecFract(VecMul(VecAdd(p3x, p3z), p3y)), 2.0f), VecOne());
}

static void TerrainNoiseHash2(f32 x, f32 y, f32* outX, f32* outY)
{
    v128f vX, vY;
    TerrainNoiseHash2V(VecSet1(x), VecSet1(y), &vX, &vY);
    *outX = VecGetX(vX);
    *outY = VecGetX(vY);
}

static f32 TerrainNoiseSimplex2(f32 x, f32 y)
{
    const f32 K1 = 0.366025404f;
    const f32 K2 = 0.211324865f;

    f32 iX = Floorf32(x + (x + y) * K1);
    f32 iY = Floorf32(y + (x + y) * K1);
    f32 aX = x - iX + (iX + iY) * K2;
    f32 aY = y - iY + (iX + iY) * K2;

    f32 m = aY <= aX ? 1.0f : 0.0f;
    f32 oX = m;
    f32 oY = 1.0f - m;

    f32 bX = aX - oX + K2;
    f32 bY = aY - oY + K2;
    f32 cX = aX - 1.0f + 2.0f * K2;
    f32 cY = aY - 1.0f + 2.0f * K2;

    f32 h0 = Maxf32(0.5f - (aX * aX + aY * aY), 0.0f);
    f32 h1 = Maxf32(0.5f - (bX * bX + bY * bY), 0.0f);
    f32 h2 = Maxf32(0.5f - (cX * cX + cY * cY), 0.0f);

    h0 = h0 * h0 * h0 * h0;
    h1 = h1 * h1 * h1 * h1;
    h2 = h2 * h2 * h2 * h2;

    v128f hashX, hashY;
    TerrainNoiseHash2V(
        VecSetR(iX,      iX + oX, iX + 1.0f, 0.0f),
        VecSetR(iY,      iY + oY, iY + 1.0f, 0.0f),
        &hashX,
        &hashY
    );
    v128f cornerX = VecSetR(aX, bX, cX, 0.0f);
    v128f cornerY = VecSetR(aY, bY, cY, 0.0f);
    v128f weight  = VecSetR(h0, h1, h2, 0.0f);
    v128f n = VecMul(weight, VecAdd(VecMul(cornerX, hashX), VecMul(cornerY, hashY)));
    return 70.0f * (VecGetX(n) + VecGetY(n) + VecGetZ(n));
}

static v128f TerrainNoiseSimplex2V(v128f x, v128f y)
{
    const f32 K1 = 0.366025404f;
    const f32 K2 = 0.211324865f;
    v128f xy = VecAdd(x, y);
    v128f iX = VecFloor(VecFmadd(xy, VecSet1(K1), x));
    v128f iY = VecFloor(VecFmadd(xy, VecSet1(K1), y));
    v128f iXYK2 = VecMulf(VecAdd(iX, iY), K2);
    v128f aX = VecAdd(VecSub(x, iX), iXYK2);
    v128f aY = VecAdd(VecSub(y, iY), iXYK2);
    v128f m  = VecBlend(VecZero(), VecOne(), VecCmpLe(aY, aX));
    v128f oX = m;
    v128f oY = VecSub(VecOne(), m);
    v128f bX = VecAddf(VecSub(aX, oX), K2), bY = VecAddf(VecSub(aY, oY), K2);
    v128f cX = VecAddf(aX, -1.0f + 2.0f * K2), cY = VecAddf(aY, -1.0f + 2.0f * K2);

    v128f h0 = VecMax(VecSub(VecSet1(0.5f), VecAdd(VecMul(aX, aX), VecMul(aY, aY))), VecZero());
    v128f h1 = VecMax(VecSub(VecSet1(0.5f), VecAdd(VecMul(bX, bX), VecMul(bY, bY))), VecZero());
    v128f h2 = VecMax(VecSub(VecSet1(0.5f), VecAdd(VecMul(cX, cX), VecMul(cY, cY))), VecZero());
    h0 = VecMul(VecMul(h0, h0), VecMul(h0, h0));
    h1 = VecMul(VecMul(h1, h1), VecMul(h1, h1));
    h2 = VecMul(VecMul(h2, h2), VecMul(h2, h2));

    v128f hx, hy;
    TerrainNoiseHash2V(iX, iY, &hx, &hy);
    v128f n0 = VecMul(h0, VecAdd(VecMul(aX, hx), VecMul(aY, hy)));
    TerrainNoiseHash2V(VecAdd(iX, oX), VecAdd(iY, oY), &hx, &hy);
    v128f n1 = VecMul(h1, VecAdd(VecMul(bX, hx), VecMul(bY, hy)));
    TerrainNoiseHash2V(VecAddf(iX, 1.0f), VecAddf(iY, 1.0f), &hx, &hy);
    v128f n2 = VecMul(h2, VecAdd(VecMul(cX, hx), VecMul(cY, hy)));
    return VecMulf(VecAdd(VecAdd(n0, n1), n2), 70.0f);
}

static void TerrainNoiseMatMul(f32* x, f32* y)
{
    f32 ox = *x, oy = *y;
    *x = 1.6f * ox + 1.2f * oy;
    *y = -1.2f * ox + 1.6f * oy;
}

static f32 TerrainNoiseSimplexRidgedSimple(f32 x, f32 y)
{
    f32 f = 0.5000f * Absf32(TerrainNoiseSimplex2(x, y));
    TerrainNoiseMatMul(&x, &y);
    f += 0.2500f * Absf32(TerrainNoiseSimplex2(x, y));
    return Clampf32(0.68f - f, 0.05f, 1.0f);
}

static v128f TerrainNoiseSimplexRidgedSimpleV(v128f x, v128f y)
{
    v128f f = VecMulf(VecFabs(TerrainNoiseSimplex2V(x, y)), 0.5000f);
    v128f ox = x, oy = y;
    x = VecAdd(VecMulf(ox, 1.6f), VecMulf(oy, 1.2f));
    y = VecAdd(VecMulf(ox, -1.2f), VecMulf(oy, 1.6f));
    f = VecFmadd(VecFabs(TerrainNoiseSimplex2V(x, y)), VecSet1(0.2500f), f);
    return VecClamp(VecSub(VecSet1(0.68f), f), VecSet1(0.05f), VecOne());
}

static f32 TerrainNoisePerlinTerrain(f32 x, f32 y, int freq, f32 h, f32 w, f32 m)
{
    for (int i = 0; i < freq; i++)
    {
        h += w * TerrainNoisePerlin2(x * m, y * m);
        w *= 0.5f;
        m *= 2.0f;
    }
    return Clampf32(h * 0.68f + 0.1f, 0.04f, 1.0f);
}

static v128f TerrainNoisePerlinTerrainV(v128f x, v128f y, int freq, f32 h, f32 w, f32 m)
{
    v128f hv = VecSet1(h);
    for (int i = 0; i < freq; i++)
    {
        hv = VecFmadd(TerrainNoisePerlin2V(VecMulf(x, m), VecMulf(y, m)), VecSet1(w), hv);
        w *= 0.5f;
        m *= 2.0f;
    }
    return VecClamp(VecAddf(VecMulf(hv, 0.68f), 0.1f), VecSet1(0.04f), VecOne());
}

static f32 TerrainNoiseDensity01(f32 x, f32 z)
{
    f32 noiseScale = Clampf32(td_Params.hillFrequency, 0.05f, 4.0f);
    f32 lowlandScale = Clampf32(td_Params.hillAmplitude, 0.25f, 8.0f);
    f32 mountainScale = Clampf32(td_Params.ridgeFrequency, 0.05f, 2.0f);
    f32 cx = (x + td_SeedOffsetX) / TERRAIN_CHUNK_CELLS * noiseScale;
    f32 cz = (z + td_SeedOffsetZ) / TERRAIN_CHUNK_CELLS * noiseScale;
    f32 mountain = TerrainNoiseSimplexRidgedSimple(cx * mountainScale, cz * mountainScale) *
                   TerrainNoisePerlinTerrain(cx * lowlandScale * 4.0f, cz * lowlandScale * 4.0f, 2, TD_START_HEIGHT, TD_WEIGHT, TD_MULT);
    f32 lowland = TerrainNoisePerlinTerrain(cx * lowlandScale, cz * lowlandScale, 4, TD_START_HEIGHT, TD_WEIGHT, TD_MULT);
    return lowland + (mountain - lowland) * 0.64f;
}

static v128f TerrainNoiseDensity01V(v128f x, v128f z)
{
    f32 noiseScale    = Clampf32(td_Params.hillFrequency , 0.05f, 4.0f);
    f32 lowlandScale  = Clampf32(td_Params.hillAmplitude , 0.25f, 8.0f);
    f32 mountainScale = Clampf32(td_Params.ridgeFrequency, 0.05f, 2.0f);
    v128f cx = VecMulf(VecAddf(x, td_SeedOffsetX), noiseScale / (f32)TERRAIN_CHUNK_CELLS);
    v128f cz = VecMulf(VecAddf(z, td_SeedOffsetZ), noiseScale / (f32)TERRAIN_CHUNK_CELLS);
    v128f mountain = VecMul(TerrainNoiseSimplexRidgedSimpleV(VecMulf(cx, mountainScale), VecMulf(cz, mountainScale)),
                            TerrainNoisePerlinTerrainV(VecMulf(cx, lowlandScale * 4.0f), VecMulf(cz, lowlandScale * 4.0f), 2, TD_START_HEIGHT, TD_WEIGHT, TD_MULT));
    v128f lowland = TerrainNoisePerlinTerrainV(VecMulf(cx, lowlandScale), VecMulf(cz, lowlandScale), 4, TD_START_HEIGHT, TD_WEIGHT, TD_MULT);
    return VecFmadd(VecSub(mountain, lowland), VecSet1(0.64f), lowland);
}

static f32 TerrainDensity_IslandFade(f32 x, f32 z)
{
    if (!td_Params.island) return 0.0f;
    f32 dist = Sqrtf(x * x + z * z);
    f32 t = Clampf32((dist - td_Params.islandRadius) / Maxf32(td_Params.islandFalloff, 1.0f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static v128f TerrainDensity_IslandFadeV(v128f x, v128f z)
{
    if (!td_Params.island) return VecZero();
    v128f dist = VecSqrt(VecAdd(VecMul(x, x), VecMul(z, z)));
    v128f t = VecClamp(VecDivf(VecSubf(dist, td_Params.islandRadius), Maxf32(td_Params.islandFalloff, 1.0f)), VecZero(), VecOne());
    return VecMul(VecMul(t, t), VecSub(VecSet1(3.0f), VecMulf(t, 2.0f)));
}

TerrainGenParams Terrain_DefaultGenParams(void)
{
    TerrainGenParams defaults = {
        .seed           = 0,
        .seaLevel       = 0.0f,
        .baseHeight     = -8.0f,
        .hillAmplitude  = 1.0f,  .hillFrequency = 1.0f,
        .ridgeAmplitude = 1.0f,  .ridgeFrequency = 0.45f,
        .carveAmplitude = 1.0f,  .carveFrequency = 0.045f,
        .island = false,
        .islandRadius = 250.0f, .islandFalloff = 110.0f,
        .fixedArea = false,
        .fixedWorldSize = TERRAIN_FIXED_WORLD_DEFAULT_SIZE,
    };
    return defaults;
}

void TerrainDensity_SetParams(const TerrainGenParams* params)
{
    td_Params = *params;
    u32 h = params->seed * 0x9E3779B9u;
    td_SeedOffsetX = (f32)(h & 0xFFFFu) * 0.731f;
    td_SeedOffsetZ = (f32)((h >> 16) & 0xFFFFu) * 0.617f;
}

const TerrainGenParams* TerrainDensity_GetParams(void)
{
    return &td_Params;
}

f32 TerrainDensity_IslandMask(f32 x, f32 z)
{
    return TerrainDensity_IslandFade(x, z);
}

f32 TerrainDensity_Height(f32 x, f32 z)
{
    f32 density = TerrainNoiseDensity01(x, z);
    f32 heightScale = Clampf32(td_Params.ridgeAmplitude, 0.25f, 2.0f);
    f32 elev = ((density * 255.0f - 7.0f) * 0.3333333f + 9.666667f) * heightScale;

    if (td_Params.island)
    {
        f32 t = TerrainDensity_IslandFade(x, z);
        // taller mountains toward the center (fade 0), tapering to normal at the coast
        elev *= 1.0f + (1.0f - t) * TD_CENTER_GAIN;
        f32 height = td_Params.baseHeight + elev;
        // far from the island keep streaming, but generate a flat sea-level plane
        // instead of sinking the whole field below the water line
        return height * (1.0f - t) + td_Params.seaLevel * t;
    }
    return td_Params.baseHeight + elev;
}

static v128f TerrainDensity_HeightV(v128f x, v128f z)
{
    v128f density = TerrainNoiseDensity01V(x, z);
    f32 heightScale = Clampf32(td_Params.ridgeAmplitude, 0.25f, 2.0f);
    v128f elev = VecMulf(VecAddf(VecMulf(VecSubf(VecMulf(density, 255.0f), 7.0f), 0.3333333f), 9.666667f), heightScale);

    if (td_Params.island)
    {
        v128f t = TerrainDensity_IslandFadeV(x, z);
        elev = VecMul(elev, VecAddf(VecMulf(VecSub(VecOne(), t), TD_CENTER_GAIN), 1.0f));
        v128f height = VecAddf(elev, td_Params.baseHeight);
        return VecFmadd(VecSub(VecSet1(td_Params.seaLevel), height), t, height);
    }
    return VecAddf(elev, td_Params.baseHeight);
}

static f32 TerrainDensity_Carve(f32 x, f32 y, f32 z)
{
    f32 freq = td_Params.carveFrequency;
    return td_Params.carveAmplitude *
           TerrainNoiseSimplex2((x + td_SeedOffsetX) * freq + y * freq * 0.37f,
                                (z + td_SeedOffsetZ) * freq - y * freq * 0.19f);
}

static v128f TerrainDensity_CarveV(v128f x, v128f y, v128f z)
{
    f32 freq = td_Params.carveFrequency;
    v128f nx = VecAdd(VecMulf(VecAddf(x, td_SeedOffsetX), freq), VecMulf(y, freq * 0.37f));
    v128f nz = VecSub(VecMulf(VecAddf(z, td_SeedOffsetZ), freq), VecMulf(y, freq * 0.19f));
    return VecMulf(TerrainNoiseSimplex2V(nx, nz), td_Params.carveAmplitude);
}

// raw analytic field: terrain heightfield + 3D carve, no bedrock floor, no sculpt. the
// floor is a min applied by the callers as their final step so sculpt can never defeat it.
static f32 TerrainDensity_Raw(f32 x, f32 y, f32 z)
{
    f32 islandFade = TerrainDensity_IslandFade(x, z);
    f32 carve = TerrainDensity_Carve(x, y, z) * (1.0f - islandFade);
    return (y - TerrainDensity_Height(x, z)) + carve;
}

f32 TerrainDensity_SDF(f32 x, f32 y, f32 z)
{
    // solid bedrock floor: min with (y - surface) keeps integer samples at/below
    // TERRAIN_BEDROCK_Y negative (solid), so the world has no bottom to fall through
    return Minf32(TerrainDensity_Raw(x, y, z), y - TERRAIN_BEDROCK_SURFACE_Y);
}

f32 TerrainDensity_At(f32 x, f32 y, f32 z)
{
    f32 density = TerrainDensity_Raw(x, y, z);
    // sculpt edits raise/lower the field so grass follows painted/sculpted terrain, added
    // BEFORE the floor min so a dig can never punch through the bedrock (floor always wins).
    // skip the map probe when nothing is edited.
    if (TerrainEdit_NumChunks() != 0u)
        density += TerrainEdit_DeltaAt((s32)Floorf32(x + 0.5f), (s32)Floorf32(y + 0.5f), (s32)Floorf32(z + 0.5f));
    return Minf32(density, y - TERRAIN_BEDROCK_SURFACE_Y);
}

f32 TerrainDensity_SurfaceY(f32 x, f32 z, f32 startY, float3* outNormal)
{
    // the sdf is ~signed distance in meters and d(sdf)/dy is ~1, so y -= sdf is a Newton
    // step that converges in a couple of iterations from the analytic height seed
    f32 y = startY;
    for (s32 i = 0; i < 6; i++)
        y -= TerrainDensity_At(x, y, z);

    if (outNormal)
    {
        const f32 e = 0.5f;
        float3 grad = {
            TerrainDensity_At(x + e, y, z) - TerrainDensity_At(x - e, y, z),
            TerrainDensity_At(x, y + e, z) - TerrainDensity_At(x, y - e, z),
            TerrainDensity_At(x, y, z + e) - TerrainDensity_At(x, y, z - e)
        };
        // the sdf increases toward air, so its gradient is the outward (up-facing) normal
        *outNormal = F3NormSafe(grad);
    }
    return y;
}

void TerrainDensity_GetYRange(f32* outMin, f32* outMax)
{
    // Ported algorithm maps density 0.04..1.0 through the original y formula.
    f32 heightScale = Clampf32(td_Params.ridgeAmplitude, 0.25f, 2.0f);
    f32 minElev = ((0.04f * 255.0f - 7.0f) * 0.3333333f + 9.666667f) * heightScale;
    f32 maxElev = ((1.00f * 255.0f - 7.0f) * 0.3333333f + 9.666667f) * heightScale;
    if (td_Params.island) maxElev *= 1.0f + TD_CENTER_GAIN; // center peaks are the tallest
    f32 lo = td_Params.baseHeight + minElev - td_Params.carveAmplitude - 2.0f;
    f32 hi = td_Params.baseHeight + maxElev + td_Params.carveAmplitude + 2.0f;
    // stream a chunk past the bedrock floor so the solid floor itself always meshes
    lo = Minf32(lo, TERRAIN_BEDROCK_Y - (f32)TERRAIN_CHUNK_CELLS);
    if (td_Params.island)
    {
        lo = Minf32(lo, td_Params.seaLevel - td_Params.carveAmplitude - 2.0f);
        hi = Maxf32(hi, td_Params.seaLevel + 2.0f);
    }
    *outMin = lo;
    *outMax = hi;
}

// quantize a signed distance in meters to s8. the scale is world fixed so a coarse
// sample bit-matches the fine sample at the same position (see TERRAIN_SDF_CLAMP)
static s8 TerrainDensity_Quantize(f32 sdf)
{
    f32 q = sdf * (127.0f / TERRAIN_SDF_CLAMP);
    if (q >  127.0f) q =  127.0f;
    if (q < -127.0f) q = -127.0f;
    s32 v = (s32)(q >= 0.0f ? q + 0.5f : q - 0.5f);
    if (v == 0) v = q < 0.0f ? -1 : 1;
    return (s8)v;
}

void TerrainDensity_SampleChunk(s32 cx, s32 cy, s32 cz, u32 lod, s8* out)
{
    const f32 voxel = TERRAIN_VOXEL_SIZE * (f32)(1 << lod);
    const f32 ox = (f32)cx * (TERRAIN_CHUNK_CELLS * voxel);
    const f32 oy = (f32)cy * (TERRAIN_CHUNK_CELLS * voxel);
    const f32 oz = (f32)cz * (TERRAIN_CHUNK_CELLS * voxel);
    const f32 carveSkip = TERRAIN_SDF_CLAMP + td_Params.carveAmplitude;

    // the heightfield part only depends on the column, evaluate it once per (x,z)
    f32 heights[TERRAIN_SAMPLES_AXIS * TERRAIN_SAMPLES_AXIS];
    for (s32 z = 0; z < TERRAIN_SAMPLES_AXIS; z++)
    for (s32 x = 0; x < TERRAIN_SAMPLES_AXIS; x += 4)
    {
        v128f wx = VecAdd(VecSet1(ox + (f32)(x - 1) * voxel), VecMulf(VecSetR(0.0f, 1.0f, 2.0f, 3.0f), voxel));
        v128f wz = VecSet1(oz + (f32)(z - 1) * voxel);
        f32 h[4];
        VecStore(h, TerrainDensity_HeightV(wx, wz));
        u32 n = Minu32(4u, (u32)(TERRAIN_SAMPLES_AXIS - x));
        for (u32 i = 0; i < n; i++) heights[z * TERRAIN_SAMPLES_AXIS + x + i] = h[i];
    }

    s8* dst = out;
    for (s32 z = 0; z < TERRAIN_SAMPLES_AXIS; z++)
    for (s32 y = 0; y < TERRAIN_SAMPLES_AXIS; y++)
    for (s32 x = 0; x < TERRAIN_SAMPLES_AXIS; x += 4)
    {
        v128f wx = VecAdd(VecSet1(ox + (f32)(x - 1) * voxel), VecMulf(VecSetR(0.0f, 1.0f, 2.0f, 3.0f), voxel));
        v128f wy = VecSet1(oy + (f32)(y - 1) * voxel);
        v128f wz = VecSet1(oz + (f32)(z - 1) * voxel);
        f32 h[4] = {0};
        u32 n = Minu32(4u, (u32)(TERRAIN_SAMPLES_AXIS - x));
        for (u32 i = 0; i < n; i++) h[i] = heights[z * TERRAIN_SAMPLES_AXIS + x + i];
        v128f sdf = VecSub(wy, VecLoad(h));
        // the carve term only matters near the surface, skip the 3D noise when the
        // column distance alone saturates the s8 range
        v128f carveMask = VecAnd(VecCmpGt(sdf, VecSet1(-carveSkip)), VecCmpLt(sdf, VecSet1(carveSkip)));
        if (VecMovemask(carveMask))
        {
            v128f carve = VecMul(TerrainDensity_CarveV(wx, wy, wz), VecSub(VecOne(), TerrainDensity_IslandFadeV(wx, wz)));
            sdf = VecAdd(sdf, VecSelect(VecZero(), carve, carveMask));
        }
        f32 s[4];
        VecStore(s, sdf);
        for (u32 i = 0; i < n; i++) *dst++ = TerrainDensity_Quantize(s[i]);
    }

    // sparse sculpt edits overlay on top, world fixed like the base field so every
    // lod sees identical values at shared sample positions
    TerrainEdit_OverlayChunk(cx, cy, cz, lod, out);

    // bedrock floor, applied last so sculpt can never dig through it. each y-plane clamps
    // its samples toward solid by the quantized (worldY - floor): above the floor that
    // threshold saturates to +127 and the min is a no-op, below it drives samples solid.
    for (s32 y = 0; y < TERRAIN_SAMPLES_AXIS; y++)
    {
        s32 floorQ = TerrainDensity_Quantize(oy + (f32)(y - 1) * voxel - TERRAIN_BEDROCK_SURFACE_Y);
        for (s32 z = 0; z < TERRAIN_SAMPLES_AXIS; z++)
        for (s32 x = 0; x < TERRAIN_SAMPLES_AXIS; x++)
        {
            s8* p = &out[(z * TERRAIN_SAMPLES_AXIS + y) * TERRAIN_SAMPLES_AXIS + x];
            *p = (s8)Mins32(*p, floorQ);
        }
    }
}
