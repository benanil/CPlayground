#ifndef TEXTURE_H
#define TEXTURE_H

#include "IntFloat.h"

#include <SDL3/SDL_gpu.h>

typedef struct Texture_
{
    s32 width, height;
    SDL_GPUTexture* handle;
    SDL_GPUTextureFormat format;
    void* buffer;
    u64 bufferSize;
    u32 mipLevels;
    u32 numLayers;
    s8 channels;
    s8 type;
} Texture;

#endif // TEXTURE_H
