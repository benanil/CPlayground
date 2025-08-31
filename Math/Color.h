#ifndef MATH_COLOR
#define MATH_COLOR

#include "Vector.h"
#include "SIMD.h"

purefn uint32_t PackColorToUint(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return r | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24);
}

purefn uint32_t MakeRGBAGrayScale(uint8_t gray) {
    return (uint32_t)(gray) * 0x01010101u;
}

purefn uint32_t PackColorToUint3Float(float r, float g, float b) {
    return (uint32_t)(r * 255.0f) | ((uint32_t)(g * 255.0f) << 8) | ((uint32_t)(b * 255.0f) << 16);
}

purefn uint32_t PackColor3PtrToUint(float* c) {
    return (uint32_t)(*c * 255.0f) | ((uint32_t)(c[1] * 255.0f) << 8) | ((uint32_t)(c[2] * 255.0f) << 16);
}

purefn uint32_t PackColor4PtrToUint(float* c) {
    return (uint32_t)(*c * 255.0f) | ((uint32_t)(c[1] * 255.0f) << 8) | ((uint32_t)(c[2] * 255.0f) << 16) | ((uint32_t)(c[3] * 255.0f) << 24);
}

forceinline void UnpackColor3Uint(unsigned color, float* colorf) {
    const float toFloat = 1.0f / 255.0f;
    colorf[0] = (float)(color >> 0  & 0xFF) * toFloat;
    colorf[1] = (float)(color >> 8  & 0xFF) * toFloat;
    colorf[2] = (float)(color >> 16 & 0xFF) * toFloat;
}

forceinline void UnpackColor4Uint(unsigned color, float* colorf) {
    const float toFloat = 1.0f / 255.0f;
    colorf[0] = (float)(color >> 0  & 0xFF) * toFloat;
    colorf[1] = (float)(color >> 8  & 0xFF) * toFloat;
    colorf[2] = (float)(color >> 16 & 0xFF) * toFloat;
    colorf[3] = (float)(color >> 24) * toFloat;
}

purefn uint32_t MultiplyU32Colors(uint32_t a, uint32_t b)
{
    uint32_t result = 0u;
    result |= ((a & 0xffu) * (b & 0xffu)) >> 8u;
    result |= ((((a >> 8u) & 0xffu) * ((b >> 8u) & 0xffu)) >> 8u) << 8u;
    result |= ((((a >> 16u) & 0xffu) * ((b >> 16u) & 0xffu)) >> 8u) << 16u;
    return result;
}

purefn Vec3f HUEToRGB(float h) {
    float r = MCLAMP01(Absf(h * 6.0f - 3.0f) - 1.0f);
    float g = MCLAMP01(2.0f - Absf(h * 6.0f - 2.0f));
    float b = MCLAMP01(2.0f - Absf(h * 6.0f - 4.0f));
    return (Vec3f){ r, g, b };
}

// converts hue to rgb color
purefn uint32_t HUEToRGBU32(float h) {
    Vec3f v3 = HUEToRGB(h);
    uint32_t res = PackColor3PtrToUint(&v3.x);
    return res | 0xFF000000u; // make the alpha 255
}

purefn Vec3f RGBToHSV(Vec3f rgb)
{
    float r = rgb.x, g = rgb.y, b = rgb.z;
    float K = 0.0f;
    if (g < b) {
        float t = g;
        g = b;
        b = t;
        // Swap(g, b);
        K = -1.0f;
    }

    if (r < g) {
        float t = g;
        g = r;
        r = t;
        //Swap(r, g);
        K = -2.0f / 6.0f - K;
    }
    const float chroma = r - (g < b ? g : b);
    return (Vec3f){
        Absf(K + (g - b) / (6.0f * chroma + 1e-20f)),
        chroma / (r + 1e-20f),
        r
    };
}

forceinline void HSVToRGB(Vec3f hsv, float* dst)
{
    const Vector4x32f K = VecSetR(1.0f, 2.0f / 3.0f, 1.0f / 3.0f, 3.0f);
    Vector4x32f p = VecFabs(VecSub(VecMul(VecFract(VecAdd(VecSet1(hsv.x), K)), VecSet1(6.0f)), VecSet1(3.0f)));
    Vector4x32f kx = VecSplatX(K);
    Vector4x32f rv = VecMul(VecLerp(kx, VecClamp01(VecSub(p, kx)), hsv.y), VecSet1(hsv.z));
    Vec3Store(dst, rv);
}

#endif // MATH_COLOR