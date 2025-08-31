#ifndef PLATFORM_C
#define PLATFORM_C

PlatformContext PlatformCtx = {0};

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
            Bitset_Set(&PlatformCtx.DownKeys, vk_code);
            break;
        }
        case SAPP_EVENTTYPE_KEY_UP: {
            int vk_code = event->key_code;
            if (vk_code > 0 && vk_code < 512) {
                Bitset_Reset(&PlatformCtx.DownKeys, vk_code);
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

void FatalError(const char* format, ...)
{
    char buffer[1024]; // Adjust the size according to your needs
    // Format the error message
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    puts(buffer);
    OutputDebugString(buffer);
    // Display the message box
    #ifdef PLATFORM_WINDOWS
    MessageBoxA(NULL, buffer, "Fatal Error", MB_ICONERROR | MB_OK);
    #elif PLATFORM_ANDROID
    __android_log_write(ANDROID_LOG_ERROR, "ANIL", line_buf);
    #endif
}

void DebugLog(const char* format, ...)
{
    char buffer[1024]; // Adjust the size according to your needs
    // Format the error message
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    puts(buffer);
    #ifdef PLATFORM_WINDOWS
    MessageBoxA(NULL, buffer, "DebugLog", MB_ICONWARNING | MB_OK);
    OutputDebugString(buffer);
    #elif PLATFORM_ANDROID
    __android_log_write(ANDROID_LOG_INFO, "ANIL", line_buf);
    #endif
}

void GetMousePos(float* x, float* y) {
    #ifdef PLATFORM_WINDOWS
    //ASSERT((uint64_t)x & (uint64_t)y); // shouldn't be nullptr
    POINT point;
    GetCursorPos(&point);
    *x = (float)point.x;
    *y = (float)point.y;
    #else
    #error "Get mouse pos is not defined"
    #endif
}

void SetMousePos(float x, float y)
{
    #ifdef PLATFORM_WINDOWS
    SetCursorPos((int)x, (int)y);
    #elif defined(_SAPP_MACOS)
    #error "monitor size is not defined"
    #endif
}

void wGetMouseWindowPos(float* x, float* y) {
    *x = PlatformCtx.MousePosX; *y = PlatformCtx.MousePosY;
}

void wGetMonitorSize(int* width, int* height) 
{
    #ifdef PLATFORM_WINDOWS
    *width  = GetSystemMetrics(SM_CXSCREEN);
    *height = GetSystemMetrics(SM_CYSCREEN);
    #elif defined(_SAPP_MACOS)
    #error "monitor size is not defined"
    #else
    *width = sapp_width();
    *height = sapp_height();
    #endif
}

void SetMouseWindowPos(float x, float y)
{
    SetMousePos(PlatformCtx.WindowPosX + x, PlatformCtx.WindowPosY + y);
}

bool AnyKeyDown()           { return Bitset_Count(&PlatformCtx.DownKeys) > 0; }
bool GetKeyDown(char c)     { return Bitset_Get(&PlatformCtx.DownKeys, c); }
bool GetKeyReleased(char c) { return Bitset_Get(&PlatformCtx.ReleasedKeys, c); }
bool GetKeyPressed(char c)  { return Bitset_Get(&PlatformCtx.PressedKeys, c); }

float GetMouseWheelDelta() { return PlatformCtx.MouseWheelDelta; }
bool GetDoubleClicked() { return PlatformCtx.DoubleClicked; }
bool AnyMouseKeyDown()                 { return PlatformCtx.MouseDown > 0; }
bool GetMouseDown(uint32_t button)     { return !!(PlatformCtx.MouseDown     & button); }
bool GetMouseReleased(uint32_t button) { return !!(PlatformCtx.MouseReleased & button); }
bool GetMousePressed(uint32_t button)  { return !!(PlatformCtx.MousePressed  & button); }

static void SetPressedAndReleasedKeys()
{
    Bitset_AndNot(&PlatformCtx.ReleasedKeys, &PlatformCtx.LastKeys, &PlatformCtx.DownKeys);
    Bitset_AndNot(&PlatformCtx.PressedKeys , &PlatformCtx.DownKeys, &PlatformCtx.LastKeys);
    // Mouse
    PlatformCtx.MouseReleased = PlatformCtx.MouseLast & ~PlatformCtx.MouseDown;
    PlatformCtx.MousePressed  = ~PlatformCtx.MouseLast & PlatformCtx.MouseDown;
}

void wSetWindowSize(int width, int height)
{
    #ifdef PLATFORM_WINDOWS
    PlatformCtx.WindowWidth = width; PlatformCtx.WindowHeight = height;
    SetWindowPos((void*)sapp_win32_get_hwnd(), NULL, PlatformCtx.WindowPosX, PlatformCtx.WindowPosY, width, height, 0);
    #elif defined(_SAPP_LINUX)
    XResizeWindow(sapp_x11_get_display(), sapp_x11_get_window(), (unsigned)width, (unsigned)height);
    XFlush(sapp_x11_get_display());
    #elif defined(_SAPP_MACOS)
    #error "Window get size is not defined"
    #endif
}

void wSetWindowPosition(int x, int y)
{
    #ifdef PLATFORM_WINDOWS
    PlatformCtx.WindowPosX = x; PlatformCtx.WindowPosY = y;
    SetWindowPos((void*)sapp_win32_get_hwnd(), NULL, x, y, PlatformCtx.WindowWidth, PlatformCtx.WindowHeight, 0);
    #elif defined(_SAPP_LINUX)
    XMoveWindow(sapp_x11_get_display(), sapp_x11_get_window(), x, y);
    XFlush(sapp_x11_get_display());
    #elif defined(_SAPP_MACOS)
    #error "Window get size is not defined"
    #endif
}

static void FixSeperators(char* dst, const char* src)
{
    int len = StringLength(src);
    SmallMemCpy(dst, src, len);
    
    for (int i = 0; i < len; i++) 
        if (dst[i] == '/') dst[i] = '\\';
}

bool wOpenFolder(const char* folderPath) 
{
    #ifdef PLATFORM_WINDOWS
    char copy[512] = {0};
    FixSeperators(copy, folderPath);

    if ((size_t)ShellExecuteA(NULL, "open", copy, NULL, NULL, SW_SHOWNORMAL) <= 32) 
        return false;
    return true;
    #else 
    return false;
    #endif
}

bool wOpenFile(const char* filePath)
{
    #ifdef PLATFORM_WINDOWS
    char copy[512] = {0};
    FixSeperators(copy, filePath);  

    if ((size_t)ShellExecuteA(NULL, NULL, copy, NULL, NULL, SW_SHOW) <= 32)
        return false;
    return true;
    #else 
    return false;
    #endif
}


double GetDeltaTime() 
{ 
    return PlatformCtx.DeltaTime; 
}

static uint64_t DownKeys[8]; 
static uint64_t LastKeys[8]; 
static uint64_t PressedKeys[8];
static uint64_t ReleasedKeys[8];

void PlatformInit()
{
    PlatformCtx.DownKeys.bits = DownKeys;
    PlatformCtx.LastKeys.bits = LastKeys;
    PlatformCtx.PressedKeys.bits = PressedKeys;
    PlatformCtx.ReleasedKeys.bits = ReleasedKeys;
}

double TimeSinceStartup()
{
    return stm_sec(stm_diff(stm_now(), PlatformCtx.StartupTime));
}

#endif // PLATFORM_C
