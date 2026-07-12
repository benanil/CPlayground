#ifndef PLATFORM_C
#define PLATFORM_C

#include "Include/Platform.h"
#include "Include/Bitset.h"
#include "Include/Camera.h"
#include "Include/Memory.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_filesystem.h>

#if defined(_WIN32) || defined(_WIN64)
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
	#include "Windows/PlatformWindows.c"
	
	static void PlatformApplyRoundedWindowRegion(void);
	static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep);
#endif

#define STB_SPRINTF_IMPLEMENTATION
#include "Extern/stb/stb_sprintf.h"

PlatformContext PlatformCtx = {0};
extern Camera g_Camera;
extern SDL_Window* g_SDLWindow;

static ALIGNAS(SIMD_NUM_BYTES) u64 DownKeys[8];
static ALIGNAS(SIMD_NUM_BYTES) u64 LastKeys[8]; 
static ALIGNAS(SIMD_NUM_BYTES) u64 PressedKeys[8];
static ALIGNAS(SIMD_NUM_BYTES) u64 ReleasedKeys[8];
static SDL_Cursor* g_Cursors[wCursor_Count];
static wCursor g_CurrentCursor = wCursor_Count;

static void PlatformClearInputState(void)
{
    MemsetZero(DownKeys, sizeof(DownKeys));
    MemsetZero(LastKeys, sizeof(LastKeys));
    MemsetZero(PressedKeys, sizeof(PressedKeys));
    MemsetZero(ReleasedKeys, sizeof(ReleasedKeys));
    PlatformCtx.MouseDown = 0;
    PlatformCtx.MouseLast = 0;
    PlatformCtx.MousePressed = 0;
    PlatformCtx.MouseReleased = 0;
    PlatformCtx.TextInputLength = 0u;
    PlatformCtx.TextInput[0] = 0;
    PlatformCtx.TextKeyEventCount = 0u;
}

inline static s32 GetRealKey(s32 x)
{
    if (x & 0x40000000u) return SDLK_PLUSMINUS + x - 0x40000039u;
    if (x & 0x20000000u) return SDLK_PLUSMINUS + 0x122u + x - 0x20000001u;
    return x;
}

static void PlatformPushTextKeyEvent(s32 key, u16 mod)
{
    switch (key)
    {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_BACKSPACE: case SDLK_DELETE:
        case SDLK_RETURN: case SDLK_A: case SDLK_C: case SDLK_V: case SDLK_X:
            break;
        default:
            return;
    }

    if (PlatformCtx.TextKeyEventCount < (u32)ARRAY_SIZE(PlatformCtx.TextKeyEvents))
    {
        PlatformTextKeyEvent* event = &PlatformCtx.TextKeyEvents[PlatformCtx.TextKeyEventCount++];
        event->key = key;
        event->mod = mod;
    }
}

// Sokol event callback
void EventCallback(const SDL_Event* event) 
{
    switch (event->type) {
        case SDL_EVENT_KEY_DOWN: {
            s32 vk_code = GetRealKey(event->key.key);
            
            if (vk_code > 0 && vk_code < 512) BitsetSet(DownKeys, vk_code);
            else SDL_Log("unhandelled key code down: %x", vk_code);

            PlatformPushTextKeyEvent(event->key.key, (u16)event->key.mod);
            break;
        }
        case SDL_EVENT_KEY_UP: {
            s32 vk_code = GetRealKey(event->key.key);
            
            if (vk_code > 0 && vk_code < 512) BitsetReset(DownKeys, vk_code);
            else SDL_Log("unhandelled key code up: %x", vk_code);
            
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            PlatformCtx.MousePosX = event->motion.x;
            PlatformCtx.MousePosY = event->motion.y;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            PlatformCtx.MouseWheelDelta = event->wheel.y;
            break;
            
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            u32 button_flag = 1u << (event->button.button - 1u);
            PlatformCtx.MouseDown |= button_flag;
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
        {
            u32 len = (u32)SDL_strlen(event->text.text);
            u32 space = (u32)sizeof(PlatformCtx.TextInput) - PlatformCtx.TextInputLength - 1u;
            u32 copy = Minu32(len, space);
            if (copy > 0u)
            {
                MemCopy(PlatformCtx.TextInput + PlatformCtx.TextInputLength, event->text.text, copy);
                PlatformCtx.TextInputLength += copy;
                PlatformCtx.TextInput[PlatformCtx.TextInputLength] = 0;
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: 
        {
            u32 button_flag = 1u << (event->button.button - 1u);
            // Handle f64 click detection for left button
            if (button_flag == MouseButton_Left) {
                u64 current_time = SDL_GetPerformanceCounter();
                f32 time_diff = (current_time - PlatformCtx.LastClickTime) / (double)PlatformCtx.CPUFrequency;
                PlatformCtx.DoubleClicked = (time_diff < 0.4);
                PlatformCtx.SecondsSinceLastClick = 0.0f;
                PlatformCtx.LastClickTime = current_time;
            }
            PlatformCtx.MouseDown &= ~button_flag;
            
            break;
        }
        case  SDL_EVENT_WINDOW_RESIZED: {
            if ((event->window.data1 + event->window.data2) != 0)
                Camera_RecalculateProjection(&g_Camera, event->window.data1, event->window.data2);
            PlatformCtx.WindowWidth = event->window.data1;
            PlatformCtx.WindowHeight = event->window.data2;
            wApplyWindowShape();
            break;
        }
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            wApplyWindowShape();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            PlatformCtx.WindowFocused = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            PlatformCtx.WindowFocused = false;
            PlatformClearInputState();
            break;
        case SDL_EVENT_WINDOW_MOVED:
            PlatformCtx.WindowPosX = event->window.data1;
            PlatformCtx.WindowPosY = event->window.data2;
            break;
        case SDL_EVENT_QUIT:
            
            break;
            
        default:
            break;
    }
}

const char* ConcatWithTempPath(const char* added, size_t addedLen)
{
    char* prefPath = SDL_GetPrefPath("CEngine", APPLICATION_NAME);
    if (!prefPath) { AX_WARN("getting pref path failed!"); return NULL; }
    int len = StringLength(prefPath);
    char* path = ArenaAllocGlobal(4096);
    MemsetZero(path, 4096);
    SmallMemCpy(path, prefPath, len);
    SmallMemCpy(path + len, added, addedLen);
    return path;
}

void GetMousePos(float* x, float* y) { SDL_GetMouseState(x, y); }

void SetMousePos(float x, float y) {  SDL_WarpMouseGlobal(x, y); }

void wGetMouseWindowPos(float* x, float* y) {
    GetMousePos(x, y);
}

void wGetMonitorSize(int* width, int* height) 
{
    const SDL_DisplayMode* DM = SDL_GetCurrentDisplayMode(0);
    if (DM)
    {
        *width  = DM->w;
        *height = DM->h;
    }
    else
    {
        *width = 1920;
        *height = 1090;
    }
}

void SetMouseWindowPos(float x, float y)
{
    SDL_WarpMouseInWindow(g_SDLWindow, x, y);
}

u8 AnyKeyDown()          { return PopCount512(DownKeys) > 0; }
u8 GetKeyDown(s32 c)     { return BitsetGet(DownKeys    , GetRealKey(c) & 511); }
u8 GetKeyReleased(s32 c) { return BitsetGet(ReleasedKeys, GetRealKey(c) & 511); }
u8 GetKeyPressed(s32 c)  { return BitsetGet(PressedKeys , GetRealKey(c) & 511); }

// Mouse
f32 GetMouseWheelDelta()  { return PlatformCtx.MouseWheelDelta; }
u8 GetDoubleClicked()    { return PlatformCtx.DoubleClicked; }
u8 AnyMouseKeyDown()            { return PlatformCtx.MouseDown > 0; }
u8 GetMouseDown(s32 button)     { return !!(PlatformCtx.MouseDown     & button); }
u8 GetMouseReleased(s32 button) { return !!(PlatformCtx.MouseReleased & button); }
u8 GetMousePressed(s32 button)  { return !!(PlatformCtx.MousePressed  & button); }

void wSetCursor(wCursor cursor)
{
    if ((u32)cursor >= (u32)wCursor_Count) cursor = wCursor_Default;
    if (g_CurrentCursor == cursor) return;

    SDL_SystemCursor systemCursor = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (cursor)
    {
        case wCursor_ResizeEW:   systemCursor = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
        case wCursor_ResizeNS:   systemCursor = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
        case wCursor_ResizeNWSE: systemCursor = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
        case wCursor_ResizeNESW: systemCursor = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        case wCursor_Move:       systemCursor = SDL_SYSTEM_CURSOR_MOVE; break;
        case wCursor_Default:
        default:                 systemCursor = SDL_SYSTEM_CURSOR_DEFAULT; break;
    }

    if (!g_Cursors[cursor]) g_Cursors[cursor] = SDL_CreateSystemCursor(systemCursor);
    if (g_Cursors[cursor]) SDL_SetCursor(g_Cursors[cursor]);
    g_CurrentCursor = cursor;
}

u32 PlatformConsumeTextInput(char* dst, u32 capacity)
{
    if (!dst || capacity == 0u) return 0u;
    u32 count = Minu32(PlatformCtx.TextInputLength, capacity - 1u);
    if (count > 0u) MemCopy(dst, PlatformCtx.TextInput, count);
    dst[count] = 0;
    PlatformCtx.TextInputLength = 0u;
    PlatformCtx.TextInput[0] = 0;
    return count;
}

u32 PlatformConsumeTextKeyEvents(PlatformTextKeyEvent* dst, u32 capacity)
{
    u32 count = Minu32(PlatformCtx.TextKeyEventCount, capacity);
    if (dst && count > 0u) MemCopy(dst, PlatformCtx.TextKeyEvents, (size_t)count * sizeof(PlatformTextKeyEvent));
    PlatformCtx.TextKeyEventCount = 0u;
    return count;
}

void SetPressedAndReleasedKeys()
{
    AndNot512(ReleasedKeys, LastKeys, DownKeys);
    AndNot512(PressedKeys , DownKeys, LastKeys);

    // Mouse
    PlatformCtx.MouseReleased = PlatformCtx.MouseLast & ~PlatformCtx.MouseDown;
    PlatformCtx.MousePressed  = ~PlatformCtx.MouseLast & PlatformCtx.MouseDown;
}

void RecordLastKeys()
{
    MemCopy(LastKeys, DownKeys, sizeof(u64) * 8);
    // PlatformCtx.LastKeys  = PlatformCtx.DownKeys;
    PlatformCtx.MouseLast = PlatformCtx.MouseDown;
}

void wSetWindowSize(s32 width, s32 height)
{
    SDL_SetWindowSize(g_SDLWindow, width, height);
    PlatformCtx.WindowWidth = width;
    PlatformCtx.WindowHeight = height;
    wApplyWindowShape();
}

void wSetWindowPosition(s32 x, s32 y)
{
    SDL_SetWindowPosition(g_SDLWindow, x, y);
    PlatformCtx.WindowPosX = x;
    PlatformCtx.WindowPosY = y;
}

//  void FolderCallback(void *userdata, const char * const *filelist, int filter)
void wOpenFolder(const char* folderPath, SDL_DialogFileCallback callback)
{
    SDL_ShowOpenFolderDialog(callback, NULL, NULL, folderPath, false);
}

void wOpenFile(const char* filePath, SDL_DialogFileCallback callback)
{
    SDL_ShowOpenFileDialog(callback, NULL, NULL, NULL, 0, filePath, false);
}

void wApplyWindowShape(void)
{
    #ifdef PLATFORM_WINDOWS
    PlatformApplyRoundedWindowRegion();
    #endif
}

static void EnableConsoleColors(void)
{
    #if defined(_WIN32)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    #endif
}

f32 GetDeltaTime() 
{ 
    return PlatformCtx.DeltaTime; 
}

s64 TimeNow()
{
    return SDL_GetPerformanceCounter();
}

// time is nanoseconds
f32  TimeToSeconds(s64 t)     { return (double)t / (double)PlatformCtx.CPUFrequency; }
s64 TimeToMilliseconds(s64 t) { return Int64MulDiv(t, 1000, PlatformCtx.CPUFrequency); }
s64 TimeToMicroseconds(s64 t) { return Int64MulDiv(t, 1000000, PlatformCtx.CPUFrequency); }

void PlatformInit()
{
    #ifdef PLATFORM_WINDOWS
    SetUnhandledExceptionFilter(CrashHandler);
    #endif
    EnableConsoleColors();
    SDL_SetLogPriorities(2); //SDL_LOG_PRIORITY_VERBOSE);
    PlatformCtx.SecondsSinceLastClick = 0.0f;
    PlatformCtx.CPUFrequency          = SDL_GetPerformanceFrequency();
    PlatformCtx.StartupTime           = SDL_GetPerformanceCounter();
    PlatformCtx.LastTime              = PlatformCtx.StartupTime;
    PlatformCtx.FrameCount            = 0;
    PlatformCtx.WindowFocused         = true;
    wApplyWindowShape();
}

void PlatformUpdate()
{
    s64 now = TimeNow();
    s64 elapsed = now - PlatformCtx.LastTime;
    PlatformCtx.DeltaTime = Clampf64((double)(elapsed) / (double)PlatformCtx.CPUFrequency, 0.0, 1.0);
    PlatformCtx.LastTime  = now;
    SetPressedAndReleasedKeys();
    RecordLastKeys();
}

f32 TimeSinceStartup()
{
    return (double)(TimeNow() - PlatformCtx.StartupTime) / (double)PlatformCtx.CPUFrequency;
}

#endif // PLATFORM_C
