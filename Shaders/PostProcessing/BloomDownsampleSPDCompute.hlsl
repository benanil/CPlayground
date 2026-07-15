// AMD FidelityFX Single Pass Downsampler (SPD) integration for the bloom downsample chain.
// Replaces the per-mip dispatch loop in BloomPrefilterDownsampleCompute with one dispatch that
// builds every mip via wave-intrinsic quad reduction + groupshared LDS bridging between waves.
// Threshold/knee/clamp are folded into SpdLoadSourceImageH since SPD only ever reads the raw
// source once per output texel (no per-mip re-threshold like the old chain). Packed (half
// precision) path: halves LDS/register traffic for the reduction, UAVs stay rgba16f either way.
#define BLOOM_EPSILON AH1(0.0005)

cbuffer SpdBloomParams : register(b0, space2)
{
    float2 invInputSize;   // 1 / full-res source dimensions
    uint2  mip0Size;       // bloom_width, bloom_height (mip 0 dimensions)
    uint   mips;           // total mip levels to produce (<=8, capped by SDL_GPU's per-stage RW storage texture limit)
    uint   numWorkGroups;  // dispatchX * dispatchY (single slice, so no z factor)
    float  threshold;
    float  knee;
    float  clampValue;
    float  padding0;
};

Texture2D<float4> SourceTexture : register(t0, space0);
SamplerState SourceSampler : register(s0, space0);

[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip0  : register(u0,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip1  : register(u1,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip2  : register(u2,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip3  : register(u3,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip4  : register(u4,  space1);
// Mip5 is the cross-workgroup sync point: SPD's last surviving workgroup re-reads every texel
// written here by all other workgroups, so it must be globally coherent.
[[vk::image_format("rgba16f")]] globallycoherent RWTexture2D<float4> Mip5  : register(u5,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip6  : register(u6,  space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> Mip7  : register(u7,  space1);

globallycoherent RWStructuredBuffer<uint> SpdCounterBuffer : register(u8, space1);

bool SpdMipInBounds(int2 pix, uint mip) {
    uint2 sz = max(mip0Size >> mip, uint2(1u, 1u));
    return (uint(pix.x) < sz.x) && (uint(pix.y) < sz.y);
}

#define A_GPU
#define A_HLSL
// Must be defined before ffx_a.h is included: that's where AH1/AH2/AH4 (min16float* aliases)
// get typedef'd, gated on A_HALF already being set.
#define A_HALF
#define SPD_LINEAR_SAMPLER
// We only ever call the packed (H-suffixed) entry point below; this makes ffx_spd.h stub out
// the non-packed AF4 hook functions instead of requiring real ones from us.
#define SPD_PACKED_ONLY

#include "../Vendor/FFX_SPD/ffx_a.h"

AH3 SafeHDR(AH3 color) {
    return min(max(color, 0.0f), clampValue);
}

AH3 QuadraticThreshold(AH3 color)
{
    AH1 brightness = max(max(color.r, color.g), color.b);
    AH1 soft = brightness - threshold + knee;
    soft = clamp(soft, AH1(0.0f), knee * AH1(2.0f));
    soft = soft * soft / max(knee * AH1(4.0f), BLOOM_EPSILON);
    AH1 contribution = max(soft, brightness - threshold) / max(brightness, BLOOM_EPSILON);
    return color * saturate(contribution);
}

groupshared AU1 spdCounter;
groupshared AH4 spdIntermediate[16][16];

// Source is sampled once, exactly between the 2x2 texel quad each SPD thread reduces, so the
// hardware bilinear filter does the first box-average for free (AMD's recommended fast path).
// UV math stays full float — only the resulting color gets narrowed to half.
AH4 SpdLoadSourceImageH(ASU2 p, AU1 slice)
{
    AF2 uv = (AF2(p) + AF2(1.0f, 1.0f)) * invInputSize;
    AH3 color = SafeHDR(SourceTexture.SampleLevel(SourceSampler, uv, 0.0f).rgb);
    color = QuadraticThreshold(color);
    return AH4(color, 1.0f);
}

AH4 SpdLoadH(ASU2 p, AU1 slice) {
    return AH4(Mip5[p]);
}

void SpdStoreH(ASU2 pix, AH4 value, AU1 mip, AU1 slice)
{
    if (!SpdMipInBounds(pix, mip)) return;
    switch (mip)
    {
        case 0:  Mip0[pix]  = value; break;
        case 1:  Mip1[pix]  = value; break;
        case 2:  Mip2[pix]  = value; break;
        case 3:  Mip3[pix]  = value; break;
        case 4:  Mip4[pix]  = value; break;
        case 5:  Mip5[pix]  = value; break;
        case 6:  Mip6[pix]  = value; break;
        case 7:  Mip7[pix]  = value; break;
    }
}

void SpdIncreaseAtomicCounter(AU1 slice) {
    InterlockedAdd(SpdCounterBuffer[0], 1, spdCounter);
}

AU1 SpdGetAtomicCounter() {
    return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice) {
    SpdCounterBuffer[0] = 0;
}

AH4 SpdLoadIntermediateH(AU1 x, AU1 y) {
	return spdIntermediate[x][y];
}

void SpdStoreIntermediateH(AU1 x, AU1 y, AH4 value) {
	spdIntermediate[x][y] = value;
}

AH4 SpdReduce4H(AH4 v0, AH4 v1, AH4 v2, AH4 v3) {
    return (v0 + v1 + v2 + v3) * AH1(0.25);
}

#include "../Vendor/FFX_SPD/ffx_spd.h"

[numthreads(256, 1, 1)]
void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    SpdDownsampleH(
        AU2(WorkGroupId.xy),
        AU1(LocalThreadIndex),
        AU1(mips),
        AU1(numWorkGroups),
        AU1(0));
}
