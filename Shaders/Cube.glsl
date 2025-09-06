@ctype mat4 Matrix4

@vs vs
layout(binding = 0) uniform vs_params {
    highp mat4 mvp;
    highp mat4 uModel;
    highp mat4 uLightMatrix;
    highp mat4 uViewProj;
};

layout(location = 0) in highp   vec3  aPos;
layout(location = 1) in lowp    vec3  aNormal;
layout(location = 2) in lowp    vec4  aTangent;
layout(location = 3) in mediump vec2  aTexCoords;
layout(location = 4) in lowp    uvec4 aJoints; // lowp int ranges between 0-255 
layout(location = 5) in lowp    vec4  aWeights;

@image_sample_type uAnimTex unfilterable_float
layout(binding = 1) uniform texture2D uAnimTex;

@sampler_type smp nonfiltering
layout(binding = 1) uniform sampler smp;

// out highp   vec4 vLightSpaceFrag;
out mediump vec2 vTexCoords;
out lowp    mat3 vTBN;

// https://www.shadertoy.com/view/3s33zj
highp mat3 adjoint(in highp mat4 m)
{
    return mat3(cross(m[1].xyz, m[2].xyz),
                cross(m[2].xyz, m[0].xyz),
                cross(m[0].xyz, m[1].xyz));
}

void main() {
    highp mat4 model = uModel;

    // vBoneIdx = -1;
    // if (uHasAnimation > 0) 
    // {
    //     mediump mat4 animMat = mat4(0.0);
    //     animMat[3].w = 1.0; // last row is [0.0, 0.0, 0.0, 1.0]
    // 
    //     for (int i = 0; i < 4; i++)
    //     {
    //         int matIdx = int(aJoints[i]) * 3; // 3 because our matrix is: RGBA16f x 3
    //         animMat[0] += texelFetch(sampler2D(uAnimTex, smp), ivec2(matIdx + 0, 0), 0) * aWeights[i];
    //         animMat[1] += texelFetch(sampler2D(uAnimTex, smp), ivec2(matIdx + 1, 0), 0) * aWeights[i];
    //         animMat[2] += texelFetch(sampler2D(uAnimTex, smp), ivec2(matIdx + 2, 0), 0) * aWeights[i]; 
    //     }
    //     // vBoneIdx = int(aJoints[0]);
    //     model = model * transpose(animMat);
    // }

    mediump mat3 normalMatrix = adjoint(model);
    vTBN[0] = normalize(normalMatrix * aTangent.xyz); 
    vTBN[2] = normalize(normalMatrix * aNormal);
    vTBN[1] = cross(vTBN[0], vTBN[2]) * aTangent.w;
    
    highp vec4 outPos = model * vec4(aPos, 1.0);
    vTexCoords  = aTexCoords; 
    gl_Position = uViewProj * outPos;
}
@end

@fs fs
layout(binding = 0) uniform lowp texture2D tex;
layout(binding = 0) uniform sampler texSampler;

in mediump vec2 vTexCoords;
in lowp mat3 vTBN;

out lowp vec4 frag_color;

void main() {
    frag_color = texture(sampler2D(tex, texSampler), vTexCoords); //  * color;
}
@end
@program cube vs fs