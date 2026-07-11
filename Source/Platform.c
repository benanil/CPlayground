#ifndef PLATFORM_C
#define PLATFORM_C


#if defined(_WIN32) || defined(_WIN64)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

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

#ifdef PLATFORM_WINDOWS
#include <DbgHelp.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#define WINDOW_CORNER_RADIUS 28

static void PlatformCrashLog(const char* format, ...)
{
    char message[2048];
    va_list args;
    va_start(args, format);
    SDL_vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, AX_ANSI_RED "crash: %s" AX_ANSI_RESET, message);

    HANDLE file = CreateFileA("crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written;
    char line[2200];
    int len = SDL_snprintf(line, sizeof(line), "crash: %s\r\n", message);
    if (len > 0)
        WriteFile(file, line, (DWORD)Mins32(len, (s32)sizeof(line) - 1), &written, NULL);
    CloseHandle(file);
}

#define PLATFORM_CRASH_LOG(format, ...) PlatformCrashLog(format, ##__VA_ARGS__)

static HWND PlatformGetWin32WindowHandle(void)
{
    if (!g_SDLWindow)
    {
        AX_WARN("missing SDL window for Win32 handle");
        return NULL;
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(g_SDLWindow);
    if (!props)
    {
        AX_WARN("getting SDL window properties failed: %s", SDL_GetError());
        return NULL;
    }

    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd) AX_WARN("getting Win32 window handle failed: %s", SDL_GetError());
    return hwnd;
}

static void PlatformApplyRoundedWindowRegion(void)
{
    HWND hwnd = PlatformGetWin32WindowHandle();
    if (!hwnd) return;
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

static void PrintCrashFrame(u32 idx, void* addr)
{
    HANDLE process = GetCurrentProcess();
    HMODULE module = NULL;
    char moduleName[MAX_PATH];
    moduleName[0] = '?'; moduleName[1] = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)addr, &module);
    if (module) GetModuleFileNameA(module, moduleName, sizeof(moduleName));
    uintptr_t rva = module ? (uintptr_t)addr - (uintptr_t)module : 0u;

    char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    const char* name = SymFromAddr(process, (DWORD64)(uintptr_t)addr, &displacement, symbol) ? symbol->Name : "?";

    IMAGEHLP_LINE64 line = {0};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(process, (DWORD64)(uintptr_t)addr, &lineDisplacement, &line))
    {
        PLATFORM_CRASH_LOG("  #%02u %p %s+0x%llx rva 0x%llx %s:%lu %s",
                           idx, addr, name, (unsigned long long)displacement, (unsigned long long)rva,
                           line.FileName, line.LineNumber, moduleName);
        return;
    }

    PLATFORM_CRASH_LOG("  #%02u %p %s+0x%llx rva 0x%llx %s", idx, addr, name, (unsigned long long)displacement, (unsigned long long)rva, moduleName);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    HANDLE process = GetCurrentProcess();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(process, NULL, TRUE);

    HMODULE module = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &module);
    uintptr_t rva = module ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)module : 0u;
    PLATFORM_CRASH_LOG("Exception code: 0x%08lX at %p module %p rva 0x%Ix",
                       ep->ExceptionRecord->ExceptionCode,
                       ep->ExceptionRecord->ExceptionAddress,
                       module,
                       rva);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005)
    {
        const char* access = "writing";
        if (ep->ExceptionRecord->ExceptionInformation[0] == 0) access = "reading";
        if (ep->ExceptionRecord->ExceptionInformation[0] == 8) access = "executing";
        PLATFORM_CRASH_LOG("Access violation: %s address %p",
                           access,
                           (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    PrintCrashFrame(0u, ep->ExceptionRecord->ExceptionAddress);

    #if defined(_M_X64)
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

    for (u32 i = 0; i < 24u; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                         &frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0) break;
        PrintCrashFrame(i + 1u, (void*)(uintptr_t)frame.AddrPC.Offset);
    }
    #endif

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

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
