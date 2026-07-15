#ifndef BASIS_COMPRESSOR_C_H
#define BASIS_COMPRESSOR_C_H

#include "Texture.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compression format flags
#define BASIS_FORMAT_ETC1S  0x01   // Use ETC1S (high compression, lower quality)
#define BASIS_FORMAT_UASTC  0x02   // Use UASTC (higher quality, larger files)
// If neither flag is given, UASTC is used by default.

// Other flags
#define BASIS_FLAG_NORMAL_MAP     0x04   // Input is a normal map (disable sRGB, renormalize)
#define BASIS_FLAG_NO_MIPMAPS     0x08   // Disable /automatic mipmap generation
#define BASIS_FLAG_Y_FLIP         0x10   // Flip image vertically before compression
#define BASIS_FLAG_METALLIC_ROUGHNESS 0x20
#define BASIS_FLAG_LINEAR         0x40   // Use linear metrics without channel swizzling/BC5 preference

// Quality preset for fast/slow trade-off (used when quality level is not set)
#define BASIS_QUALITY_FASTEST     0
#define BASIS_QUALITY_FAST        1
#define BASIS_QUALITY_DEFAULT     2
#define BASIS_QUALITY_SLOW        3
#define BASIS_QUALITY_VERY_SLOW   4

/**
 * Initializes the Basis Universal encoder library.
 * Must be called once before any compression functions.
 * @return 0 on success, non-zero on failure.
 */
int basis_encoder_init(void);

/**
 * Compresses an image file (PNG, JPG, TGA, BMP, etc.) to a .basis file.
 *
 * @param input_filename    Path to the input image.
 * @param output_filename   Path where the .basis file will be written.
 * @param flags             Bitwise OR of BASIS_FORMAT_* and BASIS_FLAG_*.
 *                          If neither BASIS_FORMAT_ETC1S nor BASIS_FORMAT_UASTC is given,
 *                          UASTC is used by default.
 * @param mip_smallest_dim  Smallest dimension for the mip chain (e.g., 256).
 *                          Ignored if BASIS_FLAG_NO_MIPMAPS is set.
 * @param quality_level     Compression quality (0..255 for ETC1S, 0..100 for UASTC).
 *                          Use -1 for library default.
 * @param effort_level      Compression effort/speed (0..10). Use -1 for default.
 * @return 0 on success, non-zero error code on failure.
 */
int basis_compress_file(const char* input_filename,
                        const char* output_filename,
                        unsigned int flags,
                        int mip_smallest_dim,
                        int quality_level,
                        int effort_level);

/**
 * Compresses in-memory RGBA32 layers (with caller supplied mip chains) into a
 * texture-2D-array .basis file. Used to bake texture atlas pages.
 *
 * No channel swizzling is applied: the layer data is expected to already be in the
 * channel layout the engine stores in basis files (e.g. normal maps with y in alpha).
 * BASIS_FLAG_NORMAL_MAP / BASIS_FLAG_METALLIC_ROUGHNESS only select linear (non sRGB)
 * encoding metrics.
 *
 * @param layer_mips  layer-major array of numLayers*numMips RGBA32 pointers:
 *                    layer_mips[layer*numMips + mip], mip i is (width>>i, height>>i)
 *                    texels, minimum 1x1.
 * @param num_layers  number of array layers (basis images), >= 1.
 * @param num_mips    mip levels per layer, >= 1. Mips are taken as-is, none generated.
 * @return 0 on success, non-zero error code on failure.
 */
int basis_compress_array_memory(const unsigned char* const* layer_mips,
                                int num_layers,
                                int num_mips,
                                int width,
                                int height,
                                unsigned int flags,
                                int quality_level,
                                int effort_level,
                                const char* output_filename);

/**
 * Loads/resizes the same path list used by LoadTextureArray(), builds a full
 * RGBA32 mip chain per layer, and writes it as a texture-2D-array .basis file.
 *
 * @param paths           layer paths, count entries.
 * @param count           number of texture array layers.
 * @param size            target square layer size, matching LoadTextureArray().
 * @param srgb            use sRGB resizing for source layer normalization.
 * @param flags           BASIS_FORMAT_* and BASIS_FLAG_*.
 * @return 0 on success, non-zero error code on failure.
 */
int basis_compress_texture_array_files(const char* const* paths,
                                       unsigned int count,
                                       int size,
                                       bool srgb,
                                       unsigned int flags,
                                       int quality_level,
                                       int effort_level,
                                       const char* output_filename);

/**
 * Loads a texture-2D-array .basis file and uploads it as a shader-ready Texture.
 *
 * flags only controls target GPU format selection for UASTC files:
 * BASIS_FLAG_NORMAL_MAP and BASIS_FLAG_METALLIC_ROUGHNESS prefer BC5/ASTC over BC7.
 * If the chosen compressed format is unsupported, the loader falls back to RGBA8.
 */
Texture basis_load_texture_array(const char* input_filename,
                                 unsigned int flags,
                                 const char* label);

/**
 * Loads an existing texture-2D-array .basis file or builds it from source paths,
 * then returns a shader-ready Texture. Falls back to an uncompressed RGBA8 array
 * if basis build/load fails.
 */
Texture basis_load_or_build_texture_array(const char* const* paths,
                                          unsigned int count,
                                          int size,
                                          bool srgb,
                                          const char* basis_filename,
                                          unsigned int flags,
                                          const char* label);

#ifdef __cplusplus
}
#endif

#endif /* BASIS_COMPRESSOR_C_H */
