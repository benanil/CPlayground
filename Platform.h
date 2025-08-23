#ifndef PLATFORM_H
#define PLATFORM_H

#include "Bitset128.h"
// #include "sokol_app.h"

// Platform context structure
typedef struct PlatformContext_ 
{
    // Mouse state
    float MousePosX, MousePosY;
    float MouseWheelDelta;
    float SecondsSinceLastClick;
    float DeltaTime;
    uint64_t LastClickTime;
    uint64_t StartupTime;
    
    // Window state
    int WindowWidth, WindowHeight;
    int WindowPosX, WindowPosY;
    
    // Keyboard state
    BitSet128 DownKeys, LastKeys, PressedKeys, ReleasedKeys;
    int MouseDown, MouseLast, MousePressed, MouseReleased;
    bool DoubleClicked;
    
} PlatformContext;


// Mouse button flags
typedef enum MouseButton_ {
    MouseButton_Left     = 1 << 0,
    MouseButton_Right    = 1 << 1, 
    MouseButton_Middle   = 1 << 2,
    MouseButton_Forward  = 1 << 3,
    MouseButton_Backward = 1 << 4
} MouseButton;


void GetMousePos(float* x, float* y);

void SetMousePos(float x, float y);

void wGetMouseWindowPos(float* x, float* y);

void wGetMonitorSize(int* width, int* height);

void SetMouseWindowPos(float x, float y);

bool AnyKeyDown();
bool GetKeyDown(char c);
bool GetKeyReleased(char c);
bool GetKeyPressed(char c);

float GetMouseWheelDelta();
bool GetDoubleClicked();
bool AnyMouseKeyDown();
bool GetMouseDown(uint32_t button);
bool GetMouseReleased(uint32_t button);
bool GetMousePressed(uint32_t button);

void wSetWindowSize(int width, int height);

void wSetWindowPosition(int x, int y);

bool wOpenFolder(const char* folderPath);

bool wOpenFile(const char* filePath);

double GetDeltaTime();

double TimeSinceStartup();

extern PlatformContext PlatformCtx;


#endif // PLATFORM_H