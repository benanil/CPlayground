#ifndef _H_GRAPHICS_
#define _H_GRAPHICS_
typedef enum TexFlags_
{
    TexFlags_None        = 0,
    TexFlags_MipMap      = 1,
    TexFlags_Compressed  = 2,
    TexFlags_ClampToEdge = 4,
    TexFlags_Nearest     = 8,
    TexFlags_Linear      = 16, // default linear in desktop platforms
    TexFlags_DontDeleteCPUBuffer = 32,
    // no filtering or wrapping
    TexFlags_RawData     = TexFlags_Nearest | TexFlags_ClampToEdge
} TexFlags;

#endif