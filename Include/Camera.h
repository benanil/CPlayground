#ifndef CAMERA_DEFINED
#define CAMERA_DEFINED

#include "../Math/Matrix.h"
#include "Platform.h"
#include <SDL3/SDL_keycode.h>

//#include "sokol/sokol_app.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct Camera_
{
    mat4x4 projection;
    mat4x4 view;
    
    f32 verticalFOV;
    f32 nearClip;
    f32 farClip;

    int2 viewportSize, monitorSize;
       
    float3 position;
    float2 mouseOld;
    float3 targetPos;
    float3 Front, Right, Up;
 
    f32 pitch, yaw, senstivity;
    f32 speed;

    u8 wasPressing;
    u8 captured; // owns the active right-drag; only set when the press began over the 3d scene

    FrustumPlanes frustumPlanes;

    mat4x4 inverseProjection;
    mat4x4 inverseView;
} Camera;

static inline f32 Camera_SanitizeF32(f32 value, f32 fallback)
{
    return IsFiniteF32(value) ? value : fallback;
}

static inline void Camera_SanitizeConfig(Camera* camera)
{
    camera->verticalFOV = MCLAMP(Camera_SanitizeF32(camera->verticalFOV, 65.0f), 1.0f, 179.0f);
    camera->nearClip    = MMAX(Camera_SanitizeF32(camera->nearClip, 0.1f), 0.001f);
    camera->farClip     = Camera_SanitizeF32(camera->farClip, 30000.0f);
    if (camera->farClip <= camera->nearClip + 0.001f) camera->farClip = camera->nearClip + 1.0f;
    camera->speed      = MMAX(Camera_SanitizeF32(camera->speed, 1.0f), 0.0f);
    camera->senstivity = MMAX(Camera_SanitizeF32(camera->senstivity, 15.0f), 0.0f);
}

static inline void Camera_RecalculateProjection(Camera* camera, s32 width, s32 height)
{
    if (!camera) return;
    Camera_SanitizeConfig(camera);
    width = MMAX(width, 1);
    height = MMAX(height, 1);
    camera->viewportSize.x = width; camera->viewportSize.y = height;
    camera->projection = PerspectiveFovRH_ReverseZ(camera->verticalFOV * MATH_DegToRad, (f32)width, (f32)height, camera->nearClip, camera->farClip);
    camera->inverseProjection = M44Inverse(camera->projection);
}

static inline void Camera_RecalculateView(Camera* camera)
{
    if (!camera) return;
    camera->view = M44LookAtRH(camera->position, camera->Front, camera->Up);
    camera->inverseView = M44Inverse(camera->view);
}

static inline void Camera_CalculateLook(Camera* camera) // from yaw pitch
{
    if (!camera) return;
    camera->pitch = MCLAMP(Camera_SanitizeF32(camera->pitch, 0.0f), -89.0f, 89.0f);
    camera->yaw = Camera_SanitizeF32(camera->yaw, 0.0f);
    camera->yaw = FModf(camera->yaw + 180.0f, 360.0f) - 180.0f;

    camera->Front.x = Cos(camera->yaw * MATH_DegToRad) * Cos(camera->pitch * MATH_DegToRad);
    camera->Front.y = Sin(camera->pitch * MATH_DegToRad);
    camera->Front.z = Sin(camera->yaw * MATH_DegToRad) * Cos(camera->pitch * MATH_DegToRad);
    camera->Front   = F3NormSafe(camera->Front);
    // also re-calculate the Right and Up vector
    // normalize the vectors, because their length gets closer to 0 the more you look up or down which results in slower movement.
    float3 worldUp = { 0.0f, 1.0f, 0.0f };
    camera->Right = F3NormSafe(F3Cross(&camera->Front, &worldUp));
    camera->Up = F3NormSafe(F3Cross(&camera->Right, &camera->Front));
}

static inline RayV ScreenPointToRay(Camera* camera, float2 pos)
{
    RayV ray;
    if (!camera || camera->viewportSize.x <= 0 || camera->viewportSize.y <= 0)
    {
        ray.origin = VecZero();
        ray.dir = VecSetR(0.0f, 0.0f, 1.0f, 0.0f);
        return ray;
    }

    float2 coord = (float2){ pos.x / (f32)camera->viewportSize.x, pos.y / (f32)camera->viewportSize.y };
    coord.y = 1.0f - coord.y;    // Flip Y to match the NDC coordinate system
    coord = F2SubF(F2MulF(coord, 2.0f), 1.0f); // Map to range [-1, 1]

    v128f clipSpacePos = VecSetR(coord.x, coord.y, 1.0f, 1.0f);
    v128f viewSpacePos = Vec4Transform(clipSpacePos, camera->inverseProjection.r);
    f32 w = VecGetW(viewSpacePos);
    if (w > -MATH_Epsilon && w < MATH_Epsilon)
    {
        ray.origin = VecLoad(&camera->position.x);
        ray.dir = VecLoad(&camera->Front.x);
        return ray;
    }
    viewSpacePos = VecDiv(viewSpacePos, VecSet1(w));
         
    v128f worldSpacePos = Vec4Transform(viewSpacePos, camera->inverseView.r);
    v128f toWorld = VecSub(worldSpacePos, VecLoad(&camera->position.x));
    v128f rayDir = Vec3DotfV(toWorld, toWorld) > MATH_Epsilon ? Vec3NormV(toWorld) : VecLoad(&camera->Front.x);
         
    ray.origin = VecLoad(&camera->position.x); 
    ray.dir = rayDir;
    return ray;
}

static inline void CameraInit(Camera* camera, int width, int height)
{
    if (!camera) return;
    MemsetZero(camera, sizeof(Camera));
    camera->pitch       = 0.0f;
    camera->yaw         = 0.0f;
    camera->senstivity  = 15.0f;
    camera->verticalFOV = 65.0f;
    camera->nearClip    = 0.1f;
    camera->farClip     = 2500.0f; // minimum = (sum(cascadeSizes))
    camera->speed       = 3.0f;
    camera->position.x -= 6;
    wGetMonitorSize(&camera->monitorSize.x, &camera->monitorSize.y);

    Camera_CalculateLook(camera);
    Camera_RecalculateView(camera);
    Camera_RecalculateProjection(camera, width, height);
}

static inline float2 InfiniteMouse(float2 point)
{
    int2 monitorSize;
    wGetMonitorSize(&monitorSize.x, &monitorSize.y);
    if (monitorSize.x <= 32 || monitorSize.y <= 32) return point;
    
    if (point.x > monitorSize.x - 2) { point.x = 3.0f;                    SDL_WarpMouseGlobal((int)point.x, (int)point.y); }
    if (point.y > monitorSize.y - 2) { point.y = 3.0f;                    SDL_WarpMouseGlobal((int)point.x, (int)point.y); }
    if (point.x < 2)                 { point.x = (f32)(monitorSize.x - 3); SDL_WarpMouseGlobal((int)point.x, (int)point.y); }
    if (point.y < 2)                 { point.y = (f32)(monitorSize.y - 3); SDL_WarpMouseGlobal((int)point.x, (int)point.y); }
    return point;
}

static inline void CameraUpdate(Camera* camera, f32 dt, bool canCapture)
{
    if (!camera) return;
    Camera_SanitizeConfig(camera);
    dt = MMAX(Camera_SanitizeF32(dt, 0.0f), 0.0f);

    bool pressing = GetMouseDown(MouseButton_Right);
    f32 speed = dt * (1.0f + GetKeyDown(SDLK_LSHIFT) * 6.0f) * camera->speed;

    if (!pressing)
    {
        wSetCursor(wCursor_Default);
        GetMousePos(&camera->mouseOld.x, &camera->mouseOld.y);
        camera->wasPressing = false;
        camera->captured = false;
        return;
    }

    // a right-drag may only begin while the cursor is over the 3d scene, never over an
    // editor panel (that press belongs to the context menu). once the camera owns the
    // drag it keeps it until release, so sweeping over a panel mid-rotation still works.
    // gating here also stops InfiniteMouse from warping the cursor on a ui right-click.
    if (!camera->captured)
    {
        if (!canCapture)
        {
            GetMousePos(&camera->mouseOld.x, &camera->mouseOld.y);
            camera->wasPressing = false;
            return;
        }
        camera->captured = true;
    }

    float2 mousePos;
    GetMousePos(&mousePos.x, &mousePos.y);
    float2 diff = F2Sub(mousePos, camera->mouseOld);
    
    wSetCursor(wCursor_Move);
         
    // if platform is android left side is for movement, right side is for rotating camera
    #ifdef __ANDROID__
    wGetMonitorSize(&camera->monitorSize.x, &camera->monitorSize.y);
    if (mousePos.x > (camera->monitorSize.x / 2.0f))
    #endif
    {
        if (camera->wasPressing && F2LenSq(diff) < 130.0f * 130.0f)
        {
            camera->pitch -= diff.y * dt * camera->senstivity;
            camera->yaw   += diff.x * dt * camera->senstivity;
            camera->yaw   = FModf(camera->yaw + 180.0f, 360.0f) - 180.0f;
            camera->pitch = MCLAMP(camera->pitch, -89.0f, 89.0f);
        }
        Camera_CalculateLook(camera);
    }
    #ifdef __ANDROID__
    else if (camera->wasPressing && F2LenSq(diff) < 130.0f * 130.0f)
        camera->position += (camera->Right * diff.x * 0.02f) + (camera->Front * -diff.y * 0.02f);
    #endif  

    camera->wasPressing = true;

    if (GetKeyDown(SDLK_D)) camera->position = F3Add(camera->position, F3MulF(camera->Right, speed));
    if (GetKeyDown(SDLK_A)) camera->position = F3Sub(camera->position, F3MulF(camera->Right, speed));
    if (GetKeyDown(SDLK_W)) camera->position = F3Add(camera->position, F3MulF(camera->Front, speed));
    if (GetKeyDown(SDLK_S)) camera->position = F3Sub(camera->position, F3MulF(camera->Front, speed));
    if (GetKeyDown(SDLK_E)) camera->position = F3Add(camera->position, F3MulF(camera->Up, speed));
    if (GetKeyDown(SDLK_Q)) camera->position = F3Sub(camera->position, F3MulF(camera->Up, speed));

    if (GetKeyReleased(SDLK_R))
    {
        SDL_Log("cam pos: %f, %f, %f", camera->position.x, camera->position.y, camera->position.z);
    }

    camera->mouseOld = InfiniteMouse(mousePos);
    Camera_RecalculateView(camera);
    // frustumPlanes updated unconditionally in Rendering.c (Render()) using the RevZ variant
}

#if defined(__cplusplus)
}
#endif

#endif
