#include "Include/BasisCompressWrapper.h"

#include "Extern/basis_universal/encoder/basisu_comp.h"          // Main BasisU encoder API
#include "Extern/basis_universal/encoder/basisu_enc.h"           // For image
#include "Extern/basis_universal/transcoder/basisu_file_headers.h"  // For basis_tex_format
#include "Extern/basis_universal/transcoder/basisu_transcoder.h"
#include "Include/DDSTexture.h"
#include "Include/FileSystem.h"
#include "Include/Memory.h"
#include "Include/ParallelFor.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include "Extern/stb/stb_image.h"
#include "Extern/stb/stb_image_resize2.h"

using namespace basisu;

#ifndef BASISU_MAX_TRANSCODED_MIPS
#define BASISU_MAX_TRANSCODED_MIPS 16
#endif

extern "C" SDL_GPUDevice* g_GPUDevice;

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

static bool load_input_image(const char* input_filename, image& img) {
    if (FileHasExtension(input_filename, (int)SDL_strlen(input_filename), ".dds")) {
        DDSImage dds;
        if (!DDSLoadDecompressImage(input_filename, &dds)) {
            return false;
        }

        image decoded(dds.width, dds.height);
        MemCopy(decoded.get_ptr(), dds.pixels, (size_t)dds.width * dds.height * 4u);
        DDSFreeImage(&dds);

        img = decoded;
        return true;
    }

    int w, h, ch;
    unsigned char* data = stbi_load(input_filename, &w, &h, &ch, 4);
    if (!data) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "stbi_load failed: %s", stbi_failure_reason());
        return false;
    }

    image decoded(w, h);
    MemCopy(decoded.get_ptr(), data, (size_t)w * h * 4);
    stbi_image_free(data);

    img = decoded;
    return true;
}

static uint32_t get_worker_count(void) {
    int cores = SDL_GetNumLogicalCPUCores();
    if (cores < 1) cores = 1;
    return (uint32_t)cores;
}

static uint32_t get_array_mip_count(uint32_t size) {
    uint32_t levels = 1;
    while (size > 1) {
        size >>= 1;
        levels++;
    }
    return levels;
}

static bool load_texture_array_layer_rgba(const char* path, int size, bool srgb, unsigned char* dst) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* image = NULL;
    DDSImage dds = {};
    bool ddsImage = false;

    if (!path || !dst || size < 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "load_texture_array_layer_rgba: invalid arguments");
        return false;
    }

    if (FileHasExtension(path, (int)SDL_strlen(path), ".dds")) {
        if (!DDSLoadDecompressImage(path, &dds)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "dds image loading failed! %s", path);
            return false;
        }
        width = (int)dds.width;
        height = (int)dds.height;
        channels = 4;
        image = dds.pixels;
        ddsImage = true;
    } else {
        image = stbi_load(path, &width, &height, &channels, 4);
    }

    if (!image) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "texture array layer missing: %s", path);
        return false;
    }

    (void)channels;
    if (width == size && height == size) {
        MemCopy(dst, image, (size_t)size * size * 4u);
    } else {
        if (srgb) {
            stbir_resize_uint8_srgb(image, width, height, 0, dst, size, size, 0, STBIR_RGBA);
        } else {
            stbir_resize_uint8_linear(image, width, height, 0, dst, size, size, 0, STBIR_RGBA);
        }
    }

    if (ddsImage) {
        DDSFreeImage(&dds);
    } else {
        stbi_image_free(image);
    }
    return true;
}

static void generate_texture_array_mips(unsigned char** layer_mips, int layer, int num_mips, int size, bool srgb) {
    for (int mip = 1; mip < num_mips; mip++) {
        int srcSize = size >> (mip - 1);
        int dstSize = size >> mip;
        if (srcSize < 1) srcSize = 1;
        if (dstSize < 1) dstSize = 1;

        unsigned char* src = layer_mips[(size_t)layer * num_mips + mip - 1];
        unsigned char* dst = layer_mips[(size_t)layer * num_mips + mip];
        if (srgb) {
            stbir_resize_uint8_srgb(src, srcSize, srcSize, 0, dst, dstSize, dstSize, 0, STBIR_RGBA);
        } else {
            stbir_resize_uint8_linear(src, srcSize, srcSize, 0, dst, dstSize, dstSize, 0, STBIR_RGBA);
        }
    }
}

static basist::transcoder_texture_format sdl_format_to_basis_format(SDL_GPUTextureFormat format) {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:   return basist::transcoder_texture_format::cTFBC1_RGB;
        case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:   return basist::transcoder_texture_format::cTFBC3_RGBA;
        case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM:      return basist::transcoder_texture_format::cTFBC4_R;
        case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:     return basist::transcoder_texture_format::cTFBC5_RG;
        case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:   return basist::transcoder_texture_format::cTFBC7_RGBA;
        case SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM:   return basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:   return basist::transcoder_texture_format::cTFRGBA32;
        default: return basist::transcoder_texture_format::cTFTotalTextureFormats;
    }
}

static bool is_block_compressed_sdl_format(SDL_GPUTextureFormat format) {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:
        case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:
        case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM:
        case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:
        case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:
        case SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM:
            return true;
        default:
            return false;
    }
}

static bool texture_array_format_supported(SDL_GPUTextureFormat format) {
    return SDL_GPUTextureSupportsFormat(g_GPUDevice, format, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREUSAGE_SAMPLER);
}

static SDL_GPUTextureFormat choose_basis_array_format(basist::basis_tex_format basis_format, bool has_alpha, unsigned int flags) {
    const bool two_channel = (flags & (BASIS_FLAG_NORMAL_MAP | BASIS_FLAG_METALLIC_ROUGHNESS)) != 0;
    SDL_GPUTextureFormat preferred = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    if (basis_format == basist::basis_tex_format::cETC1S) {
        preferred = has_alpha ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    } else if (basis_format == basist::basis_tex_format::cUASTC_LDR_4x4) {
#ifdef __ANDROID__
        preferred = SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
#else
        preferred = two_channel ? SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM : SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
#endif
    }

    if (texture_array_format_supported(preferred) && sdl_format_to_basis_format(preferred) != basist::transcoder_texture_format::cTFTotalTextureFormats) {
        return preferred;
    }
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

static bool upload_basis_array_layer(SDL_GPUTexture* texture, basist::basisu_transcoder* transcoder,
                                     const void* basis_data, uint32_t basis_size,
                                     uint32_t layer, uint32_t mip_levels,
                                     basist::transcoder_texture_format transcode_format,
                                     SDL_GPUTextureFormat texture_format)
{
    uint32_t bytes_per_block = basist::basis_get_bytes_per_block_or_pixel(transcode_format);
    uint32_t offsets[BASISU_MAX_TRANSCODED_MIPS];
    uint32_t sizes[BASISU_MAX_TRANSCODED_MIPS];
    uint32_t widths[BASISU_MAX_TRANSCODED_MIPS];
    uint32_t heights[BASISU_MAX_TRANSCODED_MIPS];
    uint32_t blocks[BASISU_MAX_TRANSCODED_MIPS];
    uint32_t total_size = 0;

    if (mip_levels > BASISU_MAX_TRANSCODED_MIPS) {
        mip_levels = BASISU_MAX_TRANSCODED_MIPS;
    }

    for (uint32_t mip = 0; mip < mip_levels; mip++) {
        transcoder->get_image_level_desc(basis_data, basis_size, layer, mip, widths[mip], heights[mip], blocks[mip]);
        offsets[mip] = total_size;
        sizes[mip] = blocks[mip] * bytes_per_block;
        total_size += sizes[mip];
    }

    SDL_GPUTransferBufferCreateInfo transfer_desc;
    SDL_memset(&transfer_desc, 0, sizeof(transfer_desc));
    transfer_desc.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_desc.size = total_size;

    SDL_GPUTransferBuffer* transfer_buffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transfer_desc);
    if (!transfer_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis texture array transfer buffer creation failed");
        return false;
    }

    unsigned char* ptr = (unsigned char*)SDL_MapGPUTransferBuffer(g_GPUDevice, transfer_buffer, false);
    if (!ptr) {
        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfer_buffer);
        return false;
    }

    for (uint32_t mip = 0; mip < mip_levels; mip++) {
        bool ok = transcoder->transcode_image_level(basis_data, basis_size, layer, mip,
                                                    ptr + offsets[mip],
                                                    sizes[mip] / bytes_per_block,
                                                    transcode_format, 0);
        if (!ok) {
            SDL_UnmapGPUTransferBuffer(g_GPUDevice, transfer_buffer);
            SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfer_buffer);
            return false;
        }
    }
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, transfer_buffer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);
    bool block_compressed = is_block_compressed_sdl_format(texture_format);

    for (uint32_t mip = 0; mip < mip_levels; mip++) {
        SDL_GPUTextureTransferInfo transfer_info;
        MemSet(&transfer_info, 0, sizeof(transfer_info));
        transfer_info.transfer_buffer = transfer_buffer;
        transfer_info.offset = offsets[mip];
        transfer_info.pixels_per_row = block_compressed ? 0u : widths[mip];
        transfer_info.rows_per_layer = block_compressed ? 0u : heights[mip];

        SDL_GPUTextureRegion region;
        MemSet(&region, 0, sizeof(region));
        region.texture = texture;
        region.mip_level = mip;
        region.layer = layer;
        region.w = widths[mip];
        region.h = heights[mip];
        region.d = 1;
        SDL_UploadToGPUTexture(copy_pass, &transfer_info, &region, false);
    }

    SDL_EndGPUCopyPass(copy_pass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfer_buffer);
    return true;
}

static bool upload_rgba_array_layer(Texture texture, u32 layer, const unsigned char* pixels)
{
    const u32 layer_size = (u32)texture.width * (u32)texture.height * 4u;
    SDL_GPUTransferBufferCreateInfo transfer_desc;
    SDL_memset(&transfer_desc, 0, sizeof(transfer_desc));
    transfer_desc.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_desc.size = layer_size;

    SDL_GPUTransferBuffer* transfer_buffer = SDL_CreateGPUTransferBuffer(g_GPUDevice, &transfer_desc);
    if (!transfer_buffer) {
        return false;
    }

    unsigned char* map = (unsigned char*)SDL_MapGPUTransferBuffer(g_GPUDevice, transfer_buffer, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfer_buffer);
        return false;
    }
    MemCopy(map, pixels, layer_size);
    SDL_UnmapGPUTransferBuffer(g_GPUDevice, transfer_buffer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo transfer_info;
    SDL_memset(&transfer_info, 0, sizeof(transfer_info));
    transfer_info.transfer_buffer = transfer_buffer;
    transfer_info.pixels_per_row = (u32)texture.width;
    transfer_info.rows_per_layer = (u32)texture.height;

    SDL_GPUTextureRegion region;
    SDL_memset(&region, 0, sizeof(region));
    region.texture = texture.handle;
    region.layer = layer;
    region.w = (u32)texture.width;
    region.h = (u32)texture.height;
    region.d = 1;

    SDL_UploadToGPUTexture(copy_pass, &transfer_info, &region, false);
    SDL_EndGPUCopyPass(copy_pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    SDL_ReleaseGPUTransferBuffer(g_GPUDevice, transfer_buffer);
    return true;
}

static Texture create_rgba_texture_array_from_files(const char* const* paths,
                                                    unsigned int count,
                                                    int size,
                                                    bool srgb,
                                                    const char* label)
{
    Texture texture = {};
    if (!paths || count < 1 || size < 1) {
        return texture;
    }

    SDL_GPUTextureCreateInfo tex_desc;
    SDL_memset(&tex_desc, 0, sizeof(tex_desc));
    tex_desc.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tex_desc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_desc.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tex_desc.width = (u32)size;
    tex_desc.height = (u32)size;
    tex_desc.layer_count_or_depth = count;
    tex_desc.num_levels = get_array_mip_count((u32)size);
    tex_desc.sample_count = SDL_GPU_SAMPLECOUNT_1;
    tex_desc.props = SDL_CreateProperties();

    if (label) {
        SDL_SetStringProperty(tex_desc.props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, label);
    }

    texture.handle = SDL_CreateGPUTexture(g_GPUDevice, &tex_desc);
    SDL_DestroyProperties(tex_desc.props);
    if (!texture.handle) {
        return texture;
    }

    texture.width = size;
    texture.height = size;
    texture.format = tex_desc.format;
    texture.mipLevels = tex_desc.num_levels;
    texture.numLayers = count;
    texture.channels = 4;

    unsigned char* pixels = (unsigned char*)SDL_malloc((size_t)size * size * 4u);
    if (!pixels) {
        SDL_ReleaseGPUTexture(g_GPUDevice, texture.handle);
        return Texture{};
    }

    bool ok = true;
    for (u32 layer = 0; layer < count; layer++) {
        if (!load_texture_array_layer_rgba(paths[layer], size, srgb, pixels) ||
            !upload_rgba_array_layer(texture, layer, pixels)) {
            ok = false;
            break;
        }
    }
    SDL_free(pixels);

    if (!ok) {
        SDL_ReleaseGPUTexture(g_GPUDevice, texture.handle);
        return Texture{};
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GenerateMipmapsForGPUTexture(cmd, texture.handle);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_GPUDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(g_GPUDevice, fence);
    return texture;
}

int basis_encoder_init(void) {
    basisu_encoder_init(); // already mutex-guarded + one-shot internally
    return 0;
}

// Determine compression format (ETC1S or UASTC) and quality/effort settings
static bool setup_format_params(basis_compressor_params& params,
                                unsigned int flags,
                                int quality_level,
                                int effort_level) {
    bool use_etc1s = (flags & BASIS_FORMAT_ETC1S) != 0;
    bool use_uastc = (flags & BASIS_FORMAT_UASTC) != 0;
    if (!use_etc1s && !use_uastc) {
        // Default to UASTC (same as original command line behaviour for non-metallic)
        use_uastc = true;
    }

    if (use_etc1s && use_uastc) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Cannot specify both ETC1S and UASTC.");
        return false;
    }

    if (use_etc1s) {
        params.set_format_mode(basist::basis_tex_format::cETC1S);
        // ETC1S quality: quality_level [1,255] (or 0? API says 1..255)
        if (quality_level >= 0) {
            params.m_quality_level = quality_level;
        } else {
            params.m_quality_level = BASISU_DEFAULT_QUALITY; // 128
        }
        // ETC1S effort: effort_level maps to compression_level (0..BASISU_MAX_ETC1S_COMPRESSION_LEVEL)
        if (effort_level >= 0) {
            // Map effort 0..10 to 0..BASISU_MAX_ETC1S_COMPRESSION_LEVEL (which is 3 as of 2.10)
            const int max_etc1s_effort = BASISU_MAX_ETC1S_COMPRESSION_LEVEL; // = 3
            int comp_level = (effort_level * max_etc1s_effort) / 10;
            params.m_etc1s_compression_level = comp_level;
        } else {
            params.m_etc1s_compression_level = 2; // good default
        }
    } else { // UASTC (including HDR detection? We assume LDR for now)
        // For LDR 4x4 UASTC (the most common)
        params.set_format_mode(basist::basis_tex_format::cUASTC_LDR_4x4);
        // Quality: UASTC pack level (0..4) map quality_level (0..100) to 0..4
        if (quality_level >= 0) {
            int pack_level = (quality_level * 4) / 100;
            if (pack_level < 0) pack_level = 0;
            if (pack_level > 4) pack_level = 4;
            // The pack flags control effort/quality for UASTC encoding
            // We'll use the standard flags from basisu_comp.h
            static const uint32_t pack_flags[] = {
                cPackUASTCLevelFastest,
                cPackUASTCLevelFaster,
                cPackUASTCLevelDefault,
                cPackUASTCLevelSlower,
                cPackUASTCLevelVerySlow
            };
            params.m_pack_uastc_ldr_4x4_flags = pack_flags[pack_level];
        } else {
            params.m_pack_uastc_ldr_4x4_flags = cPackUASTCLevelDefault;
        }

        params.m_rdo_uastc_ldr_4x4 = false;
    }
    return true;
}

// Convert flags and quality/effort to basis_compressor_params
static bool setup_params(basis_compressor_params& params,
                         const char* input_filename,
                         const char* output_filename,
                         unsigned int flags,
                         int mip_smallest_dim,
                         int quality_level,
                         int effort_level,
                         bool multithreaded) {
    // Clear and set basic output
    params.clear();
    params.m_out_filename = output_filename;
    params.m_write_output_basis_or_ktx2_files = true;  // let the compressor write directly
    params.m_create_ktx2_file = false;                 // we want .basis, not KTX2

    image img;
    if (!load_input_image(input_filename, img)) {
        return false;
    }

    if (flags & BASIS_FLAG_NORMAL_MAP) {
        for (uint32_t y = 0; y < img.get_height(); y++) {
            for (uint32_t x = 0; x < img.get_width(); x++) {
                color_rgba& p = img(x, y);
                p.a = p.g; // Basis BC5 transcodes R+A into the two BC4 blocks.
            }
        }
    } else if (flags & BASIS_FLAG_METALLIC_ROUGHNESS) {
        for (uint32_t y = 0; y < img.get_height(); y++) {
            for (uint32_t x = 0; x < img.get_width(); x++) {
                color_rgba& p = img(x, y);
                const uint8_t metallic = p.b;
                const uint8_t roughness = p.g;
                p.r = metallic;
                p.a = roughness; // Basis BC5 expects the second channel in alpha.
                p.g = roughness;
                p.b = 0;
            }
        }
    }
    params.m_source_images.push_back(img);

    // Flags handling
    params.m_y_flip = ((flags & BASIS_FLAG_Y_FLIP) != 0);

    bool linear = (flags & (BASIS_FLAG_NORMAL_MAP | BASIS_FLAG_METALLIC_ROUGHNESS | BASIS_FLAG_LINEAR)) != 0;

    // Normal map: renormalize normals, disable perceptual (linear) metrics
    if (flags & BASIS_FLAG_NORMAL_MAP) {
        params.m_renormalize = true;
    } else {
        params.m_renormalize = false;
    }
    params.m_perceptual = !linear;
    params.m_ktx2_and_basis_srgb_transfer_function = !linear;
    params.m_mip_srgb = !linear;

    // Mipmap generation
    if (!(flags & BASIS_FLAG_NO_MIPMAPS)) {
        params.m_mip_gen = true;
        params.m_mip_smallest_dimension = mip_smallest_dim;
        // Use a good default filter
        params.m_mip_filter = "kaiser";
        params.m_mip_wrapping = true;
        params.m_mip_fast = false;   // better quality mipmaps
    } else {
        params.m_mip_gen = false;
    }

    if (!setup_format_params(params, flags, quality_level, effort_level))
        return false;

    // per-call job pool: only worth it for a lone call, not when the caller already runs many
    // of these in parallel (ParallelFor over a batch) - status output would garble too then.
    params.m_multithreading = multithreaded;
    params.m_status_output = multithreaded;
    params.m_print_stats = false;   // avoid extra clutter
    return true;
}

// -----------------------------------------------------------------------------
// Public C API
// -----------------------------------------------------------------------------
int basis_compress_file(const char* input_filename,
                        const char* output_filename,
                        unsigned int textureFlags,
                        int mip_smallest_dim,
                        int quality_level,
                        int effort_level,
                        bool multithreaded)
{
    if (basis_encoder_init() != 0) {
        return -100;
    }
    if (input_filename == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compression failed for %s input_filename null", output_filename);
        return 0;
    }
    // Check if this is a metallic/roughness texture (bit 1 set)
    int isMetallicRoughness = (textureFlags & 2) != 0;
    int isNormal = (textureFlags & 1) != 0;
    unsigned int flags = 0;
    if (isMetallicRoughness)
        flags |= BASIS_FORMAT_ETC1S;   // Use ETC1S for metallic/roughness (higher compression)
    else
        flags |= BASIS_FORMAT_UASTC;   // Use UASTC for other textures (higher quality)

    if (isNormal)
        flags |= BASIS_FLAG_NORMAL_MAP;
    if (isMetallicRoughness)
        flags |= BASIS_FLAG_METALLIC_ROUGHNESS;

    basis_compressor_params params;
    if (!setup_params(params, input_filename, output_filename, flags,
                      mip_smallest_dim, quality_level, effort_level, multithreaded)) {
        return -1;  // parameter setup failed
    }

    // Create a job pool for threading (optional but recommended)
    job_pool jpool(params.m_multithreading ? get_worker_count() : 1);
    params.m_pJob_pool = &jpool;

    basis_compressor compressor;
    if (!compressor.init(params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compressor init failed for %s", input_filename);
        return -2;
    }

    basis_compressor::error_code err = compressor.process();
    if (err != basis_compressor::cECSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compression failed for %s with error %d", input_filename, (int)err);
        return -3;
    }

    return 0; // success
}

int basis_compress_array_memory(const unsigned char* const* layer_mips,
                                int num_layers,
                                int num_mips,
                                int width,
                                int height,
                                unsigned int flags,
                                int quality_level,
                                int effort_level,
                                const char* output_filename)
{
    if (basis_encoder_init() != 0) {
        return -100;
    }
    if (!layer_mips || num_layers < 1 || num_mips < 1 || width < 1 || height < 1 || !output_filename) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_array_memory: invalid arguments for %s",
                output_filename ? output_filename : "(null)");
        return -1;
    }

    basis_compressor_params params;
    params.clear();
    params.m_out_filename = output_filename;
    params.m_write_output_basis_or_ktx2_files = true;
    params.m_create_ktx2_file = false;
    params.m_tex_type = basist::cBASISTexType2DArray;

    // mips are caller supplied, never generated, and the data keeps the channel layout
    // it had inside the source basis files: no swizzling, no renormalization
    params.m_mip_gen = false;
    params.m_y_flip = false;
    params.m_renormalize = false;

    bool linear = (flags & (BASIS_FLAG_NORMAL_MAP | BASIS_FLAG_METALLIC_ROUGHNESS | BASIS_FLAG_LINEAR)) != 0;
    params.m_perceptual = !linear;
    params.m_ktx2_and_basis_srgb_transfer_function = !linear;
    params.m_mip_srgb = !linear;

    if (!setup_format_params(params, flags, quality_level, effort_level))
        return -1;

    for (int layer = 0; layer < num_layers; layer++) {
        const unsigned char* mip0 = layer_mips[(size_t)layer * num_mips];
        if (!mip0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_array_memory: layer %d mip 0 is null", layer);
            return -1;
        }
        image img(width, height);
        MemCopy(img.get_ptr(), mip0, (size_t)width * height * 4);
        params.m_source_images.push_back(img);

        basisu::vector<image> mips;
        for (int m = 1; m < num_mips; m++) {
            const unsigned char* data = layer_mips[(size_t)layer * num_mips + m];
            int mw = width >> m;  if (mw < 1) mw = 1;
            int mh = height >> m; if (mh < 1) mh = 1;
            if (!data) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_array_memory: layer %d mip %d is null", layer, m);
                return -1;
            }
            image mip(mw, mh);
            MemCopy(mip.get_ptr(), data, (size_t)mw * mh * 4);
            mips.push_back(mip);
        }
        if (num_mips > 1)
            params.m_source_mipmap_images.push_back(mips);
    }

    params.m_multithreading = true;
    params.m_status_output = true;
    params.m_print_stats = false;

    job_pool jpool(get_worker_count());
    params.m_pJob_pool = &jpool;

    basis_compressor compressor;
    if (!compressor.init(params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compressor init failed for %s", output_filename);
        return -2;
    }

    basis_compressor::error_code err = compressor.process();
    if (err != basis_compressor::cECSuccess) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Compression failed for %s with error %d", output_filename, (int)err);
        return -3;
    }

    return 0;
}

int basis_compress_texture_array_files(const char* const* paths,
                                       unsigned int count,
                                       int size,
                                       bool srgb,
                                       unsigned int flags,
                                       int quality_level,
                                       int effort_level,
                                       const char* output_filename)
{
    if (!paths || count < 1 || size < 1 || !output_filename) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_texture_array_files: invalid arguments");
        return -1;
    }

    if (basis_encoder_init() != 0) {
        return -100;
    }

    const int num_mips = (int)get_array_mip_count((uint32_t)size);
    const size_t pointer_count = (size_t)count * num_mips;
    unsigned char** layer_mips = (unsigned char**)SDL_calloc(pointer_count, sizeof(unsigned char*));
    if (!layer_mips) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_texture_array_files: pointer allocation failed");
        return -1;
    }

    int result = 0;
    for (unsigned int layer = 0; layer < count; layer++) {
        for (int mip = 0; mip < num_mips; mip++) {
            int mip_size = size >> mip;
            if (mip_size < 1) mip_size = 1;

            layer_mips[(size_t)layer * num_mips + mip] = (unsigned char*)SDL_malloc((size_t)mip_size * mip_size * 4u);
            if (!layer_mips[(size_t)layer * num_mips + mip]) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_compress_texture_array_files: mip allocation failed layer=%u mip=%d", layer, mip);
                result = -1;
                goto cleanup;
            }
        }

        if (!load_texture_array_layer_rgba(paths[layer], size, srgb, layer_mips[(size_t)layer * num_mips])) {
            result = -1;
            goto cleanup;
        }

        generate_texture_array_mips(layer_mips, (int)layer, num_mips, size, srgb);
    }

    result = basis_compress_array_memory((const unsigned char* const*)layer_mips,
                                         (int)count, num_mips, size, size,
                                         flags, quality_level, effort_level,
                                         output_filename);

cleanup:
    for (size_t i = 0; i < pointer_count; i++) {
        if (layer_mips[i]) SDL_free(layer_mips[i]);
    }
    SDL_free(layer_mips);
    return result;
}

Texture basis_load_texture_array(const char* input_filename,
                                 unsigned int flags,
                                 const char* label)
{
    Texture texture = {0};
    if (!input_filename) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: input filename null");
        return texture;
    }

    uint64_t file_size64 = FileSize(input_filename);
    if (!file_size64 || file_size64 > UINT32_MAX) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: invalid file size %s", input_filename);
        return texture;
    }

    char* basis_data = ReadAllFileAlloc(input_filename);
    if (!basis_data) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: failed reading %s", input_filename);
        return texture;
    }

    uint32_t file_size = (uint32_t)file_size64;
    basist::basisu_transcoder_init();

    basist::basisu_transcoder transcoder;
    basist::basisu_file_info file_info;
    basist::basisu_image_info image_info;
    if (!transcoder.get_file_info(basis_data, file_size, file_info) ||
        file_info.m_total_images < 1 ||
        !transcoder.start_transcoding(basis_data, file_size) ||
        !transcoder.get_image_info(basis_data, file_size, image_info, 0)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: invalid basis array %s", input_filename);
        FreeAllText(basis_data);
        return texture;
    }

    basist::basis_tex_format basis_format = transcoder.get_basis_tex_format(basis_data, file_size);
    SDL_GPUTextureFormat texture_format = choose_basis_array_format(basis_format, image_info.m_alpha_flag, flags);
    basist::transcoder_texture_format transcode_format = sdl_format_to_basis_format(texture_format);
    if (transcode_format == basist::transcoder_texture_format::cTFTotalTextureFormats) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: unsupported transcode format");
        FreeAllText(basis_data);
        return texture;
    }

    uint32_t mip_levels = basisu::minimum(image_info.m_total_levels, (uint32_t)BASISU_MAX_TRANSCODED_MIPS);
    SDL_GPUTextureCreateInfo tex_desc;
    SDL_memset(&tex_desc, 0, sizeof(tex_desc));
    tex_desc.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    tex_desc.format = texture_format;
    tex_desc.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_desc.width = image_info.m_width;
    tex_desc.height = image_info.m_height;
    tex_desc.layer_count_or_depth = file_info.m_total_images;
    tex_desc.num_levels = mip_levels;
    tex_desc.sample_count = SDL_GPU_SAMPLECOUNT_1;
    tex_desc.props = SDL_CreateProperties();

    if (label) {
        SDL_SetStringProperty(tex_desc.props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, label);
    }

    texture.handle = SDL_CreateGPUTexture(g_GPUDevice, &tex_desc);
    SDL_DestroyProperties(tex_desc.props);
    if (!texture.handle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: texture creation failed %s", input_filename);
        FreeAllText(basis_data);
        return texture;
    }

    bool ok = true;
    for (uint32_t layer = 0; layer < file_info.m_total_images; layer++) {
        basist::basisu_image_info layer_info;
        if (!transcoder.get_image_info(basis_data, file_size, layer_info, layer) ||
            layer_info.m_width != image_info.m_width ||
            layer_info.m_height != image_info.m_height ||
            layer_info.m_total_levels < mip_levels) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: layer mismatch %s layer=%u", input_filename, layer);
            ok = false;
            break;
        }

        if (!upload_basis_array_layer(texture.handle, &transcoder, basis_data, file_size,
                                      layer, mip_levels, transcode_format, texture_format)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_texture_array: layer upload failed %s layer=%u", input_filename, layer);
            ok = false;
            break;
        }
    }

    FreeAllText(basis_data);
    if (!ok) {
        SDL_ReleaseGPUTexture(g_GPUDevice, texture.handle);
        texture = {};
        return texture;
    }

    texture.width = (s32)image_info.m_width;
    texture.height = (s32)image_info.m_height;
    texture.format = texture_format;
    texture.mipLevels = mip_levels;
    texture.numLayers = file_info.m_total_images;
    texture.channels = 4;
    texture.type = 0;
    texture.buffer = NULL;
    texture.bufferSize = 0;
    return texture;
}

Texture basis_load_or_build_texture_array(const char* const* paths,
                                          unsigned int count,
                                          int size,
                                          bool srgb,
                                          const char* basis_filename,
                                          unsigned int flags,
                                          const char* label)
{
    Texture texture = {};
    if (!paths || count < 1 || size < 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "basis_load_or_build_texture_array: invalid arguments");
        return texture;
    }

    bool basis_ready = basis_filename && FileExist(basis_filename);
    if (!basis_ready && basis_filename) {
        int compress = basis_compress_texture_array_files(paths, count, size, srgb, flags, -1, -1, basis_filename);
        if (compress != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "basis array build failed: %s result=%d", basis_filename, compress);
        }
        basis_ready = compress == 0;
    }

    if (basis_ready) {
        texture = basis_load_texture_array(basis_filename, flags, label);
    }

    if (!texture.handle && basis_ready) {
        int compress = basis_compress_texture_array_files(paths, count, size, srgb, flags, -1, -1, basis_filename);
        if (compress != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "basis array rebuild failed: %s result=%d", basis_filename, compress);
        } else {
            texture = basis_load_texture_array(basis_filename, flags, label);
        }
    }

    if (!texture.handle) {
        texture = create_rgba_texture_array_from_files(paths, count, size, srgb, label);
    }

    return texture;
}
