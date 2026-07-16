#ifndef TEXTURE_SYSTEM_H
#define TEXTURE_SYSTEM_H

#include "Graphics.h"
#include "Extern/SmolAtlas.h"

enum { TextureClass_Albedo, TextureClass_Normal, TextureClass_MetallicRoughness, TextureClass_Count };

// compressed pages keep the last mips as cpu shadows because block alignment
// cannot be guaranteed for region uploads at those sizes
#define TEXTURE_PAGE_TAIL_MIPS 3

// compressed pages: mips 0..TEXTURE_PAGE_DIRECT_MIPS-1 are region uploaded straight to the
// gpu, TEXTURE_PAGE_ALIGN keeps every region block aligned at those mips. the remaining
// tail mips live as cpu shadows and re-upload whole after each append
#define TEXTURE_PAGE_ALIGN       64u
#define TEXTURE_PAGE_DIRECT_MIPS 5u
#define TEXTURE_PAGE_MAX_MIPS    (TEXTURE_PAGE_DIRECT_MIPS + TEXTURE_PAGE_TAIL_MIPS)

typedef struct TexturePageClass_
{
    Texture     pages;                            // 2d array texture with layerCount layers
    u32         layerCount;                       // gpu layers, grows 1 -> 2 -> 4 -> 8
    u32         openPages;                        // pages with live packer state
    SmolAtlas*  packer[TEXTURE_PAGE_LAYERS];       // persistent, packs incrementally across appends, items removable
    u8*         tailMips[TEXTURE_PAGE_LAYERS][TEXTURE_PAGE_TAIL_MIPS]; // compressed path only
} TexturePageClass;

// tracks the atlas item backing a live descriptor so its page-space can be reclaimed on
// removal instead of leaking until TextureSystem_ResetPacking. item is NULL for descriptors
// that never went through the packer (defaults, or restored from a baked dump)
typedef struct TextureDescriptorPacking_
{
    u32            textureClass;
    SmolAtlasItem* item;
} TextureDescriptorPacking;

// per scene texture state: page atlases, descriptors and materials.
// material slots are stable: bundle->materialOffset never moves once assigned.
// removal frees material/descriptor slots and their atlas item, reclaiming page space immediately.
typedef struct TextureSystem_
{
    TextureDescriptor*        descriptors;       // MAX_TEXTURE_DESCRIPTORS
    TextureDescriptorPacking* descriptorPacking;  // MAX_TEXTURE_DESCRIPTORS
    MaterialGPU*              materials;         // MAX_GPU_MATERIALS
    u64*                      descriptorSlots;   // MAX_TEXTURE_DESCRIPTORS bits, 1 means occupied
    u32                numDescriptors;
    u32                materialWatermark; // highest used material slot + 1
    SDL_GPUBuffer*     descriptorBuffer;
    SDL_GPUBuffer*     materialBuffer;
    TexturePageClass   classes[TextureClass_Count];
    SDL_GPUTextureFormat albedoFormat;
    SDL_GPUTextureFormat rgFormat;
    u32                compressed;        // block compressed page path for the lifetime of the instance
} TextureSystem;

// creates the shared page copy compute pipelines, call once after GraphicsInit
void TextureSystem_InitDevice(void);
void TextureSystem_DestroyDevice(void);

void TextureSystem_Init(TextureSystem* ts);
void TextureSystem_Destroy(TextureSystem* ts);

// packs the bundle's images into the pages, appends descriptors and writes the bundle's
// materials at [materialOffset, +numMaterials). stagingTextures is bundle local:
// stagingTextures[i] holds image i (basis buffer for compressed pages, gpu handle otherwise).
// caller keeps ownership of the staging textures. out: 0 on failure
s32 TextureSystem_AppendBundle(TextureSystem* ts, const SceneBundle* bundle, const Texture* stagingTextures, u32 materialOffset);

// clears the bundle's material slots to defaults and releases descriptor slots, freeing
// their atlas items back to the page packer (skipped for descriptors with no live item,
// i.e. defaults or ones restored from a baked dump)
void TextureSystem_RemoveBundle(TextureSystem* ts, const SceneBundle* bundle, u32 materialOffset);

// downloads the class pages from gpu memory and writes them to path as a raw dump in
// the platform's block compressed format (file write runs on a worker thread). the dump
// is editor local, game builds will bake basis instead. out: 0 on failure or empty class
s32 TextureSystem_SaveBakedClass(TextureSystem* ts, u32 textureClass, const char* path);

// restores a freshly initialized texture system from raw page dumps plus the saved
// descriptor and material tables (descriptors include the default entries). atlasPaths
// entries may be NULL for classes that only hold defaults. fails when a dump's format
// does not match the platform, callers fall back to a repack. packer state is NOT
// restored, the caller must repack before appending new bundles. out: 0 on failure
s32 TextureSystem_RestoreBaked(TextureSystem* ts,
                               const char* atlasPaths[TextureClass_Count],
                               const TextureDescriptor* descriptors, u32 numDescriptors,
                               const MaterialGPU* materials, u32 materialWatermark);

// drops all pages and descriptors so the caller can re-append every live bundle.
// materials and their offsets are preserved, re-appends rewrite them in place
void TextureSystem_ResetPacking(TextureSystem* ts);

// releases the gpu handles and cpu basis buffers of a texture range
void TextureSystem_ReleaseTextures(Texture* textures, u32 count);

#endif // TEXTURE_SYSTEM_H
