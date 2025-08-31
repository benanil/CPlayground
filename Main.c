
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_LOG_IMPL
#define SOKOL_GLUE_IMPL
#define SOKOL_D3D11
#define SOKOL_TIME_IMPL

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION

#define STBI_MALLOC(size)           ( rpmalloc(size) )
#define STBI_FREE(ptr)              ( rpfree(ptr) )
#define STBI_REALLOC(ptr, size)     ( rprealloc(ptr, size) )

#define STBIR_MALLOC(size, c)       ( rpmalloc(size) )
#define STBIR_FREE(ptr, c)          ( (void)(c), rpfree(ptr) )

#ifndef NOMINMAX
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN 
#  define VC_EXTRALEAN
#endif

#include "Extern/rpmalloc.c"

#include "Extern/sokol/sokol_gfx.h"
#include "Extern/sokol/sokol_app.h"
#include "Extern/sokol/sokol_log.h"
#include "Extern/sokol/sokol_glue.h"
#include "Extern/sokol/sokol_time.h"

#include "Extern/stb/stb_image.h"

#include "Extern/zstd.c"
#include "Extern/ufbx.c"
#include "Extern/dynarray.c"

#include "Math/Matrix.h"
#include "Camera.h"
#include "Bitset.h"

#include "Shaders/Cube.glsl.h"

#include "Platform.c"
#include "Graphics.c"
#include "GLTFParser.c"
#include "Animation.c"
#include "AssetManager.c"

static struct {
    float rx, ry;
    sg_pipeline pip;
    sg_bindings bind;
} state;

typedef struct {
    float x, y, z;
    uint32_t color;
    int16_t u, v;
} vertex_t;

static Camera camera;

static void _sapp_setup_wave_icon(void);

void Init(void) 
{
    _sapp_setup_wave_icon();
    PlatformInit();
    rInit();
    stm_setup();
    PlatformCtx.StartupTime = stm_now();
    MemsetZero(&camera, sizeof(camera));
    camera.pitch = 0.0f;
    camera.yaw = -0.0f;
    camera.senstivity = 15.0f;
    camera.verticalFOV = 65.0f;
    camera.nearClip = 0.1f;
    camera.farClip = 2400.0f;
    camera.speed = 0.4f;
    camera.position.x -= 6;

    CameraInit(&camera);

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });


    Texture img = rImportTexture("Test.jpg", TexFlags_MipMap, "Test Tex");

    vertex_t vertices[] = {
        /* pos                  color       uvs */
        { -1.0f, -1.0f, -1.0f,  0xFF0000FF,     0,     0 },
        {  1.0f, -1.0f, -1.0f,  0xFF0000FF, 32767,     0 },
        {  1.0f,  1.0f, -1.0f,  0xFF0000FF, 32767, 32767 },
        { -1.0f,  1.0f, -1.0f,  0xFF0000FF,     0, 32767 },

        { -1.0f, -1.0f,  1.0f,  0xFF00FF00,     0,     0 },
        {  1.0f, -1.0f,  1.0f,  0xFF00FF00, 32767,     0 },
        {  1.0f,  1.0f,  1.0f,  0xFF00FF00, 32767, 32767 },
        { -1.0f,  1.0f,  1.0f,  0xFF00FF00,     0, 32767 },

        { -1.0f, -1.0f, -1.0f,  0xFFFF0000,     0,     0 },
        { -1.0f,  1.0f, -1.0f,  0xFFFF0000,     0, 32767 },
        { -1.0f,  1.0f,  1.0f,  0xFFFF0000, 32767, 32767 },
        { -1.0f, -1.0f,  1.0f,  0xFFFF0000, 32767, 0},

        {  1.0f, -1.0f, -1.0f,  0xFFFF007F,     0,     0 },
        {  1.0f,  1.0f, -1.0f,  0xFFFF007F,     0, 32767 },
        {  1.0f,  1.0f,  1.0f,  0xFFFF007F, 32767, 32767 },
        {  1.0f, -1.0f,  1.0f,  0xFFFF007F, 32767, 0     },

        { -1.0f, -1.0f, -1.0f,  0xFFFF7F00,     0,     0 },
        { -1.0f, -1.0f,  1.0f,  0xFFFF7F00, 32767,     0 },
        {  1.0f, -1.0f,  1.0f,  0xFFFF7F00, 32767, 0    },
        {  1.0f, -1.0f, -1.0f,  0xFFFF7F00,     0, 0   },

        { -1.0f,  1.0f, -1.0f,  0xFF007FFF, 0    , 32767 },
        { -1.0f,  1.0f,  1.0f,  0xFF007FFF, 0    ,32767},
        {  1.0f,  1.0f,  1.0f,  0xFF007FFF, 32767, 32767 },
        {  1.0f,  1.0f, -1.0f,  0xFF007FFF,     0, 32767 },
    };

    sg_buffer vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = SG_RANGE(vertices),
        .label = "cube-vertices"
    });

    /* create an index buffer for the cube */
    uint16_t indices[] = {
        0, 1, 2,  0, 2, 3,
        6, 5, 4,  7, 6, 4,
        8, 9, 10,  8, 10, 11,
        14, 13, 12,  15, 14, 12,
        16, 17, 18,  16, 18, 19,
        22, 21, 20,  23, 22, 20
    };
    sg_buffer ibuf = sg_make_buffer(&(sg_buffer_desc){
        .usage.index_buffer = true,
        .data = SG_RANGE(indices),
        .label = "cube-indices"
    });

    // the first 4 samplers are just different min-filters
    sg_sampler_desc smp_desc = { .mag_filter = SG_FILTER_LINEAR, };

    smp_desc.min_lod = 0.0f;
    smp_desc.max_lod = 8;
    smp_desc.min_filter = SG_FILTER_LINEAR;
    smp_desc.mag_filter = SG_FILTER_LINEAR;
    smp_desc.mipmap_filter = SG_FILTER_LINEAR;
    smp_desc.max_anisotropy = 8;

    sg_sampler sampler = sg_make_sampler(&smp_desc);
    
    /* create shader */
    sg_shader shd = sg_make_shader(cube_shader_desc(sg_query_backend()));

    /* create pipeline object */
    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .layout = {
            .attrs = {
                [0].format = SG_VERTEXFORMAT_FLOAT3,
                [1].format = SG_VERTEXFORMAT_UBYTE4N,
                [2].format = SG_VERTEXFORMAT_SHORT2N
            }
        },
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode = SG_CULLMODE_BACK,
        .depth = {
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
            .write_enabled = true
        },
        .label = "texcube-pipeline"
    });

    /* setup resource bindings */
    state.bind = (sg_bindings) {
        .vertex_buffers[0] = vbuf,
        .samplers[0] = sampler,
        .images[0] = img.handle,
        .index_buffer = ibuf
    };
}

void Frame(void)
{
    /* NOTE: the vs_params_t struct has been code-generated by the shader-code-gen */
    vs_params_t vs_params;
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    const double dt = sapp_frame_duration();
    PlatformCtx.DeltaTime = dt;
    Matrix4 proj = PerspectiveFovRH(60.0f * (MATH_PI / 180.0f), w, h, 0.01f, 10.0f);
    Matrix4 view = LookAtRH((Vec3f){0.0f, 1.5f, 6.0f}, (Vec3f){0.0f, 0.0f, -1.0f}, (Vec3f){0.0f, 1.0f, 0.0f});
    Matrix4 view_proj = Matrix4Multiply(proj, view);
    state.rx += (float)dt; 
    state.ry += (float)dt;

    PlatformCtx.SecondsSinceLastClick += (float)dt;
    //Matrix4 rym = Matrix4RotationY(FModf(state.ry + MATH_PI, MATH_TwoPI) - MATH_PI);
    Matrix4 model = MatrixFromScalef(5.0f); // Matrix4Multiply(MatrixFromScalef(4.0f), rym); //(rxm, rym);

    CameraUpdate(&camera, (float)dt);

    view_proj = Matrix4Multiply(camera.view, camera.projection);
    vs_params.mvp = Matrix4Multiply(model, view_proj);

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { 0.25f, 0.5f, 0.75f, 1.0f }
            },
        },
        .swapchain = sglue_swapchain()
    });
    sg_apply_pipeline(state.pip);
    sg_apply_bindings(&state.bind);
    sg_apply_uniforms(UB_vs_params, &SG_RANGE(vs_params));
    sg_draw(0, 36, 1);
    sg_end_pass();
    sg_commit();
}

void Cleanup(void) {
    sg_shutdown();
    rDestroy();
    rpmalloc_finalize();
}

extern void sokol_event_callback(const sapp_event* event);

void* alloc_fn(size_t size, void* user_data)
{
    (void*)user_data;
    return rpmalloc(size);
}

void free_fn(void* ptr, void* user_data)
{
    (void*)user_data;
    rpfree(ptr);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = Init,
        .frame_cb = Frame,
        .cleanup_cb = Cleanup,
        .allocator.alloc_fn = alloc_fn,
        .allocator.free_fn = free_fn,
        .event_cb = sokol_event_callback,
        .width = 800,
        .height = 600,
        .sample_count = 4,
        .window_title = "CPlayground",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}


static void _sapp_setup_wave_icon(void)
{
    const uint32_t colors[8] = { 0xFF003300, 0xFF006600, 0xFF009900, 0xFF00CC00, 0xFF00FF00, 0xFF33FF33, 0xFF66FF66, 0xFF99FF99 };
    const int icon_sizes[3] = { 16, 32, 64 };
    uint32_t wave_icon_pixels[16 * 16 + 32 * 32 + 64 * 64] = {0};
    uint32_t* dst = wave_icon_pixels;
    
    sapp_icon_desc icon_desc = {0};
    for (int i = 0; i < 3; i++) {
        const int dim = icon_sizes[i];
        icon_desc.images[i] = (sapp_image_desc){dim, dim, {dst, dim * dim * sizeof(uint32_t)}};
        dst += dim * dim;
    }
    
    dst = wave_icon_pixels;
    for (int i = 0; i < 3; i++) {
        const int dim = icon_sizes[i];
        for (int y = 0; y < dim; y++) {
            for (int x = 0; x < dim; x++) {
                int wave_y = dim/2 + SinR(x * 6.28f / dim * 2.0f) * dim * 0.3f;
                *dst++ = (Absf(y - wave_y) <= 2) ? colors[x * 7 / dim] : 0x00FFFFFF;
            }
        }
    }
    sapp_set_icon(&icon_desc);
}