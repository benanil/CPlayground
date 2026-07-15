// AMD FidelityFX Single Pass Downsampler (SPD) integration for the Hi-Z depth pyramid.
// Replaces the per-mip dispatch loop in HiZDownscaleCompute with one dispatch that reduces
// every remaining mip via wave-intrinsic quad reduction + groupshared LDS bridging between
// waves. Mip 0 of tex_hiz (the raw depth copy, see HiZBuildCompute.hlsl) is left untouched and
// is this shader's "source image" — SPD's internal mip 0 output lands in tex_hiz's real mip 1.
cbuffer HiZSPDParams : register(b0, space2)
{
    uint2  sourceSize; // tex_hiz mip 0 dimensions, for clamped source loads
    uint2  mip1Size;   // tex_hiz mip 1 dimensions (SPD's internal mip 0 output size)
    uint   mips;       // reduction levels to produce (<=8, capped by SDL_GPU's per-stage RW storage texture limit)
    uint   numWorkGroups;
    uint   padding0;
    uint   padding1;
};

Texture2D<float> SourceHiZ : register(t0, space0);

[[vk::image_format("r32f")]] RWTexture2D<float> Mip1 : register(u0, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip2 : register(u1, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip3 : register(u2, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip4 : register(u3, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip5 : register(u4, space1);
// Mip6 is the cross-workgroup sync point: SPD's last surviving workgroup re-reads every texel
// written here by all other workgroups, so it must be globally coherent.
[[vk::image_format("r32f")]] globallycoherent RWTexture2D<float> Mip6 : register(u5, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip7 : register(u6, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> Mip8 : register(u7, space1);

globallycoherent RWStructuredBuffer<uint> SpdCounterBuffer : register(u8, space1);

bool SpdMipInBounds(int2 pix, uint mip)
{
    uint2 sz = max(mip1Size >> mip, uint2(1u, 1u));
    return (uint(pix.x) < sz.x) && (uint(pix.y) < sz.y);
}

#define A_GPU
#define A_HLSL

#include "../Vendor/FFX_SPD/ffx_a.h"

groupshared AU1 spdCounter;
groupshared AF1 spdIntermediate[16][16];

// Raw texel fetch, not a bilinear sample: min-reduction over 4 depths is not the same as their
// weighted average, so we can't use SPD's linear-sampler shortcut like the bloom port does.
// Depth is single-channel; SPD's plumbing is float4-shaped throughout, so it rides in .x only.
AF4 SpdLoadSourceImage(ASU2 p, AU1 slice) {
    uint2 clamped = min(uint2(p), sourceSize - 1u);
    float d = SourceHiZ.Load(int3(clamped, 0));
    return AF4(d, 0.0f, 0.0f, 0.0f);
}

AF4 SpdLoad(ASU2 p, AU1 slice) {
    return AF4(Mip6[p], 0.0f, 0.0f, 0.0f);
}

void SpdStore(ASU2 pix, AF4 value, AU1 mip, AU1 slice)
{
    if (!SpdMipInBounds(pix, mip)) return;
    switch (mip)
    {
        case 0: Mip1[pix] = value.x; break;
        case 1: Mip2[pix] = value.x; break;
        case 2: Mip3[pix] = value.x; break;
        case 3: Mip4[pix] = value.x; break;
        case 4: Mip5[pix] = value.x; break;
        case 5: Mip6[pix] = value.x; break;
        case 6: Mip7[pix] = value.x; break;
        case 7: Mip8[pix] = value.x; break;
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

AF4 SpdLoadIntermediate(AU1 x, AU1 y) {
    return AF4(spdIntermediate[x][y], 0.0f, 0.0f, 0.0f);
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value) {
    spdIntermediate[x][y] = value.x;
}

// Reversed-Z: the farthest occluder has the smallest depth value, so a conservative per-tile
// depth (the one guaranteed not to occlude anything it shouldn't) is the minimum, not the max.
AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3) {
    return min(min(v0, v1), min(v2, v3));
}

#include "../Vendor/FFX_SPD/ffx_spd.h"

[numthreads(256, 1, 1)]
void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    SpdDownsample(
        AU2(WorkGroupId.xy),
        AU1(LocalThreadIndex),
        AU1(mips),
        AU1(numWorkGroups),
        AU1(0));
}
