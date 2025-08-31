#ifndef CAMERA_DEFINED
#define CAMERA_DEFINED

#include "Math/Matrix.h"
#include "Platform.h"
//#include "sokol/sokol_app.h"

typedef struct Camera_
{
    Matrix4 projection;
    Matrix4 view;
    
    float verticalFOV;
    float nearClip;
    float farClip;

    Vec2i viewportSize, monitorSize;
       
    Vec3f position;
    Vec2f mouseOld;
    Vec3f targetPos;
    Vec3f Front, Right, Up;
 
    float pitch, yaw, senstivity;
    float speed;

    bool wasPressing;
 
    FrustumPlanes frustumPlanes;

    Matrix4 inverseProjection;
    Matrix4 inverseView;
} Camera;


static inline void Camera_RecalculateProjection(Camera* camera, int width, int height)
{
    camera->viewportSize.x = width; camera->viewportSize.y = height;
    camera->projection = PerspectiveFovRH(camera->verticalFOV * MATH_DegToRad, (float)width, (float)height, camera->nearClip, camera->farClip);
    camera->inverseProjection = Matrix4Inverse(camera->projection);
}

static inline void Camera_RecalculateView(Camera* camera)
{
    camera->view = LookAtRH(camera->position, camera->Front, camera->Up);
    camera->inverseView = Matrix4Inverse(camera->view);
}

static inline void Camera_CalculateLook(Camera* camera) // from yaw pitch
{
    camera->Front.x = Cos(camera->yaw * MATH_DegToRad) * Cos(camera->pitch * MATH_DegToRad);
    camera->Front.y = Sin(camera->pitch * MATH_DegToRad);
    camera->Front.z = Sin(camera->yaw * MATH_DegToRad) * Cos(camera->pitch * MATH_DegToRad);
    camera->Front = Vec3Norm(camera->Front);
    // also re-calculate the Right and Up vector
    // normalize the vectors, because their length gets closer to 0 the more you look up or down which results in slower movement.
    camera->Right = Vec3NormEst(Vec3Cross(camera->Front, Vec3Up()));
    camera->Up = Vec3Cross(camera->Right, camera->Front);
}

static inline RayV ScreenPointToRay(Camera* camera, Vec2f pos)
{
    Vec2f coord = (Vec2f){ pos.x / (float)camera->viewportSize.x, pos.y / (float)camera->viewportSize.y };
    coord.y = 1.0f - coord.y;    // Flip Y to match the NDC coordinate system
    coord = Vec2SubF(Vec2MulF(coord, 2.0f), 1.0f); // Map to range [-1, 1]

    Vector4x32f clipSpacePos = VecSetR(coord.x, coord.y, 1.0f, 1.0f);
    Vector4x32f viewSpacePos = Vector4Transform(clipSpacePos, camera->inverseProjection.r);
    viewSpacePos = VecDiv(viewSpacePos, VecSplatW(viewSpacePos));
        
    Vector4x32f worldSpacePos = Vector4Transform(viewSpacePos, camera->inverseView.r);
    Vector4x32f rayDir = Vec3NormV(VecSub(worldSpacePos, VecLoad(&camera->position.x)));
        
    RayV ray;
    ray.origin = VecLoad(&camera->position.x); 
    ray.direction = rayDir;
    return ray;
}

static inline void CameraInit(Camera* camera)
{
    Camera_CalculateLook(camera);
    Camera_RecalculateView(camera);
    Camera_RecalculateProjection(camera, 800, 600);
}


static inline void InfiniteMouse(Vec2f point)
{
    Vec2i monitorSize;
    wGetMonitorSize(&monitorSize.x, &monitorSize.y);
    
    #ifndef __ANDROID__
    if (point.x > monitorSize.x - 2) SetCursorPos(3, (int)point.y);
    if (point.y > monitorSize.y - 2) SetCursorPos((int)point.x, 3);
        
    if (point.x < 2) SetCursorPos(monitorSize.x - 3, (int)point.y);
    if (point.y < 2) SetCursorPos((int)point.x, monitorSize.y - 3);
    #endif
}

static inline void CameraUpdate(Camera* camera, float dt)
{
    bool pressing = GetMouseDown(MouseButton_Right);
    float speed = dt * (1.0f + GetKeyDown(SAPP_KEYCODE_LEFT_SHIFT) * 2.0f) * 5.0f;

    if (!pressing) { camera->wasPressing = false; return; }
        
    Vec2f mousePos;
    GetMousePos(&mousePos.x, &mousePos.y);
    Vec2f diff = Vec2Sub(mousePos, camera->mouseOld);
    sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
        
    // if platform is android left side is for movement, right side is for rotating camera
    #ifdef __ANDROID__
    if (mousePos.x > (monitorSize.x / 2.0f))
        #endif
    {
        if (camera->wasPressing && diff.x + diff.y < 130.0f)
        {
            camera->pitch -= diff.y * dt * camera->senstivity;
            camera->yaw   += diff.x * dt * camera->senstivity;
            camera->yaw   = FModf(camera->yaw + 180.0f, 360.0f) - 180.0f;
            camera->pitch = MCLAMP(camera->pitch, -89.0f, 89.0f);
        }
        Camera_CalculateLook(camera);
    }
    #ifdef __ANDROID__
    else if (camera->wasPressing && diff.x + diff.y < 130.0f)
        camera->position += (camera->Right * diff.x * 0.02f) + (camera->Front * -diff.y * 0.02f);
    #endif  

    camera->mouseOld = mousePos;
    camera->wasPressing = true;

    if (GetKeyDown(SAPP_KEYCODE_D)) camera->position = Vec3Add(camera->position, Vec3MulF(camera->Right, camera->speed));
    if (GetKeyDown(SAPP_KEYCODE_A)) camera->position = Vec3Sub(camera->position, Vec3MulF(camera->Right, camera->speed));
    if (GetKeyDown(SAPP_KEYCODE_W)) camera->position = Vec3Add(camera->position, Vec3MulF(camera->Front, camera->speed));
    if (GetKeyDown(SAPP_KEYCODE_S)) camera->position = Vec3Sub(camera->position, Vec3MulF(camera->Front, camera->speed));
    if (GetKeyDown(SAPP_KEYCODE_E)) camera->position = Vec3Add(camera->position, Vec3MulF(camera->Up, camera->speed));
    if (GetKeyDown(SAPP_KEYCODE_Q)) camera->position = Vec3Sub(camera->position, Vec3MulF(camera->Up, camera->speed));

    InfiniteMouse(mousePos);
    Camera_RecalculateView(camera);
    // frustumPlanes = CreateFrustumPlanes(view * projection);
}

#endif