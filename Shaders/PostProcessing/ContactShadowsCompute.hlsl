// Half-res sun contact shadows using Bend Studio's screen-space shadow projection.
// Bend operates in output pixel space; this wrapper maps those half-res pixels to
// the full-res reversed-Z depth prepass before including the shared implementation.

#define WAVE_SIZE 64
#define SAMPLE_COUNT 48
#define HARD_SHADOW_SAMPLES 4
#define FADE_OUT_SAMPLES 8

cbuffer ContactShadowParams : register(b0, space2)
{
    float4 lightCoordinate;
    int2 waveOffset;
    uint2 fullSize;
    uint2 shadowSize;
    float surfaceThickness;
    float bilinearThreshold;
    float shadowContrast;
    float intensity;
    uint enabled;
    uint padding;
};

Texture2D<float> DepthTexture : register(t0, space0);
SamplerState DepthSampler : register(s0, space0);
[[vk::image_format("r8")]] RWTexture2D<float> OutputShadow : register(u0, space1);

float ContactShadowSampleDepth(float2 shadowPixel)
{
    int2 p = int2(floor(shadowPixel));
    // if (p.x < 0 || p.y < 0 || p.x >= int(shadowSize.x) || p.y >= int(shadowSize.y))
    //     return 0.0f;
    uint2 fullP = clamp(uint2(p) * 2u + 1u, uint2(0u, 0u), fullSize - 1u);
    return DepthTexture.Load(int3(fullP, 0));
}

void ContactShadowWrite(int2 pixel, float value)
{
    // if (pixel.x < 0 || pixel.y < 0 || pixel.x >= int(shadowSize.x) || pixel.y >= int(shadowSize.y))
    //     return;
    OutputShadow[pixel] = max(lerp(1.0f, value, intensity), 0.3f);
}

#define BEND_SSS_SAMPLE_DEPTH(inParameters, pixelXY) ContactShadowSampleDepth(pixelXY)
#define BEND_SSS_WRITE_OUTPUT(inParameters, pixelXY, value) ContactShadowWrite(pixelXY, value)
#include "bend_sss_gpu.h"

[numthreads(64, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    if (enabled == 0u)
    {
        uint2 clearP = uint2(groupID.x * WAVE_SIZE + groupThreadID.x, groupID.y);
        if (clearP.x < shadowSize.x && clearP.y < shadowSize.y)
            OutputShadow[clearP] = 1.0f;
        return;
    }

    DispatchParameters params;
    params.SurfaceThickness = surfaceThickness;
    params.BilinearThreshold = bilinearThreshold;
    params.ShadowContrast = shadowContrast;
    params.IgnoreEdgePixels = false;
    params.UsePrecisionOffset = false;
    params.BilinearSamplingOffsetMode = false;
    params.DebugOutputEdgeMask = false;
    params.DebugOutputThreadIndex = false;
    params.DebugOutputWaveIndex = false;
    params.DepthBounds = float2(0.00001f, 1.0f);
    params.UseEarlyOut = false;
    params.LightCoordinate = lightCoordinate;
    params.WaveOffset = waveOffset;
    params.FarDepthValue = 0.0f;
    params.NearDepthValue = 1.0f;
    params.InvDepthTextureSize = 1.0f / float2(max(shadowSize, 1u));
    params.DepthTexture = DepthTexture;
    params.OutputTexture = OutputShadow;
    params.PointBorderSampler = DepthSampler;

    WriteScreenSpaceShadow(params, int3(groupID), groupThreadID);
}
