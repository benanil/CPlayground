#include "../Common.hlsl"
#define GROUP_SIZE  8u
#define GROUP_COUNT 64u

// Supports up to a 13x13 source footprint.
// A 2x upsample with SAMPLE_SCALE=2 generally requires a 10x10 footprint.
#define LDS_DIM   13u
#define LDS_COUNT (LDS_DIM * LDS_DIM)

static const float SAMPLE_SCALE = 2.0f;

cbuffer BloomUpsampleParams : register(b0, space2)
{
    float2 lowTexelSize;
    uint2  outputSize;
};

Texture2D<float4> LowTexture  : register(t0, space0);
Texture2D<float4> HighTexture : register(t1, space0);

SamplerState LowSampler  : register(s0, space0);
SamplerState HighSampler : register(s1, space0);

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> OutputTexture : register(u0, space1);

groupshared f16_3 g_LowTile[LDS_COUNT];

f16_3 LoadLowLDS(int2 p, int2 tileOrigin)
{
    uint2 local = uint2(p - tileOrigin);
    return g_LowTile[local.y * LDS_DIM + local.x];
}

// Exact equivalent of the nine bilinear tent samples when sampleStep == 2.
// The samples no longer overlap, so the minimum exact footprint is 6x6 = 36 LDS reads.
f16_3 UpsampleTentDoubleLDS(float2 center, int2 tileOrigin)
{
    int2 base = int2(floor(center)) - 2;
    f16_2 f = f16_2(frac(center));

    f16_3 rows[6];
    [unroll]
	for (uint y = 0u; y < 6u; ++y)
	{
		int2 p = base + int2(0, y);
		rows[y] =
			LoadLowLDS(p + int2(0, 0), tileOrigin) * (f16(1.0f) - f.x) +
			LoadLowLDS(p + int2(1, 0), tileOrigin) * f.x +
			LoadLowLDS(p + int2(2, 0), tileOrigin) * (f16(2.0f) - f.x * f16(2.0f)) +
			LoadLowLDS(p + int2(3, 0), tileOrigin) * (f.x * f16(2.0f)) +
			LoadLowLDS(p + int2(4, 0), tileOrigin) * (f16(1.0f) - f.x) +
			LoadLowLDS(p + int2(5, 0), tileOrigin) * f.x;
	}
	
	f16 oneMinusY = f16(1.0f) - f.y;
	return (
		rows[0] * oneMinusY +
		rows[1] * f.y +
		rows[2] * (oneMinusY * f16(2.0f)) +
		rows[3] * (f.y * f16(2.0f)) +
		rows[4] * oneMinusY +
		rows[5] * f.y) * f16(1.0f / 16.0f);
}

// Equivalent to the nine bilinear tent samples when sampleStep == 1.
// Reduces 9 bilinear LDS samples (36 reads) to 16 LDS reads. for now unused
f16_3 UpsampleTentUnitLDS(float2 center, int2 tileOrigin)
{
    int2 base = int2(floor(center)) - 1;
    f16_2 f = f16_2(frac(center));
    f16_4 wx = f16_4(f16(1.0f) - f.x,
        			 f16(2.0f) - f.x,
        			 f16(1.0f) + f.x,
        			 f.x);

    f16_4 wy = f16_4(f16(1.0f) - f.y,
                     f16(2.0f) - f.y,
                     f16(1.0f) + f.y,
                     f.y);
    f16_3 rows[4];
    [unroll]
	for (uint y = 0u; y < 4u; ++y)
	{
		int2 p = base + int2(0, y);
		rows[y] = LoadLowLDS(p + int2(0, 0), tileOrigin) * wx.x +
				  LoadLowLDS(p + int2(1, 0), tileOrigin) * wx.y +
				  LoadLowLDS(p + int2(2, 0), tileOrigin) * wx.z +
				  LoadLowLDS(p + int2(3, 0), tileOrigin) * wx.w;
	}
	
	return (rows[0] * wy.x +
			rows[1] * wy.y +
			rows[2] * wy.z +
			rows[3] * wy.w) * f16(1.0f / 16.0f);
}

f16_3 UpsampleTentFallback(float2 uv)
{
    float2 step = lowTexelSize * SAMPLE_SCALE;
    f16_3 color = 0.0f;
    color += LowTexture.SampleLevel(LowSampler, uv + float2(-1,  1) * step, 0.0f).rgb;
    color += LowTexture.SampleLevel(LowSampler, uv + float2( 0,  1) * step, 0.0f).rgb * f16(2.0f);
    color += LowTexture.SampleLevel(LowSampler, uv + float2( 1,  1) * step, 0.0f).rgb;

    color += LowTexture.SampleLevel(LowSampler, uv + float2(-1,  0) * step, 0.0f).rgb * f16(2.0f);
    color += LowTexture.SampleLevel(LowSampler, uv,                         0.0f).rgb * f16(4.0f);
    color += LowTexture.SampleLevel(LowSampler, uv + float2( 1,  0) * step, 0.0f).rgb * f16(2.0f);

    color += LowTexture.SampleLevel(LowSampler, uv + float2(-1, -1) * step, 0.0f).rgb;
    color += LowTexture.SampleLevel(LowSampler, uv + float2( 0, -1) * step, 0.0f).rgb * f16(2.0f);
    color += LowTexture.SampleLevel(LowSampler, uv + float2( 1, -1) * step, 0.0f).rgb;
    return color * f16(1.0f / 16.0f);
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(
	uint3 tid       : SV_DispatchThreadID,
	uint3 groupID   : SV_GroupID,
	uint  groupIndex : SV_GroupIndex)
{
	uint lowWidth;
	uint lowHeight;
	uint mipCount;
	LowTexture.GetDimensions(0u, lowWidth, lowHeight, mipCount);

	uint2 lowSize = uint2(lowWidth, lowHeight);
	float2 lowSizeF = float2(lowSize);
	float2 invOutputSize = rcp(float2(outputSize));

	uint2 groupBase = groupID.xy * GROUP_SIZE;
	uint2 groupLast = min(
		groupBase + uint2(GROUP_SIZE - 1u, GROUP_SIZE - 1u),
		outputSize - 1u);

	float2 centerMin = (float2(groupBase) + 0.5f) * invOutputSize * lowSizeF - 0.5f;
	float2 centerMax = (float2(groupLast) + 0.5f) * invOutputSize * lowSizeF - 0.5f;

	// UpsampleTentDoubleLDS requires an exact two-texel step.
	float2 sampleStep = float2(SAMPLE_SCALE, SAMPLE_SCALE);
    float2 radius = abs(sampleStep);

    int2 tileOrigin = int2(floor(centerMin - radius));
    int2 tileLast   = int2(floor(centerMax + radius)) + 1;
    uint2 tileSize  = uint2(tileLast - tileOrigin + 1);

    bool useLDS = all(tileSize <= uint2(LDS_DIM, LDS_DIM));

    if (useLDS)
    {
        uint tileCount = tileSize.x * tileSize.y;

        for (uint i = groupIndex; i < tileCount; i += GROUP_COUNT)
        {
            uint2 local = uint2(i % tileSize.x, i / tileSize.x);
            int2 source = tileOrigin + int2(local);
            source = clamp(source, int2(0, 0), int2(lowSize) - 1);
            g_LowTile[local.y * LDS_DIM + local.x] = f16_4(LowTexture.Load(int3(source, 0)));
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (any(tid.xy >= outputSize))
        return;

    float2 uv = (float2(tid.xy) + 0.5f) * invOutputSize;
    float2 center = uv * lowSizeF - 0.5f;

    f16_3 low;
    if (useLDS) {
		// if (sampleScale == 2.0f)
		low = UpsampleTentDoubleLDS(center, tileOrigin);
        // else low = UpsampleTentUnitLDS(center, tileOrigin);
	}
    else {
        low = UpsampleTentFallback(uv);
    }

    f16_3 high = f16_3(HighTexture.SampleLevel(HighSampler, uv, 0.0f).rgb);
    OutputTexture[tid.xy] = float4(low + high, 1.0f);
}