#ifndef MATRIX_H
#define MATRIX_H

#include "Quaternion.h"

/*********************************************************************************
    *    Description:                                                                *
    *        Row major right handed vectorized M33x3 and M44x4 structures.   *
    *        That is capable of creating and manipulating position, rotation, scale, *
    *        view and projection matrices.                                           * 
    *    Note:                                                                       *
    *        There is frustum culling code at bottom most lines of this file.        *
    *    Author:                                                                     *
    *        Anilcan Gulkaya 2025 anilcangulkaya7@gmail.com github @benanil          *
    *********************************************************************************/

typedef union M44_ {
    v128f r[4];
    f32 m[4][4];
} mat4x4;


typedef union M33_ {
    float3 r[3];
} mat3x3;

typedef union float3x4_ {
    float3 r[4];
} m34;

typedef struct FrustumPlanes_ {
    v128f planes[6];
} FrustumPlanes;

typedef struct f16_2x4_
{
    f16_4 x, y;
} f16_2x4;

typedef v128f f16_3x4[3];
typedef mat4x4 f16_4x4;

purefn mat3x3 M33Multiply(mat3x3 a, mat3x3 b)
{
    mat3x3 result;
    float3 vx = F3Mul(b.r[0], a.r[0]);
    float3 vy = F3Mul(b.r[1], a.r[0]);
    float3 vz = F3Mul(b.r[2], a.r[0]);
    result.r[0] = F3Add(F3Add(vx, vy), vz);
        
    vx = F3Mul(b.r[0], a.r[1]);
    vy = F3Mul(b.r[1], a.r[1]);
    vz = F3Mul(b.r[2], a.r[1]);
    result.r[1] = F3Add(F3Add(vx, vy), vz);
        
    vx = F3Mul(b.r[0], a.r[2]);
    vy = F3Mul(b.r[1], a.r[2]);
    vz = F3Mul(b.r[2], a.r[2]);
    result.r[2] = F3Add(F3Add(vx, vy), vz);
    return result;
}

purefn float3 M33MultiplyF3(mat3x3 m, float3 v)
{
    return F3Add(F3Add(F3MulF(m.r[0], v.x), F3MulF(m.r[1], v.y)), F3MulF(m.r[2], v.z));
}
    
static inline mat3x3 TBN(float3 normal, float3 tangent, float3 bitangent)
{
    mat3x3 M;
    M.r[0] = normal;
    M.r[1] = tangent;
    M.r[2] = bitangent;
    return M;
}

static inline mat3x3 Identity()
{
    mat3x3 result;
    result.r[0] = (float3){1.0f, 0.0f, 0.0f};
    result.r[1] = (float3){0.0f, 1.0f, 0.0f};
    result.r[2] = (float3){0.0f, 0.0f, 1.0f};
    return result;
}

purefn mat3x3 M33LookAt(float3 direction, float3 up)
{
    mat3x3 result;
    result.r[2] = direction;
    float3 Right = F3Cross(&up, &result.r[2]);
    result.r[0] = F3MulF(Right, RSqrtf(MMAX(0.00001f, F3Dot(Right, Right))));
    result.r[1] = F3Cross(&result.r[2], &result.r[0]);
    return result;
}

purefn Quaternion M33ToQuaternion(mat3x3 m)
{
    Quaternion Orientation;
    QuaternionFromMatrix((float*)&Orientation, &m.r[0].x, 3);
    return Orientation;
}

purefn mat4x4 VCALL M44Identity()
{
    mat4x4 M;
    M.r[0] = VecIdentityR0;
    M.r[1] = VecIdentityR1;
    M.r[2] = VecIdentityR2;
    M.r[3] = VecIdentityR3;
    return M;
}

purefn mat4x4 M44FromPosition(f32 x, f32 y, f32 z)
{
    mat4x4 M;
    M.r[0] = VecIdentityR0;
    M.r[1] = VecIdentityR1;
    M.r[2] = VecIdentityR2;
    M.r[3] = VecSetR(x, y, z, 1.0f);
    return M;
}

purefn mat4x4 M44FromPositionPtr(const f32* vec3)
{
    return M44FromPosition(vec3[0], vec3[1], vec3[2]);
}
    
purefn mat4x4 M44FromPositionF3(float3 vec3)
{
    return M44FromPosition(vec3.x, vec3.y, vec3.z);
}

purefn mat4x4 M44FromScale(f32 ScaleX, f32 ScaleY, f32 ScaleZ)
{
    mat4x4 M;
    M.r[0] = VecSetR(ScaleX, 0.0f, 0.0f, 0.0f);
    M.r[1] = VecSetR(0.0f, ScaleY, 0.0f, 0.0f);
    M.r[2] = VecSetR(0.0f, 0.0f, ScaleZ, 0.0f);
    M.r[3] = VecIdentityR3;
    return M;
}
    
purefn mat4x4 M44FromScaleVec(float3 vec3)
{
    return M44FromScale(vec3.x, vec3.y, vec3.z);
}
    
purefn mat4x4 M44FromScalePtr(f32* vec3)
{
    return M44FromScale(vec3[0], vec3[1], vec3[2]);
}
    
purefn mat4x4 M44FromScalef(f32 scale)
{
    return M44FromScale(scale, scale, scale);
}

purefn mat4x4 M44CreateRotation(float3 right, float3 up, float3 forward)
{
    mat4x4 m;
    m.r[0] = Vec3Load(&right.x);
    m.r[1] = Vec3Load(&up.x);
    m.r[2] = Vec3Load(&forward.x);
    m.r[3] = VecIdentityR3;
    return m;
}

// this will not work on camera matrix this is for only transformation matricies
static inline mat4x4 VCALL InverseTransform(mat4x4 inM)
{
    mat4x4 out;
    // transpose 3x3, we know m03 = m13 = m23 = 0
    v128f t0 = VecShuffle_0101(inM.r[0], inM.r[1]); // 00, 01, 10, 11
    v128f t1 = VecShuffle_2323(inM.r[0], inM.r[1]); // 02, 03, 12, 13
    out.r[0] = VecShuffle(t0, inM.r[2], 0, 2, 0, 3); // 00, 10, 20, 23(=0)
    out.r[1] = VecShuffle(t0, inM.r[2], 1, 3, 1, 3); // 01, 11, 21, 23(=0)
    out.r[2] = VecShuffle(t1, inM.r[2], 0, 2, 2, 3); // 02, 12, 22, 23(=0)
        
    // (SizeSqr(mVec[0]), SizeSqr(mVec[1]), SizeSqr(mVec[2]), 0)
    v128f sizeSqr;
    sizeSqr = VecMul(out.r[0], out.r[0]);
    sizeSqr = VecFmadd(out.r[1], out.r[1], sizeSqr);
    sizeSqr = VecFmadd(out.r[2], out.r[2], sizeSqr);
        
    // optional test to avoid divide by 0
    const v128f one = VecOne();
    // for each component, if(sizeSqr < SMALL_NUMBER) sizeSqr = 1;
    v128f rSizeSqr = VecSelect(
        VecDiv(one, sizeSqr), one,
        VecCmpLt(sizeSqr, VecSet1(1.e-8f))
    );
        
    out.r[0] = VecMul(out.r[0], rSizeSqr);
    out.r[1] = VecMul(out.r[1], rSizeSqr);
    out.r[2] = VecMul(out.r[2], rSizeSqr);
    // last line
    out.r[3] =       VecMul(out.r[0], VecSplatX(inM.r[3]));
    out.r[3] = VecFmaddLane(out.r[1], inM.r[3], out.r[3], 1);
    out.r[3] = VecFmaddLane(out.r[2], inM.r[3], out.r[3], 2);
    out.r[3] = VecSub(VecSetR(0.f, 0.f, 0.f, 1.f), out.r[3]);
    return out;
}
    
#if !defined(AX_ARM)

// https://lxjk.github.io/2017/09/03/Fast-4x4-Matrix-Inverse-with-SSE-SIMD-Explained.html
// for row major matrix
// we use Vector4x32f to represent 2x2 matrix as A = | A0  A1 |
//                                             | A2  A3 |
// 2x2 row major Matrix multiply A*B
purefn v128f VCALL Mat2Mul(v128f vec1, v128f vec2)
{
    return VecAdd(VecMul(vec1, VecSwizzle(vec2, 0, 3, 0, 3)), 
                  VecMul(VecSwizzle(vec1, 1, 0, 3, 2), VecSwizzle(vec2, 2, 1, 2, 1)));
}
// 2x2 row major Matrix adjugate multiply (A#)*B
purefn v128f VCALL Mat2AdjMul(v128f vec1, v128f vec2)
{
    return VecSub(VecMul(VecSwizzle(vec1, 3, 3, 0, 0), vec2),
                  VecMul(VecSwizzle(vec1, 1, 1, 2, 2), VecSwizzle(vec2, 2, 3, 0, 1)));
}

// 2x2 row major Matrix multiply adjugate A*(B#)
purefn v128f VCALL Mat2MulAdj(v128f vec1, v128f vec2)
{
    return VecSub(VecMul(vec1, VecSwizzle(vec2, 3, 0, 3, 0)),
                  VecMul(VecSwizzle(vec1, 1, 0, 3, 2), VecSwizzle(vec2, 2, 1, 2, 1)));
}

static inline mat4x4 VCALL M44Inverse(mat4x4 m)
{
    v128f A = VecShuffle_0101(m.r[0], m.r[1]);
    v128f B = VecShuffle_2323(m.r[0], m.r[1]);
    v128f C = VecShuffle_0101(m.r[2], m.r[3]);
    v128f D = VecShuffle_2323(m.r[2], m.r[3]);
        
    v128f detSub = VecSub(
        VecMul(VecShuffle(m.r[0], m.r[2], 0, 2, 0, 2), VecShuffle(m.r[1], m.r[3], 1, 3, 1, 3)),
        VecMul(VecShuffle(m.r[0], m.r[2], 1, 3, 1, 3), VecShuffle(m.r[1], m.r[3], 0, 2, 0, 2))
    );
    v128f detA = VecSplatX(detSub);
    v128f detB = VecSplatY(detSub);
    v128f detC = VecSplatZ(detSub);
    v128f detD = VecSplatW(detSub);
        
    v128f D_C  = Mat2AdjMul(D, C);
    v128f A_B  = Mat2AdjMul(A, B);
    v128f X_   = VecSub(VecMul(detD, A), Mat2Mul(B, D_C));
    v128f W_   = VecSub(VecMul(detA, D), Mat2Mul(C, A_B));
        
    v128f detM = VecMul(detA, detD);
    v128f Y_   = VecSub(VecMul(detB, C), Mat2MulAdj(D, A_B));
    v128f Z_   = VecSub(VecMul(detC, B), Mat2MulAdj(A, D_C));
        
    detM = VecFmadd(detB, detC, detM);
        
    v128f tr = VecMul(A_B, VecSwizzle(D_C, 0, 2, 1, 3));
    tr   = VecHadd(tr, tr);
    tr   = VecHadd(tr, tr);
    detM = VecSub(detM, tr);
        
    const v128f adjSignMask = VecSetR(1.f, -1.f, -1.f, 1.f);
    v128f rDetM = VecDiv(adjSignMask, detM);
    X_ = VecMul(X_, rDetM);
    Y_ = VecMul(Y_, rDetM);
    Z_ = VecMul(Z_, rDetM);
    W_ = VecMul(W_, rDetM);
        
    mat4x4 out;
    out.r[0] = VecShuffle(X_, Y_, 3, 1, 3, 1);
    out.r[1] = VecShuffle(X_, Y_, 2, 0, 2, 0);
    out.r[2] = VecShuffle(Z_, W_, 3, 1, 3, 1);
    out.r[3] = VecShuffle(Z_, W_, 2, 0, 2, 0);
    return out;
}
#else
static inline mat4x4 VCALL M44Inverse(mat4x4 mat)
{
    float32x4_t v0, v1, v2, v3,
    t0, t1, t2, t3, t4, t5,
    x0, x1, x2, x3, x4, x5, x6, x7, x8;
    float32x4x2_t a1;
    float32x2_t   lp, ko, hg, jn, im, fe, ae, bf, cg, dh;
    float32x4_t   x9 = VecSetR(-0.f,  0.f, -0.f,  0.f);
        
    x8 = vrev64q_f32(x9);
    /* l p k o, j n i m */
    a1  = vzipq_f32(mat.r[3], mat.r[2]);
    jn  = vget_high_f32(a1.val[0]);
    im  = vget_low_f32(a1.val[0]);
    lp  = vget_high_f32(a1.val[1]);
    ko  = vget_low_f32(a1.val[1]);
    hg  = vget_high_f32(mat.r[1]);
        
    x1  = vcombine_f32(vdup_lane_f32(lp, 0), lp);                   /* l p p p */
    x2  = vcombine_f32(vdup_lane_f32(ko, 0), ko);                   /* k o o o */
    x0  = vcombine_f32(vdup_lane_f32(lp, 1), vdup_lane_f32(hg, 1)); /* h h l l */
    x3  = vcombine_f32(vdup_lane_f32(ko, 1), vdup_lane_f32(hg, 0)); /* g g k k */
        
    t0 = vmlsq_f32(vmulq_f32(x3, x1), x2, x0);
    fe = vget_low_f32(mat.r[1]);
    x4 = vcombine_f32(vdup_lane_f32(jn, 0), jn);                   /* j n n n */
    x5 = vcombine_f32(vdup_lane_f32(jn, 1), vdup_lane_f32(fe, 1)); /* f f j j */
        
    t1 = vmlsq_f32(vmulq_f32(x5, x1), x4, x0);
    t2 = vmlsq_f32(vmulq_f32(x5, x2), x4, x3);
        
    x6 = vcombine_f32(vdup_lane_f32(im, 1), vdup_lane_f32(fe, 0)); /* e e i i */
    x7 = vcombine_f32(vdup_lane_f32(im, 0), im);                   /* i m m m */
        
    t3 = vmlsq_f32(vmulq_f32(x6, x1), x7, x0);
    t4 = vmlsq_f32(vmulq_f32(x6, x2), x7, x3);
    t5 = vmlsq_f32(vmulq_f32(x6, x4), x7, x5);
        
    /* h d f b, g c e a */
    a1 = vtrnq_f32(mat.r[0], mat.r[1]);
    x4 = vrev64q_f32(a1.val[0]); /* c g a e */
    x5 = vrev64q_f32(a1.val[1]); /* d h b f */
        
    ae = vget_low_f32(x4);
    cg = vget_high_f32(x4);
    bf = vget_low_f32(x5);
    dh = vget_high_f32(x5);
        
    x0 = vcombine_f32(ae, vdup_lane_f32(ae, 1)); /* a a a e */
    x1 = vcombine_f32(bf, vdup_lane_f32(bf, 1)); /* b b b f */
    x2 = vcombine_f32(cg, vdup_lane_f32(cg, 1)); /* c c c g */
    x3 = vcombine_f32(dh, vdup_lane_f32(dh, 1)); /* d d d h */
        
    v0 = VecXor(vmlaq_f32(vmlsq_f32(vmulq_f32(x1, t0), x2, t1), x3, t2), x8);
    v2 = VecXor(vmlaq_f32(vmlsq_f32(vmulq_f32(x0, t1), x1, t3), x3, t5), x8);
    v1 = VecXor(vmlaq_f32(vmlsq_f32(vmulq_f32(x0, t0), x2, t3), x3, t4), x9);
    v3 = VecXor(vmlaq_f32(vmlsq_f32(vmulq_f32(x0, t2), x1, t4), x2, t5), x9);
    /* determinant */
    x0 = vcombine_f32(vget_low_f32(vzipq_f32(v0, v1).val[0]),
                      vget_low_f32(vzipq_f32(v2, v3).val[0]));

    mat4x4 dest;
    // newton-raphson refined divide, the bare vrecpeq estimate is only ~8 bits
    x0 = ARMVectorDevide(VecOne(), VecHSum(vmulq_f32(x0, mat.r[0])));
    dest.r[0] = vmulq_f32(v0, x0);
    dest.r[1] = vmulq_f32(v1, x0);
    dest.r[2] = vmulq_f32(v2, x0);
    dest.r[3] = vmulq_f32(v3, x0);
    return dest;
}
#endif

purefn mat4x4 VCALL M44Multiply(mat4x4 in0, const mat4x4 in1)
{
    v128f m0, m1, m2, m3;
    m0 = VecMul(in1.r[0], VecSplatX(in0.r[0]));
    m0 = VecFmaddLane(in1.r[1], in0.r[0], m0, 1);
    m0 = VecFmaddLane(in1.r[2], in0.r[0], m0, 2); 
    m0 = VecFmaddLane(in1.r[3], in0.r[0], m0, 3); 
    in0.r[0] = m0;
        
    m1 = VecMul(in1.r[0], VecSplatX(in0.r[1]));
    m1 = VecFmaddLane(in1.r[1], in0.r[1], m1, 1); 
    m1 = VecFmaddLane(in1.r[2], in0.r[1], m1, 2); 
    m1 = VecFmaddLane(in1.r[3], in0.r[1], m1, 3); 
    in0.r[1] = m1;
        
    m2 = VecMul(in1.r[0], VecSplatX(in0.r[2]));
    m2 = VecFmaddLane(in1.r[1], in0.r[2], m2, 1); 
    m2 = VecFmaddLane(in1.r[2], in0.r[2], m2, 2); 
    m2 = VecFmaddLane(in1.r[3], in0.r[2], m2, 3); 
    in0.r[2] = m2;
        
    m3 = VecMul(in1.r[0], VecSplatX(in0.r[3]));
    m3 = VecFmaddLane(in1.r[1], in0.r[3], m3, 1); 
    m3 = VecFmaddLane(in1.r[2], in0.r[3], m3, 2); 
    m3 = VecFmaddLane(in1.r[3], in0.r[3], m3, 3); 
    in0.r[3] = m3;
    return in0;
}

purefn v128f VCALL Vec4Transform(v128f v, const v128f r[4])
{
    v128f m0;
    m0 = VecMul(r[0], VecSplatX(v));
    m0 = VecFmaddLane(r[1], v, m0, 1);
    m0 = VecFmaddLane(r[2], v, m0, 2); 
    return VecFmaddLane(r[3], v, m0, 3);
}

purefn v128f VCALL Vec3Transform(v128f vec, const v128f r[4])
{
    v128f m0;
    m0 = VecMul(r[0], VecSplatX(vec));
    m0 = VecFmaddLane(r[1], vec, m0, 1);
    m0 = VecFmaddLane(r[2], vec, m0, 2); 
    return VecAdd(r[3], m0);
}

purefn mat4x4 PerspectiveFovRH(f32 fov, f32 width, f32 height, f32 zNear, f32 zFar)
{
    f32 rad = Sin0pi(0.5f * fov);
    AX_ASSUME(rad > 0.01f);
    f32 h = Sqrtf(1.0f - (rad * rad)) / rad;
    f32 w = h * height / width; /// max(width , Height) / min(width , Height)?
    mat4x4 M = {0};
    M.m[0][0] = w;
    M.m[1][1] = h;
    M.m[2][2] = -(zFar + zNear) / (zFar - zNear);
    M.m[2][3] = -1.0f;
    M.m[3][2] = -(2.0f * zFar * zNear) / (zFar - zNear);
    M.m[3][3] = 0.0f;
    return M;
}

// Reversed-Z perspective in the D3D/Vulkan 0..1 clip-z convention: the near plane
// maps to ndc z = 1 and the far plane to ndc z = 0. Pairs with a 0.0 depth clear and a
// GREATER_OR_EQUAL depth test to give vastly better depth precision than the GL-style
// PerspectiveFovRH above. Used only by the main camera; shadows keep PerspectiveFovRH.
purefn mat4x4 PerspectiveFovRH_ReverseZ(f32 fov, f32 width, f32 height, f32 zNear, f32 zFar)
{
    f32 rad = Sin0pi(0.5f * fov);
    AX_ASSUME(rad > 0.01f);
    f32 h = Sqrtf(1.0f - (rad * rad)) / rad;
    f32 w = h * height / width;
    mat4x4 M = {0};
    M.m[0][0] = w;
    M.m[1][1] = h;
    M.m[2][2] = zNear / (zFar - zNear);
    M.m[2][3] = -1.0f;
    M.m[3][2] = (zFar * zNear) / (zFar - zNear);
    M.m[3][3] = 0.0f;
    return M;
}

purefn mat4x4 VCALL M44Transpose(mat4x4 M)
{
    mat4x4 mResult;
    #ifdef AX_ARM
    float32x4x2_t P0 = vzipq_f32(M.r[0], M.r[2]);
    float32x4x2_t P1 = vzipq_f32(M.r[1], M.r[3]);
    float32x4x2_t T0 = vzipq_f32(P0.val[0], P1.val[0]);
    float32x4x2_t T1 = vzipq_f32(P0.val[1], P1.val[1]);
    mResult.r[0] = T0.val[0];
    mResult.r[1] = T0.val[1];
    mResult.r[2] = T1.val[0];
    mResult.r[3] = T1.val[1];
    #else
    v128f vTemp1 = VecShuffleR(M.r[0], M.r[1], 1, 0, 1, 0);
    v128f vTemp3 = VecShuffleR(M.r[0], M.r[1], 3, 2, 3, 2);
    v128f vTemp2 = VecShuffleR(M.r[2], M.r[3], 1, 0, 1, 0);
    v128f vTemp4 = VecShuffleR(M.r[2], M.r[3], 3, 2, 3, 2);
    mResult.r[0] = VecShuffleR(vTemp1, vTemp2, 2, 0, 2, 0);
    mResult.r[1] = VecShuffleR(vTemp1, vTemp2, 3, 1, 3, 1);
    mResult.r[2] = VecShuffleR(vTemp3, vTemp4, 2, 0, 2, 0);
    mResult.r[3] = VecShuffleR(vTemp3, vTemp4, 3, 1, 3, 1);
    #endif
    return mResult;
}

purefn mat4x4 VCALL M44LookAtRHVec(v128f EyePosition, v128f Center, v128f UpDirection)
{
    v128f EyeDirection = VecSub(VecZero(), Center);
    v128f R0 = Vec3Cross(UpDirection, EyeDirection); 
    R0 = Vec3NormEstV(R0);
    v128f R1 = Vec3Cross(EyeDirection, R0); 
    R1 = Vec3NormEstV(R1);
        
    v128f NegEyePosition = VecSub(VecZero(), EyePosition);
    v128f D0 = Vec3DotV(R0, NegEyePosition);
    v128f D1 = Vec3DotV(R1, NegEyePosition);
    v128f D2 = Vec3DotV(EyeDirection, NegEyePosition);
    
    mat4x4 M;
    v128f test = VecSelect1110;
    M.r[0] = VecSelect(D0, R0, test); // no need select ?
    M.r[1] = VecSelect(D1, R1, test);
    M.r[2] = VecSelect(D2, EyeDirection, test);
    M.r[3] = VecIdentityR3;
    return M44Transpose(M);
}

purefn mat4x4 VCALL M44LookAtRH(const float3 eye, const float3 center, const float3 up)
{
    return M44LookAtRHVec(VecLoad(&eye.x), VecLoad(&center.x), VecLoad(&up.x));
}

purefn mat4x4 M44OrthoRH(f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar)
{
    mat4x4 Result = {0};
    Result.m[0][0] =  2.0f / (right - left);
    Result.m[1][1] =  2.0f / (top - bottom);
    Result.m[2][2] = -2.0f / (zFar - zNear);
    Result.m[3][0] = -(right + left) / (right - left);
    Result.m[3][1] = -(top + bottom) / (top - bottom);
    Result.m[3][2] = -(zFar + zNear) / (zFar - zNear);
    Result.m[3][3] = 1.0f;
    return Result;
}

purefn mat4x4 M44PositionRotationScaleVec(v128f position, Quaternion rotation, v128f scale)
{
    mat4x4 res = {0}; // M44FromQuaternion only writes the 3x3 part, w lanes must be 0
    // Export rotation to matrix
    M44FromQuaternion(&res.m[0][0], rotation);
    // Scale 3x3 matrix by given scale
    res.r[0] = VecMul(res.r[0], VecSplatX(scale));
    res.r[1] = VecMul(res.r[1], VecSplatY(scale));
    res.r[2] = VecMul(res.r[2], VecSplatZ(scale));
    // Third row is position, x, y, z, 1.0f
    res.r[3] = position;
    VecSetW(res.r[3], 1.0f);
    return res; 
}

purefn mat4x4 M44PositionRotationVec(v128f position, Quaternion rotation)
{
    mat4x4 res = {0}; // M44FromQuaternion only writes the 3x3 part, w lanes must be 0
    M44FromQuaternion(&res.m[0][0], rotation);
    res.r[3] = position;
    VecSetW(res.r[3], 1.0f);
    return res; 
}

purefn mat4x4 M44PositionRotationScale(float3 position, Quaternion rotation, float3 scale)
{
    return M44PositionRotationScaleVec(VecLoad(&position.x), rotation, VecSetR(scale.x, scale.y, scale.z, 0.0f)); 
}

purefn mat4x4 M44PositionRotationScalePtr(const f32* position, const f32* rotation, const f32* scale)
{
    mat4x4 res = {0};
    // Export rotation to matrix
    M44FromQuaternion(&res.m[0][0], VecLoad(rotation));
    // Scale 3x3 matrix by given scale
    v128f vecScale = VecLoad(scale);
    res.r[0] = VecMul(res.r[0], VecSplatX(vecScale));
    res.r[1] = VecMul(res.r[1], VecSplatY(vecScale));
    res.r[2] = VecMul(res.r[2], VecSplatZ(vecScale));
    // Third row is position, x, y, z, 1.0f
    res.r[3] = VecLoad(position);
    VecSetW(res.r[3], 1.0f);
    return res; 
}

purefn float3 VCALL M44ExtractPosition(mat4x4 matrix)
{
    float3 res;
    Vec3Store(&res.x, matrix.r[3]);
    return res;
}
    
purefn Quaternion VCALL M44ExtractRotation(mat4x4 M, u8 rowNormalize) 
{
    Quaternion res;
    QuaternionFromMatrix((f32*)&res, &M.m[0][0], 4);
    return res;
}
    
purefn float3 VCALL M44ExtractScale(mat4x4 matrix)
{
    return (float3){ Vec3LenfV(matrix.r[0]), Vec3LenfV(matrix.r[1]), Vec3LenfV(matrix.r[2]) };
}

purefn v128f VCALL M44ExtractScaleV(mat4x4 matrix)
{
    return VecSetR(Vec3LenfV(matrix.r[0]), Vec3LenfV(matrix.r[1]), Vec3LenfV(matrix.r[2]), 0.0f);
}

inline mat4x4 VCALL M44FromQuaternionV(Quaternion q)
{
    #if defined(AX_ARM)
    mat4x4 mat = {};
    M44FromQuaternion(&mat.m[0][0], q);
    mat.m[3][3] = 1.0f;
    return mat;
    #else
    mat4x4 M;
    const v128f  Constant1110 = VecSetR(1.0f, 1.0f, 1.0f, 0.0f);
        
    v128f Q0 = VecAdd(q, q);
    v128f Q1 = VecMul(q,Q0);
    v128f V0 = VecShuffleR(Q1, Q1, 3,0,0,1);
    v128f V1 = VecShuffleR(Q1, Q1, 3,1,2,2);
    V0 = VecMask(V0, VecMask3);
    V1 = VecMask(V1, VecMask3);
    v128f  R0 = VecSub(Constant1110, V0);
    R0 = VecSub(R0, V1);
        
    V0 = VecShuffleR(q, q, 3,1,0,0);
    V1 = VecShuffleR(Q0, Q0, 3,2,1,2);
    V0 = VecMul(V0, V1);
        
    V1 = VecShuffleR(q, q, 3,3,3,3);
    v128f V2 = VecShuffleR(Q0, Q0, 3,0,2,1);
    V1 = VecMul(V1, V2);
        
    v128f R1 = VecAdd(V0, V1);
    v128f R2 = VecSub(V0, V1);
        
    V0 = VecShuffleR(R1, R2, 1, 0, 2, 1);
    V0 = VecShuffleR(V0, V0, 1, 3, 2, 0);
    V1 = VecShuffleR(R1, R2, 2, 2, 0, 0);
    V1 = VecShuffleR(V1, V1, 2, 0, 2, 0);
        
    Q1 = VecShuffleR(R0, V0, 1, 0, 3, 0);
    Q1 = VecShuffleR(Q1, Q1, 1, 3, 2, 0);
    M.r[0] = Q1;
    Q1 = VecShuffleR(R0, V0, 3, 2, 3, 1);
    Q1 = VecShuffleR(Q1, Q1, 1, 3, 0, 2);
    M.r[1] = Q1;
    Q1 = VecShuffleR(V1, R0, 3, 2, 1, 0);
    M.r[2] = Q1;
    M.r[3] = VecIdentityR3;
    return M;
    #endif
}
    
static inline mat4x4 VCALL M44FromQuaternionF(const f32* quaternion)
{
    return M44FromQuaternionV(MakeQuat(quaternion[0], quaternion[1], quaternion[2], quaternion[3]));
}

purefn mat4x4 M44RotationX(f32 angleRadians) {
    mat4x4 out_matrix = M44Identity();
    f32 s, c;
    SinCos(angleRadians, &s, &c);
    out_matrix.m[1][1] = c;
    out_matrix.m[1][2] = s;
    out_matrix.m[2][1] = -s;
    out_matrix.m[2][2] = c;
    return out_matrix;
}
    
purefn mat4x4 M44RotationY(f32 angleRadians) {
    mat4x4 out_matrix = M44Identity();
    f32 s, c;
    SinCos(angleRadians, &s, &c);
    out_matrix.m[0][0] = c;
    out_matrix.m[0][2] = -s;
    out_matrix.m[2][0] = s;
    out_matrix.m[2][2] = c;
    return out_matrix;
}
    
purefn mat4x4 M44RotationZ(f32 angleRadians) {
    mat4x4 out_matrix = M44Identity();
    f32 s, c;
    SinCos(angleRadians, &s, &c);
    out_matrix.m[0][0] = c;
    out_matrix.m[0][1] = s;
    out_matrix.m[1][0] = -s;
    out_matrix.m[1][1] = c;
    return out_matrix;
}

purefn FrustumPlanes VCALL CreateFrustumPlanes(mat4x4 viewProjection)
{
    FrustumPlanes result; // normalize each plane if you want to do sphere or cone intersection
    mat4x4 C = M44Transpose(viewProjection);
    result.planes[0] = VecAdd(C.r[3], C.r[0]); // m_left_plane
    result.planes[1] = VecSub(C.r[3], C.r[0]); // m_right_plane
    result.planes[2] = VecAdd(C.r[3], C.r[1]); // m_bottom_plane
    result.planes[3] = VecSub(C.r[3], C.r[1]); // m_top_plane
    result.planes[4] = VecSub(C.r[3], C.r[2]); // m_far_plane
    // the projection is gl style (-w..w clip z), so the near plane is r3 + r2.
    // bare C.r[2] is the d3d 0..1 convention and over-culls between near and ~2x near
    result.planes[5] = VecAdd(C.r[3], C.r[2]); // m_near_plane
    return result;
}

// Same as CreateFrustumPlanes but for a D3D/Vulkan 0..1 clip-z projection (the reversed-Z
// camera matrix). The only difference is the near plane: in the 0..1 convention it is the
// bare C.r[2] row (z_clip >= 0) rather than r3 + r2. Reversed-Z swaps the near/far labels of
// the two z-planes, but {C.r[2], C.r[3]-C.r[2]} still bounds the frustum inward-oriented.
purefn FrustumPlanes VCALL CreateFrustumPlanesRevZ(mat4x4 viewProjection)
{
    FrustumPlanes result;
    mat4x4 C = M44Transpose(viewProjection);
    result.planes[0] = VecAdd(C.r[3], C.r[0]); // m_left_plane
    result.planes[1] = VecSub(C.r[3], C.r[0]); // m_right_plane
    result.planes[2] = VecAdd(C.r[3], C.r[1]); // m_bottom_plane
    result.planes[3] = VecSub(C.r[3], C.r[1]); // m_top_plane
    result.planes[4] = VecSub(C.r[3], C.r[2]); // m_far_plane
    result.planes[5] = C.r[2];                 // m_near_plane (d3d 0..1)
    return result;
}

static inline void NormalizeFrustumPlanes(FrustumPlanes* fp)
{
    fp->planes[0] = VecNorm(fp->planes[0]);
    fp->planes[1] = VecNorm(fp->planes[1]);
    fp->planes[2] = VecNorm(fp->planes[2]);
    fp->planes[3] = VecNorm(fp->planes[3]);
    fp->planes[4] = VecNorm(fp->planes[4]);
	fp->planes[5] = VecNorm(fp->planes[5]);
}

purefn v128f VCALL MaxPointAlongNormal(v128f min, v128f max, v128f n)
{
    return VecSelect(min, max, VecCmpGe(n, VecZero()));
}

static inline bool VCALL CheckAABBCulled(v128f min, v128f max, const v128f frustumPlanes[6])
{
    for (u32 i = 0u; i < 6u; ++i) // make < 6 if you want far plane 
    {
        v128f p = MaxPointAlongNormal(min, max, frustumPlanes[i]);
        p = VecSelect(VecOne(), p, VecSelect1110);
        if (VecDotf(frustumPlanes[i], p) < 0.0f) return false;
    }
    return true;
}

static inline bool isPointCulled(float3 _point, mat4x4 matrix, const v128f frustumPlanes[6])
{
    v128f point = Vec3Transform(VecLoad(&_point.x), matrix.r);
    if (VecDotf(frustumPlanes[0], point) < 0.0f) return false;
    if (VecDotf(frustumPlanes[1], point) < 0.0f) return false;
    if (VecDotf(frustumPlanes[2], point) < 0.0f) return false;
    if (VecDotf(frustumPlanes[3], point) < 0.0f) return false;
    if (VecDotf(frustumPlanes[4], point) < 0.0f) return false;
    return true;
}

static inline float2 WorldToNDC(mat4x4 viewProj, float3 worldPos)
{
    v128f pos = VecLoad(&worldPos.x);
    v128f clipCoords = Vec3Transform(pos, viewProj.r);
    pos = VecDiv(clipCoords, VecSplatW(clipCoords));
    Vec3Store(&worldPos.x, pos);
    return (float2){ worldPos.x, worldPos.y };
}

static inline float2 WorldToScreenCoord(mat4x4 viewProj, float3 worldPos, int width, int height)
{
    float2 ndc = WorldToNDC(viewProj, worldPos);
    return (float2){ (width  * ndc.x), (height * ndc.y) };
}

purefn mat4x4 DQToMatrix(DualQuaternion dq)
{
    mat4x4 m = M44Identity();
    // Build rotation matrix from real part
    M44FromQuaternion(&m.m[0][0], dq.real);
    
    // Extract translation: t = 2 * dual * conjugate(real)
    v128f real_conj = QConjugate(dq.real);
    v128f t_quat = QMul(VecMul(dq.dual, VecSet1(2.0f)), real_conj);
    
    m.r[3] = t_quat;
    VecSetW(m.r[3], 1.0f);
    return m;
}

#endif