
#ifndef SIMD_H
#define SIMD_H

// this is a helper file for working with ARM neon and SSE-SSE4.2 intrinsics, 
// But everytime writing both versions can be pain, so I’ve used macros for this purpose,.
// another reason why I’ve used macros is if I use operator overriding or functions for abstracting intrinsics,
// in debug mode, code compiles down to call instruction which is slower than instruction itself
// and macro's allows many more optimizations that compiler can do.
// a bit more explanation here: https://medium.com/@anilcangulkaya7/what-is-simd-and-how-to-use-it-3d1125faac89

#include "Common.h"

#if defined(AX_SUPPORT_SSE) && !defined(AX_ARM)
/*//////////////////////////////////////////////////////////////////////////*/
/*                                 SSE                                      */
/*//////////////////////////////////////////////////////////////////////////*/

typedef __m128  Vector4x32f;
typedef __m128  Vector4x32i;
typedef __m128i Vector4x32u;

#define VecZero()            _mm_setzero_ps()
#define VecOne()             _mm_set1_ps(1.0f)
#define VecNegativeOne()     _mm_setr_ps( -1.0f, -1.0f, -1.0f, -1.0f)
#define VecSet1(x)           _mm_set1_ps(x)

#define VecSet(x, y, z, w)   _mm_set_ps(x, y, z, w)  /* -> {w, z, y, x} */
#define VecSetR(x, y, z, w)  _mm_setr_ps(x, y, z, w) /* -> {x, y, z, w} */
#define VecLoad(x)           _mm_loadu_ps(x)
#define VecLoadA(x)          _mm_load_ps(x)

// #define Vec3Load(x)         VecSetW(_mm_loadu_ps(x), 0.0f)

#define VecStore(ptr, x)       _mm_storeu_ps(ptr, x)
#define VecStoreA(ptr, x)      _mm_store_ps(ptr, x)
#define VecFromInt(x, y, z, w) _mm_castsi128_ps(_mm_setr_epi32(x, y, z, w))
#define VecFromInt1(x)         _mm_castsi128_ps(_mm_set1_epi32(x))
#define VecToInt(x) x

#define VecFromVeci(x) _mm_castsi128_ps(x)
#define VeciFromVec(x) _mm_castps_si128(x)

#define VecCvtF32U32(x) _mm_cvtps_epi32(x)
#define VecCvtU32F32(x) _mm_cvtepi32_ps(x)

// _mm_permute_ps is avx only
// Get Set
#define VecSplatX(v) _mm_permute_ps(v, MakeShuffleMask(0, 0, 0, 0)) /* { v.x, v.x, v.x, v.x} */
#define VecSplatY(v) _mm_permute_ps(v, MakeShuffleMask(1, 1, 1, 1)) /* { v.y, v.y, v.y, v.y} */
#define VecSplatZ(v) _mm_permute_ps(v, MakeShuffleMask(2, 2, 2, 2)) /* { v.z, v.z, v.z, v.z} */
#define VecSplatW(v) _mm_permute_ps(v, MakeShuffleMask(3, 3, 3, 3)) /* { v.w, v.w, v.w, v.w} */

#define VecGetX(v) _mm_cvtss_f32(v)             /* return v.x */
#define VecGetY(v) _mm_cvtss_f32(VecSplatY(v))  /* return v.y */
#define VecGetZ(v) _mm_cvtss_f32(VecSplatZ(v))  /* return v.z */
#define VecGetW(v) _mm_cvtss_f32(VecSplatW(v))  /* return v.w */

// SSE4.1
#define VecSetX(v, x) ((v) = _mm_move_ss  ((v), _mm_set_ss(x)))
#define VecSetY(v, y) ((v) = _mm_insert_ps((v), _mm_set_ss(y), 0x10))
#define VecSetZ(v, z) ((v) = _mm_insert_ps((v), _mm_set_ss(z), 0x20))
#define VecSetW(v, w) ((v) = _mm_insert_ps((v), _mm_set_ss(w), 0x30))

purefn Vector4x32f VECTORCALL Vec3Load(void const* x) {
    Vector4x32f v = _mm_loadu_ps((float const*)x); 
    VecSetW(v, 0.0); return v;
}

// Arithmetic
#define VecAdd(a, b) _mm_add_ps(a, b)
#define VecSub(a, b) _mm_sub_ps(a, b)
#define VecMul(a, b) _mm_mul_ps(a, b)
#define VecDiv(a, b) _mm_div_ps(a, b)

#define VecAddf(a, b) _mm_add_ps(a, VecSet1(b))
#define VecSubf(a, b) _mm_sub_ps(a, VecSet1(b))
#define VecMulf(a, b) _mm_mul_ps(a, VecSet1(b))
#define VecDivf(a, b) _mm_div_ps(a, VecSet1(b))

// a * b[l] + c
#define VecFmaddLane(a, b, c, l) _mm_fmadd_ps(a, _mm_permute_ps(b, MakeShuffleMask(l, l, l, l)), c)

#define VecFmadd(a, b, c) _mm_fmadd_ps(a, b, c)
#define VecFmsub(a, b, c) _mm_fmsub_ps(a, b, c)
#define VecHadd(a, b) _mm_hadd_ps(a, b) /* pairwise add (aw+bz, ay+bx, aw+bz, ay+bx) */

#define VecNeg(a) _mm_sub_ps(_mm_setzero_ps(), a) /* -a */
#define VecRcp(a) _mm_rcp_ps(a) /* 1.0f / a */
#define VecSqrt(a) _mm_sqrt_ps(a)

#define VeciNeg(a) _mm_sub_epi32(_mm_set1_epi32(0), a) /* -a */

// Vector Math
#define VecDot(a, b)  _mm_dp_ps(a, b, 0xff)
#define VecDotf(a, b) _mm_cvtss_f32(_mm_dp_ps(a, b, 0xff))
#define VecNorm(v)    _mm_div_ps(v, _mm_sqrt_ps(_mm_dp_ps(v, v, 0xff)))
#define VecNormEst(v) _mm_mul_ps(_mm_rsqrt_ps(_mm_dp_ps(v, v, 0xff)), v)
#define VecLenf(v)    _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(v, v, 0xff)))
#define VecLen(v)     _mm_sqrt_ps(_mm_dp_ps(v, v, 0xff))

#define Vec3DotV(a, b)  _mm_dp_ps(a, b, 0x7f)
#define Vec3DotfV(a, b) _mm_cvtss_f32(_mm_dp_ps(a, b, 0x7f))
#define Vec3NormV(v)    _mm_div_ps(v, _mm_sqrt_ps(_mm_dp_ps(v, v, 0x7f)))
#define Vec3NormEstV(v) _mm_mul_ps(_mm_rsqrt_ps(_mm_dp_ps(v, v, 0x7f)), v)
#define Vec3LenfV(v)    _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(v, v, 0x7f)))
#define Vec3LenV(v)     _mm_sqrt_ps(_mm_dp_ps(v, v, 0x7f))

// Swizzling Masking
#define VecSelect1000  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000))
#define VecSelect1100  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000))
#define VecSelect1110  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000))
#define VecSelect1011  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF))
#define VecSelect1111  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF))

#define VecMaskXY _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000))
#define VecMask3  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000))
#define VecMaskX  _mm_castsi128_ps(_mm_setr_epi32(0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000))
#define VecMaskY  _mm_castsi128_ps(_mm_setr_epi32(0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000))
#define VecMaskZ  _mm_castsi128_ps(_mm_setr_epi32(0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000))
#define VecMaskW  _mm_castsi128_ps(_mm_setr_epi32(0x00000000, 0x00000000, 0x00000000, 0xFFFFFFFF))

#define MakeShuffleMask(x,y,z,w)     (x | (y<<2) | (z<<4) | (w<<6)) /* internal use only */
// vec(0, 1, 2, 3) -> (vec[x], vec[y], vec[z], vec[w])
#define VecSwizzleMask(vec, msk)     _mm_shuffle_ps(vec, vec, msk)
#define VecSwizzle(vec, x, y, z, w)  VecSwizzleMask(vec, MakeShuffleMask(x,y,z,w))

// return (vec1[x], vec1[y], vec2[z], vec2[w])
#define VecShuffle(vec1, vec2, x, y, z, w)   _mm_shuffle_ps(vec1, vec2, MakeShuffleMask(x,y,z,w))
#define VecShuffleR(vec1, vec2, x, y, z, w)  _mm_shuffle_ps(vec1, vec2, MakeShuffleMask(w,z,y,x))
// Special shuffle
#define VecShuffle_0101(vec1, vec2) _mm_movelh_ps(vec1, vec2)
#define VecShuffle_2323(vec1, vec2) _mm_movehl_ps(vec2, vec1)
#define VecRev(v) VecShuffle((v), (v), 3, 2, 1, 0)

// Logical
#define VecNot(a)       _mm_andnot_ps(a, VecSelect1111)
#define VecAnd(a, b)    _mm_and_ps(a, b)
#define VecOr(a, b)     _mm_or_ps(a, b)
#define VecXor(a, b)    _mm_xor_ps(a, b)
#define VecMask(a, msk) _mm_and_ps(a, msk)

#define VecMax(a, b) _mm_max_ps(a, b)
#define VecMin(a, b) _mm_min_ps(a, b)
#define VecFloor(a)  _mm_floor_ps(a)

#define VecCmpGt(a, b) _mm_cmpgt_ps(a, b) /* greater than */
#define VecCmpGe(a, b) _mm_cmpge_ps(a, b) /* greater or equal */
#define VecCmpLt(a, b) _mm_cmplt_ps(a, b) /* less than */
#define VecCmpLe(a, b) _mm_cmple_ps(a, b) /* less or equal */
#define VecMovemask(a) _mm_movemask_ps(a) /* */

#define VecSelect(V1, V2, Control)  _mm_blendv_ps(V1, V2, Control)
#define VecBlend(a, b, c) _mm_blendv_ps(a, b, c)

//------------------------------------------------------------------------
// Veci
#define VeciZero()            _mm_set1_epi32(0)
#define VeciSet1(x)          _mm_set1_epi32(x)
#define VeciSet(x, y, z, w)  _mm_set_epi32(x, y, z, w)
#define VeciSetR(x, y, z, w) _mm_setr_epi32(x, y, z, w)
#define VeciLoadA(x)         _mm_load_epi32(x)
#define VeciLoad(x)          _mm_loadu_epi32(x)
#define VeciLoad64(qword)    _mm_loadu_si64(qword)     /* loads 64bit integer to first 8 bytes of register */

// SSE4.1
#define VeciSetX(v, x) ((v) = _mm_insert_epi32((v), 0, x))
#define VeciSetY(v, y) ((v) = _mm_insert_epi32((v), 1, y))
#define VeciSetZ(v, z) ((v) = _mm_insert_epi32((v), 2, z))
#define VeciSetW(v, w) ((v) = _mm_insert_epi32((v), 3, w))

#define VeciSelect1111 _mm_set1_epi32(0xFFFFFFFF)

#define VecIdentityR0 _mm_setr_ps(1.0f, 0.0f, 0.0f, 0.0f)
#define VecIdentityR1 _mm_setr_ps(0.0f, 1.0f, 0.0f, 0.0f)
#define VecIdentityR2 _mm_setr_ps(0.0f, 0.0f, 1.0f, 0.0f)
#define VecIdentityR3 _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)

#define VeciAdd(a, b) _mm_add_epi32(a, b)
#define VeciSub(a, b) _mm_sub_epi32(a, b)
#define VeciMul(a, b) _mm_mul_epi32(a, b)

#define VeciNot(a)             _mm_andnot_si128(a, _mm_set1_epi32(0xFFFFFFFF))
#define VeciAnd(a, b)          _mm_and_si128(a, b)
#define VeciOr(a, b)           _mm_or_si128(a, b)
#define VeciXor(a, b)          _mm_xor_si128(a, b)

#define VeciAndNot(a, b)       _mm_andnot_si128(a, b)  /* ~a  & b */
#define VeciSrl(a, b)          _mm_srlv_epi32(a, b)    /*  a >> b */
#define VeciSll(a, b)          _mm_sllv_epi32(a, b)    /*  a << b */
#define VeciSrl32(a, b)        _mm_srli_epi32(a, b)    /*  a >> b */
#define VeciSll32(a, b)        _mm_slli_epi32(a, b)    /*  a << b */
#define VeciToVecf(a)          _mm_castsi128_ps(a)     /*  a << b */

#define VeciCmpLt(a, b) _mm_cmplt_epi32(a, b)
#define VeciCmpLe(a, b) _mm_cmple_epi32(a, b)
#define VeciCmpGt(a, b) _mm_cmpgt_epi32(a, b)
#define VeciCmpGe(a, b) _mm_cmpge_epi32(a, b)

#define VeciUnpackLow(a, b)   _mm_unpacklo_epi32(a, b)  /*  [a.x, a.y, b.x, b.y] */
#define VeciUnpackLow16(a, b) _mm_unpacklo_epi16(a, b)  /*  [a.x, a.y, b.x, b.y] */
#define VeciBlend(a, b, c)    _mm_blendv_epi8(a, b, c)


#elif defined(AX_ARM)
/*//////////////////////////////////////////////////////////////////////////*/
/*                                 NEON                                     */
/*//////////////////////////////////////////////////////////////////////////*/

typedef float32x4_t Vector4x32f;
typedef uint32x4_t Vector4x32i;
typedef uint32x4_t Vector4x32u;

#define VecZero()           vdupq_n_f32(0.0f)
#define VecOne()            vdupq_n_f32(1.0f)
#define VecNegativeOne()    vdupq_n_f32(-1.0f)
#define VecSet1(x)          vdupq_n_f32(x)
#define VecSet(x, y, z, w)  ARMCreateVec(w, z, y, x) /* -> {w, z, y, x} */
#define VecSetR(x, y, z, w) ARMCreateVec(x, y, z, w) /* -> {x, y, z, w} */
#define VecLoad(x)          vld1q_f32(x)
#define VecLoadA(x)         vld1q_f32(x)
#define Vec3Load(x)         ARMVector3Load(x)

#define VecStore(ptr, x)        vst1q_f32(ptr, x)
#define VecStoreA(ptr, x)       vst1q_f32(ptr, x)
#define VecFromInt1(x)          vdupq_n_s32(x)
#define VecFromInt(x, y, z, w)  ARMCreateVecI(x, y, z, w)
#define VecToInt(x) vreinterpretq_u32_f32(x)
#define VecCvtF32U32(x) vcvtq_u32_f32(x)
#define VecCvtU32F32(x) vcvtq_f32_u32(x)

// Get Set
#define VecSplatX(v)  vdupq_lane_f32(vget_low_f32(v), 0)
#define VecSplatY(v)  vdupq_lane_f32(vget_low_f32(v), 1)
#define VecSplatZ(v)  vdupq_lane_f32(vget_high_f32(v), 0)
#define VecSplatW(v)  vdupq_lane_f32(vget_high_f32(v), 1)

#define VecGetX(v) vgetq_lane_f32(v, 0)
#define VecGetY(v) vgetq_lane_f32(v, 1)
#define VecGetZ(v) vgetq_lane_f32(v, 2)
#define VecGetW(v) vgetq_lane_f32(v, 3)

#define VecSetX(v, x) ((v) = vsetq_lane_f32(x, v, 0))
#define VecSetY(v, y) ((v) = vsetq_lane_f32(y, v, 1))
#define VecSetZ(v, z) ((v) = vsetq_lane_f32(z, v, 2))
#define VecSetW(v, w) ((v) = vsetq_lane_f32(w, v, 3))

// Arithmetic
#define VecAdd(a, b) vaddq_f32(a, b)
#define VecSub(a, b) vsubq_f32(a, b)
#define VecMul(a, b) vmulq_f32(a, b)
#define VecDiv(a, b) ARMVectorDevide(a, b)
                                          
#define VecAddf(a, b) vaddq_f32(a, vdupq_n_f32(b))
#define VecSubf(a, b) vsubq_f32(a, vdupq_n_f32(b))
#define VecMulf(a, b) vmulq_n_f32(a, b)
#define VecDivf(a, b) ARMVectorDevide(a, VecSet1(b))

// a * b[l] + c
#define VecFmaddLane(a, b, c, l) vfmaq_laneq_f32(c, a, b, l)
#define VecFmadd(a, b, c)  vfmaq_f32(c, a, b)
#define VecFmsub(a, b, c) vfmsq_f32(c, a, b)
#define VecHadd(a, b)    vpaddq_f32(a, b)
#define VecSqrt(a)       vsqrtq_f32(a)
#define VecRcp(a)        vrecpeq_f32(a)
#define VecNeg(a)        vnegq_f32(a)

// Vector Math
#define VecDot(a, b)  ARMVectorDot(a, b)
#define VecDotf(a, b) VecGetX(ARMVectorDot(a, b))
#define VecNorm(v)    ARMVectorNorm(v)
#define VecNormEst(v) ARMVectorNormEst(v)
#define VecLenf(v)    VecGetX(ARMVectorLength(v))
#define VecLen(v)     ARMVectorLength(v)

#define Vec3DotV(a, b)  ARMVector3Dot(a, b)
#define Vec3DotfV(a, b) VecGetX(ARMVector3Dot(a, b))
#define Vec3NormV(v)    ARMVector3Norm(v)
#define Vec3NormEstV(v) ARMVector3NormEst(v)
#define Vec3LenfV(v)    VecGetX(ARMVector3Length(v))
#define Vec3LenV(v)     ARMVector3Length(v)

// vec(0, 1, 2, 3) -> (vec[x], vec[y], vec[z], vec[w])
#define VecSwizzle(vec, x, y, z, w) ARMVectorSwizzle < x, y, z, w > (vec)

#define VecShuffle(vec1, vec2, x, y, z, w)  ARMVectorShuffle < x, y, z, w > (vec1, vec2)
#define VecShuffleR(vec1, vec2, x, y, z, w) ARMVectorShuffle < w, z, y, x > (vec1, vec2)

// special shuffle
#define VecShuffle_0101(vec1, vec2) vcombine_f32(vget_low_f32(vec1), vget_low_f32(vec2))
#define VecShuffle_2323(vec1, vec2) vcombine_f32(vget_high_f32(vec1), vget_high_f32(vec2))
#define VecRev(v) ARMVectorRev(v)

#define VecNot(a)     vreinterpretq_f32_u32(vmvnq_u32(vreinterpretq_u32_f32(a)))
#define VecAnd(a, b) vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(a), b))
#define VecOr(a, b)  vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a), b))
#define VecXor(a, b) vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(a), b))

#define VecMask(a, msk) VecSelect(vdupq_n_f32(0.0f), a, msk)

#define VecMax(a, b) vmaxq_f32(a, b)
#define VecMin(a, b) vminq_f32(a, b)
#define VecFloor(a)  vrndmq_f32(a)

#define VecCmpGt(a, b) vcgtq_f32(a, b) // greater or equal
#define VecCmpGe(a, b) vcgeq_f32(a, b) // greater or equal
#define VecCmpLt(a, b) vcltq_f32(a, b) // less than
#define VecCmpLe(a, b) vcleq_f32(a, b) // less or equal
#define VecMovemask(a) ARMVecMovemask(a) /* not done */

#define VecSelect(V1, V2, Control) vbslq_f32(Control, V2, V1)
#define VecBlend(a, b, Control)    vbslq_f32(Control, b, a)

//------------------------------------------------------------------------
// Veci
#define VeciZero()   vdupq_n_u32(0)
#define VeciSet1(x)  vdupq_n_u32(x)
#define VeciSetR(x, y, z, w)  ARMCreateVecI(x, y, z, w)
#define VeciSet(x, y, z, w)   ARMCreateVecI(w, z, y, x)
#define VeciLoadA(x)          vld1q_u32(x)
#define VeciLoad(x)           vld1q_u32(x)
#define VeciLoad64(qword)     vcombine_u32(vcreate_u32(qword), vcreate_u32(0ull)) /* loads 64bit integer to first 8 bytes of register */

#define VeciSetX(v, x) ((v) = vsetq_lane_u32(x, v, 0))
#define VeciSetY(v, y) ((v) = vsetq_lane_u32(y, v, 1))
#define VeciSetZ(v, z) ((v) = vsetq_lane_u32(z, v, 2))
#define VeciSetW(v, w) ((v) = vsetq_lane_u32(w, v, 3))

#define VeciAdd(a, b) vaddq_u32(a, b)
#define VeciSub(a, b) vsubq_u32(a, b)
#define VeciMul(a, b) vmulq_u32(a, b)

#define VecFromVeci(x) vreinterpretq_f32_u32(x)
#define VeciFromVec(x) vreinterpretq_u32_f32(x)
// Swizzling Masking
#define VecSelect1000 ARMCreateVecI(0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u)
#define VecSelect1100 ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u)
#define VecSelect1110 ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u)
#define VecSelect1011 ARMCreateVecI(0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu)
#define VecSelect1111 ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu)

#define VeciSelect1111 ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu)

#define VecIdentityR0 ARMCreateVec(1.0f, 0.0f, 0.0f, 0.0f)
#define VecIdentityR1 ARMCreateVec(0.0f, 1.0f, 0.0f, 0.0f)
#define VecIdentityR2 ARMCreateVec(0.0f, 0.0f, 1.0f, 0.0f)
#define VecIdentityR3 ARMCreateVec(0.0f, 0.0f, 0.0f, 1.0f)

#define VecMaskXY ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u)
#define VecMask3  ARMCreateVecI(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u)
#define VecMaskX  ARMCreateVecI(0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u)
#define VecMaskY  ARMCreateVecI(0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0x00000000u)
#define VecMaskZ  ARMCreateVecI(0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u)
#define VecMaskW  ARMCreateVecI(0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu)

// Logical
#define VeciNot(a)     vmvnq_u32(a)
#define VeciAnd(a, b)  vandq_u32(a, b)
#define VeciOr(a, b)   vorrq_u32(a, b)
#define VeciXor(a, b)  veorq_u32(a, b)

#define VeciAndNot(a, b) vandq_u32(vmvnq_u32(a), b)  /* ~a & b */
#define VeciSrl(a, b)    vshlq_u32(a, vnegq_s32(b))  /* a >> b */
#define VeciSll(a, b)    vshlq_u32(a, b)             /* a << b */
#define VeciSrl32(a, b)  vshrq_n_u32(a, b)           /* a >> b */
#define VeciSll32(a, b)  vshlq_n_u32(a, b)           /* a << b */
#define VeciToVecf(a)    vreinterpretq_f32_s32(a)    /* Reinterpret int as float */

#define VeciCmpLt(a, b) vcltq_u32(a, b)
#define VeciCmpLe(a, b) vclte_u32(a, b)
#define VeciCmpGt(a, b) vcgtq_u32(a, b)
#define VeciCmpGe(a, b) vcgeq_u32(a, b)

#define VeciUnpackLow(a, b)   vzip1q_s32(a, b)   /* [a.x, a.y, b.x, b.y] */
#define VeciUnpackLow16(a, b) vzip1q_s16(a, b)   /* [a.x, a.y, b.x, b.y] */
#define VeciBlend(a, b, c)    vbslq_u8(c, b, a)  /* Blend a and b based on mask c */

purefn Vector4x32f ARMVectorRev(Vector4x32f v)
{
    float32x4_t rev64 = vrev64q_f32(v);
    return vextq_f32(rev64, rev64, 2);
}

purefn Vector4x32f ARMVector3Load(float* src)
{
    return vcombine_f32(vld1_f32(src), vld1_lane_f32(src + 2, vdup_n_f32(0), 0));
}

purefn Vector4x32f ARMCreateVec(float x, float y, float z, float w) {
    AX_ALIGN(16) float v[4] = {x, y, z, w};
    return vld1q_f32(v);
}

purefn Vector4x32i ARMCreateVecI(uint x, uint y, uint z, uint w) {
    return vcombine_u32(vcreate_u32(((uint64_t)x) | (((uint64_t)y) << 32)),
                        vcreate_u32(((uint64_t)z) | (((uint64_t)w) << 32)));
}

purefn Vector4x32f ARMVector3NormEst(Vector4x32f v) {
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vpadd_f32(v1, v1);
    v2 = vdup_lane_f32(v2, 0);
    v1 = vadd_f32(v1, v2); // Dot3
    v2 = vrsqrte_f32(v1); // Reciprocal sqrt (estimate)
    return vmulq_f32(v, vcombine_f32(v2, v2)); // Normalize
}

purefn Vector4x32f ARMVector3Norm(Vector4x32f v) {
    // Dot3
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vpadd_f32(v1, v1);
    v2 = vdup_lane_f32(v2, 0);
    v1 = vadd_f32(v1, v2);
    uint32x2_t VEqualsZero = vceq_f32(v1, vdup_n_f32(0));
    // Reciprocal sqrt (2 iterations of Newton-Raphson)
    float32x2_t S0 = vrsqrte_f32(v1);
    float32x2_t P0 = vmul_f32(v1, S0);
    float32x2_t R0 = vrsqrts_f32(P0, S0);
    float32x2_t S1 = vmul_f32(S0, R0);
    float32x2_t R1 = vrsqrts_f32(vmul_f32(v1, S1), S1);
    v2 = vmul_f32(S1, R1);
    // Normalize
    Vector4x32f vResult = vmulq_f32(v, vcombine_f32(v2, v2));
    vResult = vbslq_f32(vcombine_u32(VEqualsZero, VEqualsZero), vdupq_n_f32(0), vResult);
    return vResult;
}

purefn Vector4x32f ARMVector3Dot(Vector4x32f a, Vector4x32f b) {
    float32x4_t vTemp = vmulq_f32(a, b);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vpadd_f32(v1, v1);
    v2 = vdup_lane_f32(v2, 0);
    v1 = vadd_f32(v1, v2);
    return vcombine_f32(v1, v1);
}

purefn Vector4x32f ARMVector3Length(Vector4x32f v) {
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vpadd_f32(v1, v1);
    v2 = vdup_lane_f32(v2, 0);
    v1 = vadd_f32(v1, v2);
    const float32x2_t zero = vdup_n_f32(0);
    uint32x2_t VEqualsZero = vceq_f32(v1, zero);
    float32x2_t Result = vrsqrte_f32(v1);
    Result = vmul_f32(v1, Result);
    Result = vbsl_f32(VEqualsZero, zero, Result);
    return vcombine_f32(Result, Result);
}

purefn Vector4x32f ARMVectorLengthEst(Vector4x32f v) {
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vadd_f32(v1, v2);
    v1 = vpadd_f32(v1, v1);
    const float32x2_t zero = vdup_n_f32(0);
    uint32x2_t VEqualsZero = vceq_f32(v1, zero);
    float32x2_t Result = vrsqrte_f32(v1);
    Result = vmul_f32(v1, Result);
    Result = vbsl_f32(VEqualsZero, zero, Result);
    return vcombine_f32(Result, Result);
}

purefn Vector4x32f ARMVectorLength(Vector4x32f v) {
	// Dot4
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vadd_f32(v1, v2);
    v1 = vpadd_f32(v1, v1);
    const float32x2_t zero = vdup_n_f32(0);
    uint32x2_t VEqualsZero = vceq_f32(v1, zero);
    // Sqrt
    float32x2_t S0 = vrsqrte_f32(v1);
    float32x2_t P0 = vmul_f32(v1, S0);
    float32x2_t R0 = vrsqrts_f32(P0, S0);
    float32x2_t S1 = vmul_f32(S0, R0);
    float32x2_t P1 = vmul_f32(v1, S1);
    float32x2_t R1 = vrsqrts_f32(P1, S1);
    float32x2_t Result = vmul_f32(S1, R1);
    Result = vmul_f32(v1, Result);
    Result = vbsl_f32(VEqualsZero, zero, Result);
    return vcombine_f32(Result, Result);
}

purefn Vector4x32f ARMVectorDevide(Vector4x32f V1, Vector4x32f V2) {
    // 2 iterations of Newton-Raphson refinement of reciprocal
    float32x4_t Reciprocal = vrecpeq_f32(V2);
    float32x4_t S = vrecpsq_f32(Reciprocal, V2);
    Reciprocal = vmulq_f32(S, Reciprocal);
    S = vrecpsq_f32(Reciprocal, V2);
    Reciprocal = vmulq_f32(S, Reciprocal);
    return vmulq_f32(V1, Reciprocal);
}

purefn Vector4x32f ARMVectorDot(Vector4x32f a, Vector4x32f b) {
    float32x4_t vTemp = vmulq_f32(a, b);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vadd_f32(v1, v2);
    v1 = vpadd_f32(v1, v1);
    return vcombine_f32(v1, v1);
}

purefn Vector4x32f ARMVectorNormEst(Vector4x32f v) {
    float32x4_t vTemp = vmulq_f32(v, v);
    float32x2_t v1 = vget_low_f32(vTemp);
    float32x2_t v2 = vget_high_f32(vTemp);
    v1 = vadd_f32(v1, v2);
    v1 = vpadd_f32(v1, v1);
    v2 = vrsqrte_f32(v1);
    return vmulq_f32(v, vcombine_f32(v2, v2));
}

purefn Vector4x32f ARMVectorNorm(Vector4x32f v) 
{
    return ARMVectorDevide(v, ARMVectorLength(v));
}

purefn int ARMVecMovemask(Vector4x32i v) {
    const int shiftArr[4] = { 0, 1, 2, 3 };
    int32x4_t shift = vld1q_s32(shiftArr);
    return vaddvq_u32(vshlq_u32(vshrq_n_u32(v, 31), shift));
}

template<int E0, int E1, int E2, int E3>
purefn Vector4x32f ARMVectorSwizzle(Vector4x32f v)
{
    float a = vgetq_lane_f32(v, E0); 
    float b = vgetq_lane_f32(v, E1); 
    float c = vgetq_lane_f32(v, E2); 
    float d = vgetq_lane_f32(v, E3); 
    return VecSetR(a, b, c, d); 
}

template<int E0, int E1, int E2, int E3>
purefn Vector4x32f ARMVectorShuffle(Vector4x32f v0, Vector4x32f v1)
{
    float a = vgetq_lane_f32(v0, E0);
    float b = vgetq_lane_f32(v0, E1);
    float c = vgetq_lane_f32(v1, E2);
    float d = vgetq_lane_f32(v1, E3);
    return VecSetR(a, b, c, d);
}
#endif


#if defined(AX_SUPPORT_SSE) || defined(AX_ARM)
// very ugly :(
purefn float VECTORCALL VecGetN(Vector4x32f v, int n)
{
    switch (n)
    {
        case 0: return VecGetX(v);
        case 1: return VecGetY(v);
        case 2: return VecGetZ(v);
        case 3: return VecGetW(v);
    };
    AX_UNREACHABLE();
    return 1.0f;
}

forceinline Vector4x32f VECTORCALL VecSetN(Vector4x32f v, int n, float f)
{
    switch (n)
    {
        case 0: VecSetX(v, f); break;
        case 1: VecSetY(v, f); break;
        case 2: VecSetZ(v, f); break;
        case 3: VecSetW(v, f); break;
    };
    return v;
}
#endif 

purefn Vector4x32f VECTORCALL Vec3CrossV(Vector4x32f vec0, Vector4x32f vec1)
{
    #if defined(AX_ARM)
    float32x2_t v1xy = vget_low_f32(vec0);
    float32x2_t v2xy = vget_low_f32(vec1);
    float32x2_t v1yx = vrev64_f32(v1xy);
    float32x2_t v2yx = vrev64_f32(v2xy);
    float32x2_t v1zz = vdup_lane_f32(vget_high_f32(vec0), 0);
    float32x2_t v2zz = vdup_lane_f32(vget_high_f32(vec1), 0);
    uint32x4_t FlipY = ARMCreateVecI(0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u);
    Vector4x32f vResult = vmulq_f32(vcombine_f32(v1yx, v1xy), vcombine_f32(v2zz, v2yx));
    vResult = vmlsq_f32(vResult, vcombine_f32(v1zz, v1yx), vcombine_f32(v2yx, v2xy));
    vResult = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(vResult), FlipY));
    return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vResult), VecMask3));
    #else
    Vector4x32f tmp0 = VecShuffleR(vec0, vec0, 3,0,2,1);
    Vector4x32f tmp1 = VecShuffleR(vec1, vec1, 3,1,0,2);
    Vector4x32f tmp2 = VecMul(tmp0, vec1);
    Vector4x32f tmp3 = VecMul(tmp0, tmp1);
    Vector4x32f tmp4 = VecShuffleR(tmp2, tmp2, 3,0,2,1);
    return VecSub(tmp3, tmp4);
    #endif
}

purefn float VECTORCALL Min3(Vector4x32f ab)
{
    Vector4x32f xy = VecMin(VecSplatX(ab), VecSplatY(ab));
    return VecGetX(VecMin(xy, VecSplatZ(ab)));
}

purefn float VECTORCALL Max3(Vector4x32f ab)
{
    Vector4x32f xy = VecMax(VecSplatX(ab), VecSplatY(ab));
    return VecGetX(VecMax(xy, VecSplatZ(ab)));
}

#define VecClamp01(v) VecClamp(v, VecZero(), VecOne())

purefn Vector4x32f VECTORCALL VecClamp(Vector4x32f v, Vector4x32f vmin, Vector4x32f vmax)
{
    v = VecSelect(v, vmax, VecCmpGt(v, vmax));
    v = VecSelect(v, vmin, VecCmpLt(v, vmin));
    return v;
}

forceinline void VECTORCALL Vec3Store(float* f, Vector4x32f v)
{
    f[0] = VecGetX(v);
    f[1] = VecGetY(v);
    f[2] = VecGetZ(v);
}


purefn Vector4x32f VECTORCALL VecHSum(Vector4x32f v) {
    v = VecHadd(v, v); // high half -> low half
    return VecHadd(v, v);
}

#if defined(AX_ARM)
#define VecFabs(x) vabsq_f32(x)
#elif defined(AX_SUPPORT_SSE)
#define VecFabs(x) VecAnd(x, VecFromInt1(0x7fffffff))
#else
#define VecFabs(v) MakeVec4(Abs(v.x), Abs(v.y), Abs(v.z), Abs(v.w))
#endif

purefn Vector4x32f VECTORCALL VecCopySign(Vector4x32f x, Vector4x32f y)
{
    Vector4x32u clearedX = VeciAnd(VeciFromVec(x), VeciSet1(0x7fffffff));
    Vector4x32u signY    = VeciAnd(VeciFromVec(y), VeciSet1(0x80000000));
    Vector4x32u res      = VeciOr(clearedX, signY);
    return VecFromVeci(res);
}

purefn Vector4x32f VECTORCALL VecLerp(Vector4x32f x, Vector4x32f y, float t)
{
    return VecFmadd(VecSub(y, x), VecSet1(t), x);
}

purefn Vector4x32f VECTORCALL VecStep(Vector4x32f edge, Vector4x32f x)
{
    return VecBlend(VecZero(), VecOne(), VecCmpGt(x, edge));
}

purefn Vector4x32f VECTORCALL VecFract(Vector4x32f x)
{
    return VecSub(x, VecFloor(x));
}

inline Vector4x32f VECTORCALL VecSin(Vector4x32f x)
{ 
    Vector4x32i lz, gtpi; 
    Vector4x32f vpi = VecSet1(MATH_PI);
    lz = VecCmpLt(x, VecZero());
    x  = VecFabs(x);
    gtpi = VecCmpGt(x, vpi);
    
    x = VecSelect(x, VecSub(x, vpi), gtpi);
    x = VecMul(x, VecSet1(0.63655f));
    x = VecMul(x, VecSub(VecSet1(2.0f), x));
    x = VecFmadd(x, VecSet1(0.225f), VecSet1(0.775f));
    
    x = VecSelect(x, VecNeg(x), gtpi);
    x = VecSelect(x, VecNeg(x), lz);
    return x;
}

inline Vector4x32f VECTORCALL VecCos(Vector4x32f x)
{
    Vector4x32i lz, gtpi;
    Vector4x32f a;
    Vector4x32f vpi = VecSet1(MATH_PI);
    lz = VecCmpLt(VecZero(), x);
    x = VecFabs(x);
    gtpi = VecCmpGt(x, vpi);
    x = VecSelect(x, VecSub(x, vpi), gtpi);
    x = VecMul(x, VecSet1(0.159f));
    a = VecMul(VecMul(VecSet1(32.0f), x), x);
    x = VecSub(VecSet1(1.0f), VecMul(a, VecSub(VecSet1(0.75f), x)));
    return VecSelect(x, VecNeg(x), gtpi);
}

purefn Vector4x32f VECTORCALL VecAtan(Vector4x32f x)
{
    const float sa1 =  0.99997726f, sa3 = -0.33262347f, sa5  = 0.19354346f,
    sa7 = -0.11643287f, sa9 =  0.05265332f, sa11 = -0.01172120f;
      
    const Vector4x32f xx = VecMul(x, x);
    // (a9 + x_sq * a11
    Vector4x32f res = VecSet1(sa11); 
    res = VecFmadd(xx, res, VecSet1(sa9));
    res = VecFmadd(xx, res, VecSet1(sa7));
    res = VecFmadd(xx, res, VecSet1(sa5));
    res = VecFmadd(xx, res, VecSet1(sa3));
    res = VecFmadd(xx, res, VecSet1(sa1));
    return VecMul(x, res);
}

inline Vector4x32f VECTORCALL VecAtan2(Vector4x32f y, Vector4x32f x)
{
    Vector4x32f ay = VecFabs(y), ax = VecFabs(x);
    Vector4x32i swapMask = VecCmpGt(ay, ax);
    Vector4x32f z  = VecDiv(VecBlend(ay, ax, swapMask), VecBlend(ax, ay, swapMask));
    Vector4x32f th = VecAtan(z);
    th = VecSelect(th, VecSub(VecSet1(MATH_HalfPI), th), swapMask);
    th = VecSelect(th, VecSub(VecSet1(MATH_PI), th), VecCmpLt(x, VecZero()));
    return VecCopySign(th, y);
}

purefn Vector4x32f VECTORCALL VecSinCos(Vector4x32f* cv, Vector4x32f x)
{
    Vector4x32f s = VecSin(x);
    *cv = VecCos(x);
    return s;
}

purefn float VECTORCALL Min3v(Vector4x32f ab)
{
    Vector4x32f xy = VecMin(VecSplatX(ab), VecSplatY(ab));
    return VecGetX(VecMin(xy, VecSplatZ(ab)));
}

purefn float VECTORCALL Max3v(Vector4x32f ab)
{
    Vector4x32f xy = VecMax(VecSplatX(ab), VecSplatY(ab));
    return VecGetX(VecMax(xy, VecSplatZ(ab)));
}

purefn bool IsPointInsideAABB(Vector4x32f point, Vector4x32f aabbMin, Vector4x32f aabbMax)
{
    Vector4x32i cmpMin = VecCmpGe(point, aabbMin);
    Vector4x32i cmpMax = VecCmpLe(point, aabbMax);
    #if defined(AX_ARM)
    uint32_t movemask = VecMovemask(VeciAnd(cmpMin, cmpMax));
    #else
    uint32_t movemask = VecMovemask(VecAnd(cmpMin, cmpMax));
    #endif
    return (movemask & 0b111) == 0b111;
}

purefn float VECTORCALL IntersectAABB(Vector4x32f origin, Vector4x32f invDir, Vector4x32f aabbMin, Vector4x32f aabbMax, float minSoFar)
{
    if (IsPointInsideAABB(origin, aabbMin, aabbMax)) return 0.1f;
    Vector4x32f tmin = VecMul(VecSub(aabbMin, origin), invDir);
    Vector4x32f tmax = VecMul(VecSub(aabbMax, origin), invDir);
    float tnear = Max3v(VecMin(tmin, tmax));
    float tfar  = Min3v(VecMax(tmin, tmax));
    // return tnear < tfar && tnear > 0.0f && tnear < minSoFar;
    if (tnear < tfar && tnear > 0.0f && tnear < minSoFar)
        return tnear; else return 1e30f;
}

// calculate popcount of 4 32 bit integer concurrently
purefn Vector4x32u VECTORCALL PopCount32_128(Vector4x32u x)
{
    Vector4x32u y;
    y = VeciAnd(VeciSrl32(x, 1), VeciSet1(0x55555555));
    x = VeciSub(x, y);
    y = VeciAnd(VeciSrl32(x, 2), VeciSet1(0x33333333));
    x = VeciAdd(VeciAnd(x, VeciSet1(0x33333333)), y);  
    x = VeciAnd(VeciAdd(x, VeciSrl32(x, 4)), VeciSet1(0x0F0F0F0F));
    return VeciSrl32(VeciMul(x, VeciSet1(0x01010101)),  24);
}

// LeadingZeroCount of 4 32 bit integer concurrently
purefn Vector4x32u VECTORCALL LeadingZeroCount32_128(Vector4x32u x)
{
    x = VeciOr(x, VeciSrl32(x, 1));
    x = VeciOr(x, VeciSrl32(x, 2));
    x = VeciOr(x, VeciSrl32(x, 4));
    x = VeciOr(x, VeciSrl32(x, 8));
    x = VeciOr(x, VeciSrl32(x, 16)); 
    return VeciSub(VeciSet1(sizeof(uint32_t) * 8), PopCount32_128(x));
}   

typedef struct RayV_
{
    Vector4x32f origin;
    Vector4x32f direction;
} RayV;

inline RayV MakeRayV(Vector4x32f o, Vector4x32f d)
{
    return (RayV){ o, d };
}


#endif // simd.h