// SPDX-License-Identifier: MIT OR Unlicense
// smol-atlas: https://github.com/aras-p/smol-atlas
#ifndef SA_INCLUDE_SMOL_ATLAS
#define SA_INCLUDE_SMOL_ATLAS

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct SmolAtlasItem_ {
    int x;
    int y;
    int width;
    int height;
    int shelfIndex;
} SmolAtlasItem;

typedef struct SmolAtlas_ SmolAtlas;

SmolAtlas*     SACreate(int width, int height);
void           SADestroy(SmolAtlas* atlas);

int            SAWidth(const SmolAtlas* atlas);
int            SAHeight(const SmolAtlas* atlas);

SmolAtlasItem* SAPack(SmolAtlas* atlas, int width, int height);
void           SAItemRemove(SmolAtlas* atlas, SmolAtlasItem* item);
void           SAClear(SmolAtlas* atlas, int newWidth, int newHeight);

int            SAItemX(const SmolAtlasItem* item);
int            SAItemY(const SmolAtlasItem* item);
int            SAItemWidth(const SmolAtlasItem* item);
int            SAItemHeight(const SmolAtlasItem* item);

#endif // SA_INCLUDE_SMOL_ATLAS

// implementation lives outside the include guard above so a header can pull in the
// declarations only, while exactly one translation unit defines SA_SMOL_ATLAS_IMPLEMENTATION
// before including this file (again) to emit the function bodies.
#if defined(SA_SMOL_ATLAS_IMPLEMENTATION) && !defined(SA_SMOL_ATLAS_IMPLEMENTED)
#define SA_SMOL_ATLAS_IMPLEMENTED

#ifndef SAAlloc
    #include <stdlib.h>
    #define SAAlloc(size) malloc(size)
    #define SARealloc(mem, size) realloc(mem, size)
    #define SAFree(mem) free(mem)
#endif

#ifndef SAAssert
    #include <assert.h>
    #define SAAssert(cond) assert(cond)
#endif

// "memory pool" that allocates fixed-size chunks of same-size items,
// and maintains a freelist of items for O(1) alloc and free.
typedef struct SAPoolChunk_ {
    void*                storage;
    struct SAPoolChunk_*  next;
} SAPoolChunk;

typedef struct SAPool_ {
    SAPoolChunk* chunk;
    void*        freeList;
    uint32_t     elemSize;   // clamped to >= sizeof(void*), freelist node overlaps item storage
    uint32_t     chunkItems;
} SAPool;

static void SAPoolLinkChunk(SAPoolChunk* chunk, uint32_t elemSize, uint32_t count, void* tail)
{
    char* base = (char*)chunk->storage;
    for (uint32_t i = 0; i + 1 < count; ++i)
        *(void**)(base + (size_t)i * elemSize) = base + (size_t)(i + 1) * elemSize;
    *(void**)(base + (size_t)(count - 1) * elemSize) = tail;
}

static SAPoolChunk* SAPoolChunkCreate(uint32_t elemSize, uint32_t count)
{
    SAPoolChunk* chunk = (SAPoolChunk*)SAAlloc(sizeof(SAPoolChunk));
    chunk->storage = SAAlloc((size_t)elemSize * count);
    chunk->next = NULL;
    SAPoolLinkChunk(chunk, elemSize, count, NULL);
    return chunk;
}

static void SAPoolInit(SAPool* pool, uint32_t elemSize, uint32_t chunkItems)
{
    pool->elemSize = elemSize < (uint32_t)sizeof(void*) ? (uint32_t)sizeof(void*) : elemSize;
    pool->chunkItems = chunkItems;
    pool->chunk = SAPoolChunkCreate(pool->elemSize, chunkItems);
    pool->freeList = pool->chunk->storage;
}

// grabs item from free list, creating a new chunk if current one is full
static void* SAPoolAlloc(SAPool* pool)
{
    if (pool->freeList == NULL) {
        SAPoolChunk* chunk = SAPoolChunkCreate(pool->elemSize, pool->chunkItems);
        chunk->next = pool->chunk;
        pool->chunk = chunk;
        pool->freeList = chunk->storage;
    }
    void* item = pool->freeList;
    pool->freeList = *(void**)item;
    return item;
}

static void SAPoolFree(SAPool* pool, void* ptr)
{
    *(void**)ptr = pool->freeList;
    pool->freeList = ptr;
}

// makes all items "unused", but keeps the allocated space
static void SAPoolClear(SAPool* pool)
{
    SAPoolChunk* chunk = pool->chunk;
    while (chunk) {
        SAPoolChunk* next = chunk->next;
        SAPoolLinkChunk(chunk, pool->elemSize, pool->chunkItems, next ? next->storage : NULL);
        chunk = next;
    }
    pool->freeList = pool->chunk ? pool->chunk->storage : NULL;
}

static void SAPoolDestroy(SAPool* pool)
{
    SAPoolChunk* chunk = pool->chunk;
    while (chunk) {
        SAPoolChunk* next = chunk->next;
        SAFree(chunk->storage);
        SAFree(chunk);
        chunk = next;
    }
    pool->chunk = NULL;
    pool->freeList = NULL;
}

typedef struct SAFreeSpan_ {
    int                 x;
    int                 width;
    struct SAFreeSpan_* next;
} SAFreeSpan;

typedef struct SAShelf_ {
    SAFreeSpan* freeSpans;
    int         y;
    int         height;
    int         index;
} SAShelf;

struct SmolAtlas_ {
    SAShelf* shelves;
    uint32_t shelfCount;
    uint32_t shelfCapacity;
    SAPool   itemPool;
    SAPool   spanPool;
    int      width;
    int      height;
};

static int SAMaxInt(int a, int b)
{
    return a > b ? a : b;
}

static bool SAShelfHasSpaceFor(const SAShelf* shelf, int width)
{
    for (SAFreeSpan* it = shelf->freeSpans; it != NULL; it = it->next)
        if (width <= it->width)
            return true;
    return false;
}

static void SAShelfMergeFreeSpans(SAFreeSpan* prev, SAFreeSpan* span, SAPool* spanPool)
{
    SAFreeSpan* next = span->next;
    if (next != NULL && span->x + span->width == next->x) {
        // merge with next
        span->width += next->width;
        span->next = next->next;
        SAPoolFree(spanPool, next);
    }
    if (prev != NULL && prev->x + prev->width == span->x) {
        // merge with prev
        prev->width += span->width;
        prev->next = span->next;
        SAPoolFree(spanPool, span);
    }
}

static void SAShelfAddFreeSpan(SAShelf* shelf, int x, int width, SAPool* spanPool)
{
    SAFreeSpan* freeSpan = (SAFreeSpan*)SAPoolAlloc(spanPool);
    freeSpan->x = x;
    freeSpan->width = width;

    // insert into free spans list at the right position (sorted by x)
    SAFreeSpan* it = shelf->freeSpans;
    SAFreeSpan* prev = NULL;
    while (it != NULL && freeSpan->x >= it->x) {
        prev = it;
        it = it->next;
    }
    freeSpan->next = it;
    if (prev == NULL)
        shelf->freeSpans = freeSpan;
    else
        prev->next = freeSpan;

    SAShelfMergeFreeSpans(prev, freeSpan, spanPool);
}

static void SAShelfInit(SAShelf* shelf, int y, int width, int height, int index, SAPool* spanPool)
{
    shelf->y = y;
    shelf->height = height;
    shelf->index = index;

    SAFreeSpan* span = (SAFreeSpan*)SAPoolAlloc(spanPool);
    span->x = 0;
    span->width = width;
    span->next = NULL;
    shelf->freeSpans = span;
}

static SmolAtlasItem* SAShelfAllocItem(SAShelf* shelf, int w, int h, SAPool* itemPool, SAPool* spanPool)
{
    if (h > shelf->height)
        return NULL;

    // find a suitable free span
    SAFreeSpan* it = shelf->freeSpans;
    SAFreeSpan* prev = NULL;
    while (it != NULL && it->width < w) {
        prev = it;
        it = it->next;
    }

    // no space in this shelf
    if (it == NULL)
        return NULL;

    const int x = it->x;
    const int rest = it->width - w;
    if (rest > 0) {
        // there will be still space left in this span, adjust
        it->x += w;
        it->width -= w;
    } else {
        // whole span is taken, remove it
        if (prev != NULL)
            prev->next = it->next;
        else
            shelf->freeSpans = it->next;
        SAPoolFree(spanPool, it);
    }

    SmolAtlasItem* item = (SmolAtlasItem*)SAPoolAlloc(itemPool);
    item->x = x;
    item->y = shelf->y;
    item->width = w;
    item->height = h;
    item->shelfIndex = shelf->index;
    return item;
}

static void SAShelfFreeItem(SAShelf* shelf, SmolAtlasItem* item, SAPool* itemPool, SAPool* spanPool)
{
    SAAssert(item);
    SAAssert(item->shelfIndex == shelf->index);
    SAAssert(item->y == shelf->y);
    SAShelfAddFreeSpan(shelf, item->x, item->width, spanPool);
    SAPoolFree(itemPool, item);
}

static SAShelf* SAAddShelf(SmolAtlas* atlas, int y, int width, int height, int index)
{
    if (atlas->shelfCount == atlas->shelfCapacity) {
        uint32_t newCapacity = atlas->shelfCapacity ? atlas->shelfCapacity * 2 : 8;
        atlas->shelves = (SAShelf*)SARealloc(atlas->shelves, (size_t)newCapacity * sizeof(SAShelf));
        atlas->shelfCapacity = newCapacity;
    }
    SAShelf* shelf = &atlas->shelves[atlas->shelfCount++];
    SAShelfInit(shelf, y, width, height, index, &atlas->spanPool);
    return shelf;
}

SmolAtlas* SACreate(int width, int height)
{
    SmolAtlas* atlas = (SmolAtlas*)SAAlloc(sizeof(SmolAtlas));
    atlas->shelves = NULL;
    atlas->shelfCount = 0;
    atlas->shelfCapacity = 0;
    SAPoolInit(&atlas->itemPool, (uint32_t)sizeof(SmolAtlasItem), 1024);
    SAPoolInit(&atlas->spanPool, (uint32_t)sizeof(SAFreeSpan), 1024);
    atlas->width = width > 0 ? width : 64;
    atlas->height = height > 0 ? height : 64;
    return atlas;
}

void SADestroy(SmolAtlas* atlas)
{
    if (atlas == NULL)
        return;
    SAPoolDestroy(&atlas->itemPool);
    SAPoolDestroy(&atlas->spanPool);
    SAFree(atlas->shelves);
    SAFree(atlas);
}

int SAWidth(const SmolAtlas* atlas)
{
    return atlas->width;
}

int SAHeight(const SmolAtlas* atlas)
{
    return atlas->height;
}

SmolAtlasItem* SAPack(SmolAtlas* atlas, int w, int h)
{
    // find best shelf
    SAShelf* bestShelf = NULL;
    int bestScore = 0x7fffffff;

    int topY = 0;
    for (uint32_t i = 0; i < atlas->shelfCount; ++i) {
        SAShelf* shelf = &atlas->shelves[i];
        const int shelfHeight = shelf->height;
        topY = SAMaxInt(topY, shelf->y + shelfHeight);

        if (shelfHeight < h)
            continue; // too short

        if (shelfHeight == h) { // exact height fit, try to use it
            SmolAtlasItem* res = SAShelfAllocItem(shelf, w, h, &atlas->itemPool, &atlas->spanPool);
            if (res != NULL)
                return res;
        }

        // otherwise the shelf is too tall, track best one
        const int score = shelfHeight - h;
        if (score < bestScore && SAShelfHasSpaceFor(shelf, w)) {
            bestScore = score;
            bestShelf = shelf;
        }
    }

    if (bestShelf != NULL) {
        SmolAtlasItem* res = SAShelfAllocItem(bestShelf, w, h, &atlas->itemPool, &atlas->spanPool);
        if (res != NULL)
            return res;
    }

    // no shelf with enough space: add a new shelf
    if (w <= atlas->width && h <= atlas->height - topY) {
        SAShelf* shelf = SAAddShelf(atlas, topY, atlas->width, h, (int)atlas->shelfCount);
        return SAShelfAllocItem(shelf, w, h, &atlas->itemPool, &atlas->spanPool);
    }

    // out of space
    return NULL;
}

void SAItemRemove(SmolAtlas* atlas, SmolAtlasItem* item)
{
    if (item == NULL)
        return;
    SAAssert(item->shelfIndex >= 0 && (uint32_t)item->shelfIndex < atlas->shelfCount);
    SAShelfFreeItem(&atlas->shelves[item->shelfIndex], item, &atlas->itemPool, &atlas->spanPool);
}

void SAClear(SmolAtlas* atlas, int newWidth, int newHeight)
{
    SAPoolClear(&atlas->itemPool);
    SAPoolClear(&atlas->spanPool);
    atlas->shelfCount = 0;
    if (newWidth > 0)  atlas->width = newWidth;
    if (newHeight > 0) atlas->height = newHeight;
}

int SAItemX(const SmolAtlasItem* item)
{
    return item->x;
}
int SAItemY(const SmolAtlasItem* item)
{
    return item->y;
}
int SAItemWidth(const SmolAtlasItem* item)
{
    return item->width;
}
int SAItemHeight(const SmolAtlasItem* item)
{
    return item->height;
}

#endif // SA_SMOL_ATLAS_IMPLEMENTATION && !SA_SMOL_ATLAS_IMPLEMENTED
