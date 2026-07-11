// transvoxel-unity port terrain surface: heap-resident colored triangle soup, every
// chunk lives in one shared vertex buffer and draws in a single indirect multidraw.
// lighting is baked into the vertex color at build time; the fragment stage only adds
// the editor brush highlight so the cursor follows the mouse without any remesh.
#include "Bitpack.hlsl"

struct VSInput
{
    float4 pos : POSITION0;
    uint color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    f16_3_io color  : COLOR;
};

cbuffer vs_params : register(b0, space1)
{
    float4x4 uViewProj;
};

cbuffer ps_params : register(b0, space3)
{
    float4 uBrushPosRadius; // xyz world position, w radius (<= 0 when inactive)
};

VSOutput vert(VSInput i)
{
    VSOutput o;
    o.position = mul(uViewProj, float4(i.pos.xyz, 1.0));
    o.worldPos = i.pos.xyz;
    o.color = UnpackColor3Uint(i.color);
    return o;
}

float4 frag(VSOutput i) : SV_Target0
{
    float3 color = float3(f16_3(i.color));
    if (uBrushPosRadius.w > 0.0)
    {
        float brushDist = distance(i.worldPos, uBrushPosRadius.xyz);
        float glow = 1.0 - smoothstep(uBrushPosRadius.w * 0.7, uBrushPosRadius.w, brushDist);
        color = lerp(color, float3(1.0, 0.85, 0.45), glow * 0.55);
    }
    return float4(color, 1.0);
}
