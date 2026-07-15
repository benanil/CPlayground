#ifndef DDS_TEXTURE_H
#define DDS_TEXTURE_H

#include "Common.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct DDSImage_
{
    u32 width;
    u32 height;
    u8* pixels;
} DDSImage;

bool DDSLoadDecompressImage(const char* inputFilename, DDSImage* outImage);
void DDSFreeImage(DDSImage* image);

#if defined(__cplusplus)
}
#endif

#endif // DDS_TEXTURE_H
