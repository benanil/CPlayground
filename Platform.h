#ifndef PLATFORM_H
#define PLATFORM_H

// enables logging no matter what
// #define AX_ENABLE_LOGGING

#if defined(AX_ENABLE_LOGGING) || defined(_DEBUG) || defined(DEBUG) || defined(Debug)
#ifdef __ANDROID__
    #include <android/log.h>
    #define AX_ERROR(format, ...) { __android_log_print(ANDROID_LOG_FATAL, "AX-FATAL", "%s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__); ASSERT(0);}
    #define AX_LOG(format, ...)    __android_log_print(ANDROID_LOG_INFO, "AX-INFO", "%s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__)
    #define AX_WARN(format, ...)   __android_log_print(ANDROID_LOG_WARN, "AX-WARN", "%s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__)
#else
    void FatalError(const char* format, ...); // defined in PlatformBla.cpp
    void DebugLog(const char* format, ...); 

    #define AX_LOG(format, ...)  DebugLog("axInfo: %s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__)
    #define AX_WARN(format, ...) DebugLog("axWarn: %s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__)

    #if !(defined(__GNUC__) || defined(__GNUG__))
    #   define AX_ERROR(format, ...) FatalError("%s -line:%i " format, GetFileName(__FILE__), __LINE__, ##__VA_ARGS__)
    #else                                                             
    #   define AX_ERROR(format, ...) FatalError("%s -line:%i " format, GetFileName(__FILE__), __LINE__,##__VA_ARGS__)
    #endif
#endif
#else
    #define AX_ERROR(format, ...)
    #define AX_LOG(format, ...)  
    #define AX_WARN(format, ...) 
#endif

#include "Bitset.h"
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
    Bitset DownKeys, LastKeys, PressedKeys, ReleasedKeys;
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

void PlatformInit();

extern PlatformContext PlatformCtx;


#endif // PLATFORM_H