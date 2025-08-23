#ifndef Vector_H
#define Vector_H

#include "Math.h"
#include "Common.h"

typedef struct Vec3f_ {
    float x, y, z;
} Vec3f;

typedef struct Vec3i_ {
    int x, y, z;
} Vec3i;

typedef struct Vec2f_ {
    float x, y;
} Vec2f;

typedef struct Vec2i_ {
    int x, y;
} Vec2i;

typedef struct Ray_ {
    Vec3f origin, dir;
} Ray;


// VECTOR32
purefn Vec3f Vec3Add(Vec3f a, Vec3f b) { return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z}; }
purefn Vec3f Vec3Mul(Vec3f a, Vec3f b) { return (Vec3f){a.x * b.x, a.y * b.y, a.z * b.z}; }
purefn Vec3f Vec3Div(Vec3f a, Vec3f b) { return (Vec3f){a.x / b.x, a.y / b.y, a.z / b.z}; }
purefn Vec3f Vec3Sub(Vec3f a, Vec3f b) { return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z}; }
purefn Vec3f Vec3Neg(Vec3f a) { return (Vec3f){-a.x, -a.y, -a.z}; }

purefn Vec3f Vec3AddF(Vec3f a, float b) { return (Vec3f){a.x + b, a.y + b, a.z + b}; }
purefn Vec3f Vec3MulF(Vec3f a, float b) { return (Vec3f){a.x * b, a.y * b, a.z * b}; }
purefn Vec3f Vec3DivF(Vec3f a, float b) { return (Vec3f){a.x / b, a.y / b, a.z / b}; }
purefn Vec3f Vec3SubF(Vec3f a, float b) { return (Vec3f){a.x - b, a.y - b, a.z - b}; }

purefn float Vec3Len(Vec3f a) { return Sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
purefn float Vec3Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

purefn Vec3f Vec3Norm(Vec3f a)    { return Vec3DivF(a, Sqrtf(a.x * a.x + a.y * a.y + a.z * a.z)); }
purefn Vec3f Vec3NormSafe(Vec3f a){ return Vec3DivF(a, MATH_Epsilon + Sqrtf(a.x * a.x + a.y * a.y + a.z * a.z)); }
purefn Vec3f Vec3NormEst(Vec3f a) { return Vec3MulF(a, RSqrtf(a.x * a.x + a.y * a.y + a.z * a.z)); }

purefn float Vec3Dist(Vec3f a, Vec3f b) {
    Vec3f diff = Vec3Sub(a, b);
    return Sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z); 
}

purefn float Vec3DistSqr(Vec3f a, Vec3f b) {
    Vec3f diff = Vec3Sub(a, b);
    return (diff.x * diff.x + diff.y * diff.y + diff.z * diff.z); 
}

purefn float Vec3AngleBetween(Vec3f a, Vec3f b) {
    float dot = Vec3Dot(a, b) * RSqrtf(Vec3Dot(a, a) * Vec3Dot(b, b));
    dot = MCLAMP(dot, -1.0f, 1.0f);
    return ACos(dot);
}

purefn Vec3f Vec3Reflect(Vec3f in, Vec3f normal) {
    return Vec3Sub(in, Vec3MulF(normal, Vec3Dot(normal, in) * 2.0f));
}

purefn Vec3f Vec3Lerp(Vec3f a, Vec3f b, float t) {
    return (Vec3f) {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

purefn Vec3f Vec3Cross(Vec3f a, Vec3f b) {
    return (Vec3f) {
        a.y * b.z - b.y * a.z,
        a.z * b.x - b.z * a.x,
        a.x * b.y - b.x * a.y
    };
}

purefn Vec3f Vec3Zero()     { return (Vec3f){ 0.0f,  0.0f,  0.0f}; }
purefn Vec3f Vec3One()      { return (Vec3f){ 1.0f,  1.0f,  1.0f}; }
purefn Vec3f Vec3Up()       { return (Vec3f){ 0.0f,  1.0f,  0.0f}; }
purefn Vec3f Vec3Left()     { return (Vec3f){-1.0f,  0.0f,  0.0f}; }
purefn Vec3f Vec3Down()     { return (Vec3f){ 0.0f, -1.0f,  0.0f}; }
purefn Vec3f Vec3Right()    { return (Vec3f){ 1.0f,  0.0f,  0.0f}; }
purefn Vec3f Vec3Forward()  { return (Vec3f){ 0.0f,  0.0f,  1.0f}; }
purefn Vec3f Vec3Backward() { return (Vec3f){ 0.0f,  0.0f, -1.0f}; }

// VECTOR3I
purefn Vec3i Vec3iAdd(Vec3i a, Vec3i b) { return (Vec3i){a.x + b.x, a.y + b.y, a.z + b.z}; }
purefn Vec3i Vec3iMul(Vec3i a, Vec3i b) { return (Vec3i){a.x * b.x, a.y * b.y, a.z * b.z}; }
purefn Vec3i Vec3iDiv(Vec3i a, Vec3i b) { return (Vec3i){a.x / b.x, a.y / b.y, a.z / b.z}; }
purefn Vec3i Vec3iSub(Vec3i a, Vec3i b) { return (Vec3i){a.x - b.x, a.y - b.y, a.z - b.z}; }
purefn Vec3i Vec3iNeg(Vec3i a) { return (Vec3i){-a.x, -a.y, -a.z}; }

purefn Vec3i Vec3iAddI(Vec3i a, int b) { return (Vec3i){a.x + b, a.y + b, a.z + b}; }
purefn Vec3i Vec3iMulI(Vec3i a, int b) { return (Vec3i){a.x * b, a.y * b, a.z * b}; }
purefn Vec3i Vec3iDivI(Vec3i a, int b) { return (Vec3i){a.x / b, a.y / b, a.z / b}; }
purefn Vec3i Vec3iSubI(Vec3i a, int b) { return (Vec3i){a.x - b, a.y - b, a.z - b}; }

// VECTOR2I
purefn Vec2i Vec2iAdd(Vec2i a, Vec2i b) { return (Vec2i){a.x + b.x, a.y + b.y}; }
purefn Vec2i Vec2iMul(Vec2i a, Vec2i b) { return (Vec2i){a.x * b.x, a.y * b.y}; }
purefn Vec2i Vec2iDiv(Vec2i a, Vec2i b) { return (Vec2i){a.x / b.x, a.y / b.y}; }
purefn Vec2i Vec2iSub(Vec2i a, Vec2i b) { return (Vec2i){a.x - b.x, a.y - b.y}; }
purefn Vec2i Vec2iNeg(Vec2i a) { return (Vec2i){-a.x, -a.y}; }

purefn Vec2i Vec2iAddI(Vec2i a, int b) { return (Vec2i){a.x + b, a.y + b}; }
purefn Vec2i Vec2iMulI(Vec2i a, int b) { return (Vec2i){a.x * b, a.y * b}; }
purefn Vec2i Vec2iDivI(Vec2i a, int b) { return (Vec2i){a.x / b, a.y / b}; }
purefn Vec2i Vec2iSubI(Vec2i a, int b) { return (Vec2i){a.x - b, a.y - b}; }

purefn float Vec2iLen(Vec2i a) { return Sqrtf(a.x * a.x + a.y * a.y); }
purefn float Vec2iLenSqr(Vec2i a) { return (a.x * a.x + a.y * a.y); }
purefn float Vec2iDist(Vec2i a, Vec2i b) { return Vec2iLen(Vec2iSub(a, b)); }

// Vec2f
purefn Vec2f Vec2Add(Vec2f a, Vec2f b) { return (Vec2f){a.x + b.x, a.y + b.y}; }
purefn Vec2f Vec2Mul(Vec2f a, Vec2f b) { return (Vec2f){a.x * b.x, a.y * b.y}; }
purefn Vec2f Vec2Div(Vec2f a, Vec2f b) { return (Vec2f){a.x / b.x, a.y / b.y}; }
purefn Vec2f Vec2Sub(Vec2f a, Vec2f b) { return (Vec2f){a.x - b.x, a.y - b.y}; }
purefn Vec2f Vec2Neg(Vec2f a) { return (Vec2f){-a.x, -a.y}; }

purefn Vec2f Vec2AddF(Vec2f a, float b) { return (Vec2f){a.x + b, a.y + b}; }
purefn Vec2f Vec2MulF(Vec2f a, float b) { return (Vec2f){a.x * b, a.y * b}; }
purefn Vec2f Vec2DivF(Vec2f a, float b) { return (Vec2f){a.x / b, a.y / b}; }
purefn Vec2f Vec2SubF(Vec2f a, float b) { return (Vec2f){a.x - b, a.y - b}; }

purefn float Vec2Len(Vec2f a) { return Sqrtf(a.x * a.x + a.y * a.y); }
purefn float Vec2LenSqr(Vec2f a) { return (a.x * a.x + a.y * a.y); }
purefn float Vec2Dist(Vec2f a, Vec2f b) { return Vec2Len(Vec2Sub(a, b)); }

purefn Vec2f Lerp(Vec2f a, Vec2f b, float t) {
    return (Vec2f) { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

purefn Vec2f Vec2Rotate(Vec2f vec, float angle) {
    float s = Sin(angle), c = Cos(angle);
    return (Vec2f){vec.x * c - vec.y * s, vec.x * s + vec.y * c};
}

purefn Vec2f ToVec2f(Vec2i vec)    { return (Vec2f){(float)vec.x, (float)vec.y }; }
purefn Vec2i ToVector2i(Vec2f vec) { return (Vec2i){  (int)vec.x, (int)vec.y   }; }

purefn bool PointBoxIntersection(Vec2f min, Vec2f max, Vec2f point)
{
    return point.x <= max.x && point.y <= max.y &&
           point.x >= min.x && point.y >= min.y;
}

purefn bool RectPointIntersect(Vec2f min, Vec2f scale, Vec2f point)
{
    Vec2f max = { min.x + scale.x, min.y + scale.y };
    return point.x <= max.x && point.y <= max.y &&
           point.x >= min.x && point.y >= min.y;
}

purefn uint32_t GreaterThan2(Vec2f a, Vec2f b)
{
    return (uint32_t)(a.x > b.x) | ((uint32_t)(a.y > b.y) << 1);
}

purefn uint32_t LessThan2(Vec2f a, Vec2f b)
{
    return (uint32_t)(a.x < b.x) | ((uint32_t)(a.y < b.y) << 1);
}

purefn uint32_t GreaterThan3(Vec3f a, Vec3f b)
{
    return (uint32_t)(a.x > b.x) | ((uint32_t)(a.y > b.y) << 1) | ((uint32_t)(a.y > b.y) << 2);
}

purefn uint32_t LessThan3(Vec3f a, Vec3f b)
{
    return (uint32_t)(a.x < b.x) | ((uint32_t)(a.y < b.y) << 1) | ((uint32_t)(a.y < b.y) << 2);
}

purefn uint32_t All2(uint32_t msk) { return msk == 0b11u; }
purefn uint32_t All3(uint32_t msk) { return msk == 0b111u; }

purefn uint32_t Any2(uint32_t msk) { return msk > 0; }
purefn uint32_t Any3(uint32_t msk) { return msk > 0; }

#endif //Vector.h