#include "Source/Terrain/TransvoxelUnity.h"
#include "Include/Platform.h"

bool tPoolInit(tPool* pool, u32 numItems, tPoolSpawnFn spawn, tPoolDestroyFn destroy, void* userData)
{
    if (!pool || !spawn)
    {
        AX_WARN("transvoxel unity pool init failed: invalid argument");
        return false;
    }

    *pool = (tPool){0};
    pool->items = ArrayCreatePrealloc(void*, Maxu32(numItems, 1u));
    if (!pool->items)
    {
        AX_WARN("transvoxel unity pool init failed: allocation failed");
        return false;
    }

    pool->spawn = spawn;
    pool->destroy = destroy;
    pool->userData = userData;

    for (u32 i = 0; i < numItems; i++)
    {
        void* item = spawn(userData);
        if (!item)
        {
            AX_WARN("transvoxel unity pool init failed: spawn failed");
            tPoolDestroy(pool);
            return false;
        }

        ArrayPush(pool->items, item);
    }

    return true;
}

void tPoolDestroy(tPool* pool)
{
    if (!pool)
        return;

    if (pool->destroy && pool->items)
    {
        for (size_t i = 0; i < ArrayLength(pool->items); i++)
            pool->destroy(pool->items[i], pool->userData);
    }

    ArrayDestroy(pool->items);
    *pool = (tPool){0};
}

void* tPoolGet(tPool* pool)
{
    if (!pool || !pool->items || !pool->spawn)
    {
        AX_WARN("transvoxel unity pool get failed: invalid pool");
        return NULL;
    }

    void* item = NULL;
    if (ArrayLength(pool->items) > 0)
    {
        ArrayPop(pool->items, &item);
        return item;
    }

    item = pool->spawn(pool->userData);
    if (!item)
        AX_WARN("transvoxel unity pool get failed: spawn failed");

    return item;
}

bool tPoolAdd(tPool* pool, void* item)
{
    if (!pool || !pool->items || !item)
    {
        AX_WARN("transvoxel unity pool add failed: invalid argument");
        return false;
    }

    size_t oldCount = ArrayLength(pool->items);
    ArrayPush(pool->items, item);
    return ArrayLength(pool->items) == oldCount + 1;
}
