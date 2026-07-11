#ifndef ARRAY_INCLUDE_ARRAY
#define ARRAY_INCLUDE_ARRAY

#include "../Common.h"
#include "../Memory.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum ArrayField_
{
    ArrayField_Capacity,
    ArrayField_Length,
    ArrayField_Stride,
    ArrayField_Count
} ArrayField;

#define ARRAY_DEFAULT_CAPACITY 1u
#define ARRAY_GROWTH_FACTOR    2u

void*  ArrayCreateRaw(size_t capacity, size_t stride);
void   ArrayDestroyRaw(void* arr);
size_t ArrayFieldGet(const void* arr, ArrayField field);
void   ArrayFieldSet(void* arr, ArrayField field, size_t value);
void*  ArrayResizeRaw(void* arr);
void*  ArrayPushRaw(void* arr, const void* value);
void   ArrayPopRaw(void* arr, void* out);

#define ArrayDef(type) type* /* not required */
#define ArrayCreate(type)                    ((type*)ArrayCreateRaw(ARRAY_DEFAULT_CAPACITY, sizeof(type)))
#define ArrayCreatePrealloc(type, capacity)  ((type*)ArrayCreateRaw((capacity), sizeof(type)))
#define ArrayDestroy(arr)                    ArrayDestroyRaw(arr)
#define ArrayPush(arr, value)                ((arr) = ArrayPushRaw((arr), &(value)))
#define ArrayPop(arr, out)                   ArrayPopRaw((arr), (out))
#define ArrayCapacity(arr)                   ArrayFieldGet((arr), ArrayField_Capacity)
#define ArrayLength(arr)                     ArrayFieldGet((arr), ArrayField_Length)
#define ArrayStride(arr)                     ArrayFieldGet((arr), ArrayField_Stride)

#if defined(__cplusplus)
}
#endif

#endif // ARRAY_INCLUDE_ARRAY
