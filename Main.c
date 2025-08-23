
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_LOG_IMPL
#define SOKOL_GLUE_IMPL
#define SOKOL_D3D11
#define SOKOL_TIME_IMPL

#define STB_IMAGE_IMPLEMENTATION

#include "Extern/sokol/sokol_gfx.h"
#include "Extern/sokol/sokol_app.h"
#include "Extern/sokol/sokol_log.h"
#include "Extern/sokol/sokol_glue.h"
#include "Extern/sokol/sokol_time.h"

#include "Extern/stb/stb_image.h"

#include "Math/Matrix.h"
#include "Camera.h"
#include "Bitset128.h"

#include "Shaders/Cube.glsl.h"

#include "Platform.c"
#include "Graphics.c"

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

void init(void) 
{
    stm_setup();
    PlatformCtx.StartupTime = stm_now();
    MemsetZero(&camera, sizeof(camera));
    camera.pitch = 0.0f;
    camera.yaw = -9.0f;
    camera.senstivity = 15.0f;
    camera.verticalFOV = 65.0f;
    camera.nearClip = 0.1f;
    camera.farClip = 2400.0f;
    camera.speed = 0.4f;

    CameraInit(&camera);

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    int channels;
    sg_image_data img_data;
    sg_image_desc imageDesc = {};
    unsigned char* pixels = stbi_load("Test.jpg", &imageDesc.width, &imageDesc.height, &channels, 4);
    imageDesc.num_mipmaps = rGetMipmapImageData(&img_data, pixels, imageDesc.width, imageDesc.height);
    imageDesc.pixel_format = SG_PIXELFORMAT_RGBA8;
    imageDesc.data = img_data;

    sg_image image = sg_make_image(&imageDesc);

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
        { -1.0f,  1.0f, -1.0f,  0xFFFF0000, 32767,     0 },
        { -1.0f,  1.0f,  1.0f,  0xFFFF0000, 32767, 32767 },
        { -1.0f, -1.0f,  1.0f,  0xFFFF0000,     0, 32767 },

        {  1.0f, -1.0f, -1.0f,  0xFFFF007F,     0,     0 },
        {  1.0f,  1.0f, -1.0f,  0xFFFF007F, 32767,     0 },
        {  1.0f,  1.0f,  1.0f,  0xFFFF007F, 32767, 32767 },
        {  1.0f, -1.0f,  1.0f,  0xFFFF007F,     0, 32767 },

        { -1.0f, -1.0f, -1.0f,  0xFFFF7F00,     0,     0 },
        { -1.0f, -1.0f,  1.0f,  0xFFFF7F00, 32767,     0 },
        {  1.0f, -1.0f,  1.0f,  0xFFFF7F00, 32767, 32767 },
        {  1.0f, -1.0f, -1.0f,  0xFFFF7F00,     0, 32767 },

        { -1.0f,  1.0f, -1.0f,  0xFF007FFF,     0,     0 },
        { -1.0f,  1.0f,  1.0f,  0xFF007FFF, 32767,     0 },
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
    smp_desc.max_lod = imageDesc.num_mipmaps;
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
        .images[0] = image,
        .index_buffer = ibuf
    };
}

void frame(void) {
    /* NOTE: the vs_params_t struct has been code-generated by the shader-code-gen */
    vs_params_t vs_params;
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    const double dt = sapp_frame_duration();
    PlatformCtx.DeltaTime = dt;
    Matrix4 proj = PerspectiveFovRH(60.0f * (MATH_PI / 180.0f), w, h, 0.01f, 10.0f);
    Matrix4 view = LookAtRH((Vec3f){0.0f, 1.5f, 6.0f}, (Vec3f){0.0f, 0.0f, -1.0f}, (Vec3f){0.0f, 1.0f, 0.0f});
    Matrix4 view_proj = Matrix4Multiply(proj, view);
    state.rx += (float)dt; state.ry += (float)dt;


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

void cleanup(void) {
    sg_shutdown();
}

extern void sokol_event_callback(const sapp_event* event);

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = sokol_event_callback,
        .width = 800,
        .height = 600,
        .sample_count = 4,
        .window_title = "Cube (sokol-app)",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}