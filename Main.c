
#define HANDMADE_MATH_IMPLEMENTATION
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_LOG_IMPL
#define SOKOL_GLUE_IMPL
#define SOKOL_D3D11
#define SOKOL_TIME_IMPL

#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_time.h"

#include "Math/Matrix.h"
#include "Camera.h"
#include "Bitset128.h"

#include "Shaders/Cube.glsl.h"

static struct {
    float rx, ry;
    sg_pipeline pip;
    sg_bindings bind;
} state;

static Camera camera;

// Platform context structure
typedef struct PlatformContext_ 
{
    // Mouse state
    float MousePosX, MousePosY;
    float MouseWheelDelta;
    uint32_t MouseDown;
    bool DoubleClicked;
    float SecondsSinceLastClick;
    uint64_t LastClickTime;
    
    // Window state
    int WindowWidth, WindowHeight;
    int WindowPosX, WindowPosY;
    
    // Keyboard state
    BitSet128 DownKeys;
    
} PlatformContext;

static PlatformContext PlatformCtx = {0};

// Sokol event callback
void sokol_event_callback(const sapp_event* event) 
{
    switch (event->type) {
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            PlatformCtx.MousePosX = event->mouse_x;
            PlatformCtx.MousePosY = event->mouse_y;
            break;
        case SAPP_EVENTTYPE_MOUSE_SCROLL:
            PlatformCtx.MouseWheelDelta = event->scroll_y;
            break;
            
        case SAPP_EVENTTYPE_MOUSE_DOWN: {
            uint32_t button_flag = 1 << (event->mouse_button);
            PlatformCtx.MouseDown |= button_flag;
            break;
        }
        case SAPP_EVENTTYPE_MOUSE_UP: {
            uint32_t button_flag = 1 << (event->mouse_button);
            
            // Handle double click detection for left button
            if (button_flag == MouseButton_Left) {
                uint64_t current_time = stm_now();
                double time_diff = stm_sec(stm_diff(current_time, PlatformCtx.LastClickTime));
                PlatformCtx.DoubleClicked = (time_diff < 0.4);
                PlatformCtx.SecondsSinceLastClick = 0.0f;
                PlatformCtx.LastClickTime = current_time;
            }
            
            PlatformCtx.MouseDown &= ~button_flag;
            break;
        }
        case SAPP_EVENTTYPE_KEY_DOWN: {
            int vk_code = event->key_code;
            if (vk_code > 0 && vk_code < 128) {
                BitSet128_Set(&PlatformCtx.DownKeys, vk_code);
            }
            break;
        }
        case SAPP_EVENTTYPE_KEY_UP: {
            int vk_code = event->key_code;
            if (vk_code > 0 && vk_code < 128) {
                BitSet128_Clear(&PlatformCtx.DownKeys, vk_code);
            }
            break;
        }
        case SAPP_EVENTTYPE_QUIT_REQUESTED:
            sapp_request_quit();
            break;
            
        default:
            break;
    }
}


void GetMousePos(float* x, float* y)
{
    ASSERT((uint64_t)x & (uint64_t)y); // shouldn't be nullptr
    POINT point;
    GetCursorPos(&point);
    *x = (float)point.x;
    *y = (float)point.y;
}

void SetMousePos(float x, float y)
{
    SetCursorPos((int)x, (int)y);
}

void GetMouseWindowPos(float* x, float* y)
{
    *x = PlatformCtx.MousePosX; *y = PlatformCtx.MousePosY;
}

void SetMouseWindowPos(float x, float y)
{
    SetMousePos(PlatformCtx.WindowPosX + x, PlatformCtx.WindowPosY + y);
}

// Update function to handle time-based updates (call this each frame)
void platform_update(float dt) {
    PlatformCtx.SecondsSinceLastClick += dt;
}

// Utility functions to match your original API
bool is_key_down(int vk_code) {
    if (vk_code < 0 || vk_code >= 128) return false;
    return BitSet128_Test(&PlatformCtx.DownKeys, vk_code);
}

bool is_mouse_button_down(uint32_t button) {
    return (PlatformCtx.MouseDown & button) != 0;
}

void wGetMonitorSize(int* width, int* height)
{
    *width  = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
}

float get_mouse_x() { return PlatformCtx.MousePosX; }
float get_mouse_y() { return PlatformCtx.MousePosY; }
float get_mouse_wheel_delta() { return PlatformCtx.MouseWheelDelta; }
bool get_double_clicked() { return PlatformCtx.DoubleClicked; }

void init(void) 
{
    stm_setup();
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

    /* cube vertex buffer */
    float vertices[] = {
        -1.0, -1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
        1.0, -1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
        1.0,  1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
        -1.0,  1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
        
        -1.0, -1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
        1.0, -1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
        1.0,  1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
        -1.0,  1.0,  1.0,   0.0, 1.0, 0.0, 1.0,

        -1.0, -1.0, -1.0,   0.0, 0.0, 1.0, 1.0,
        -1.0,  1.0, -1.0,   0.0, 0.0, 1.0, 1.0,
        -1.0,  1.0,  1.0,   0.0, 0.0, 1.0, 1.0,
        -1.0, -1.0,  1.0,   0.0, 0.0, 1.0, 1.0,

        1.0, -1.0, -1.0,    1.0, 0.5, 0.0, 1.0,
        1.0,  1.0, -1.0,    1.0, 0.5, 0.0, 1.0,
        1.0,  1.0,  1.0,    1.0, 0.5, 0.0, 1.0,
        1.0, -1.0,  1.0,    1.0, 0.5, 0.0, 1.0,

        -1.0, -1.0, -1.0,   0.0, 0.5, 1.0, 1.0,
        -1.0, -1.0,  1.0,   0.0, 0.5, 1.0, 1.0,
        1.0, -1.0,  1.0,   0.0, 0.5, 1.0, 1.0,
        1.0, -1.0, -1.0,   0.0, 0.5, 1.0, 1.0,
        
        -1.0,  1.0, -1.0,   1.0, 0.0, 0.5, 1.0,
        -1.0,  1.0,  1.0,   1.0, 0.0, 0.5, 1.0,
        1.0,  1.0,  1.0,   1.0, 0.0, 0.5, 1.0,
        1.0,  1.0, -1.0,   1.0, 0.0, 0.5, 1.0
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

    /* create shader */
    sg_shader shd = sg_make_shader(cube_shader_desc(sg_query_backend()));

    /* create pipeline object */
    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .layout = {
            /* test to provide buffer stride, but no attr offsets */
            .buffers[0].stride = 28,
            .attrs = {
                [ATTR_cube_position].format = SG_VERTEXFORMAT_FLOAT3,
                [ATTR_cube_color0].format   = SG_VERTEXFORMAT_FLOAT4
            }
        },
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode = SG_CULLMODE_BACK,
        .depth = {
            .write_enabled = true,
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
        .label = "cube-pipeline"
    });

    /* setup resource bindings */
    state.bind = (sg_bindings) {
        .vertex_buffers[0] = vbuf,
        .index_buffer = ibuf
    };
}

void frame(void) {
    /* NOTE: the vs_params_t struct has been code-generated by the shader-code-gen */
    vs_params_t vs_params;
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    const float t = sapp_frame_duration();
	Matrix4 proj = PerspectiveFovRH(60.0f * (MATH_PI / 180.0f), w, h, 0.01f, 10.0f);
    Matrix4 view = LookAtRH((Vec3f){0.0f, 1.5f, 6.0f}, (Vec3f){0.0f, 0.0f, -1.0f}, (Vec3f){0.0f, 1.0f, 0.0f});
    Matrix4 view_proj = Matrix4Multiply(proj, view);
    state.rx += t; state.ry += t;

    Matrix4 rym = Matrix4RotationY(FModf(state.ry + MATH_PI, MATH_TwoPI) - MATH_PI);
    Matrix4 model = rym; //Matrix4Multiply(rxm, rym);

    CameraUpdate(&camera, t);

    view_proj = Matrix4Multiply(camera.projection, camera.view);
    vs_params.mvp = Matrix4Multiply(view_proj, model);

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