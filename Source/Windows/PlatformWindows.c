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
extern SDL_Window* g_SDLWindow;
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