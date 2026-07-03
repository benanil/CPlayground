#include "Include/DataStructures/Array.h"
#include "Include/Platform.h"

static size_t* ArrayHeader(const void* arr)
{
    return (size_t*)arr - ArrayField_Count;
}

void* ArrayCreateRaw(size_t capacity, size_t stride)
{
    if (capacity == 0)
        capacity = ARRAY_DEFAULT_CAPACITY;

    size_t headerSize = ArrayField_Count * sizeof(size_t);
    size_t dataSize   = capacity * stride;
    size_t* arr       = (size_t*)AllocateTLSFGlobal(headerSize + dataSize);
    if (!arr)
    {
        AX_WARN("array allocation failed (%llu bytes)", (u64)(headerSize + dataSize));
        return NULL;
    }

    arr[ArrayField_Capacity] = capacity;
    arr[ArrayField_Length]   = 0;
    arr[ArrayField_Stride]   = stride;
    return (void*)(arr + ArrayField_Count);
}

void ArrayDestroyRaw(void* arr)
{
    if (!arr)
        return;

    DeAllocateTLSFGlobal(ArrayHeader(arr));
}

size_t ArrayFieldGet(const void* arr, ArrayField field)
{
    if (!arr)
        return 0;

    return ArrayHeader(arr)[field];
}

void ArrayFieldSet(void* arr, ArrayField field, size_t value)
{
    if (!arr)
        return;

    ArrayHeader(arr)[field] = value;
}

void* ArrayResizeRaw(void* arr)
{
    if (!arr)
        return NULL;

    size_t capacity  = ArrayCapacity(arr);
    size_t stride    = ArrayStride(arr);
    size_t newCap    = capacity ? capacity * ARRAY_GROWTH_FACTOR : ARRAY_DEFAULT_CAPACITY;
    size_t totalSize = ArrayField_Count * sizeof(size_t) + newCap * stride;
    size_t* resized  = (size_t*)ReAllocateTLSFGlobal(ArrayHeader(arr), totalSize);
    if (!resized)
    {
        AX_WARN("array resize failed (%llu bytes)", (u64)totalSize);
        return arr;
    }

    resized[ArrayField_Capacity] = newCap;
    return (void*)(resized + ArrayField_Count);
}

void* ArrayPushRaw(void* arr, const void* value)
{
    if (!arr || !value)
        return arr;

    size_t length = ArrayLength(arr);
    if (length >= ArrayCapacity(arr))
    {
        arr = ArrayResizeRaw(arr);
        if (length >= ArrayCapacity(arr))
        {
            AX_WARN("array push failed, capacity exhausted");
            return arr;
        }
    }

    size_t stride = ArrayStride(arr);
    MemCopy((char*)arr + length * stride, value, stride);
    ArrayFieldSet(arr, ArrayField_Length, length + 1);
    return arr;
}

void ArrayPopRaw(void* arr, void* out)
{
    if (!arr || !out || ArrayLength(arr) == 0)
        return;

    size_t stride = ArrayStride(arr);
    size_t idx    = ArrayLength(arr) - 1;
    MemCopy(out, (char*)arr + idx * stride, stride);
    ArrayFieldSet(arr, ArrayField_Length, idx);
}
