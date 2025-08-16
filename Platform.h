#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
// #include "sokol_app.h"

/*********************************************************************************
 *    Description:                                                               *
 *        Cross-platform input and window management using Sokol                *
 *        Provides Windows-compatible API for mouse, keyboard, and window events *
 *    Author:                                                                    *
 *        Platform abstraction layer for game/application development            *
 *********************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

// Mouse button flags
typedef enum MouseButton_ {
    MouseButton_Left     = 1 << 0,
    MouseButton_Right    = 1 << 1, 
    MouseButton_Middle   = 1 << 2,
    MouseButton_Forward  = 1 << 3,
    MouseButton_Backward = 1 << 4
} MouseButton;

// Platform context structure (opaque to users)
typedef struct PlatformContext_ PlatformContext;

// Core event handling functions
void sokol_event_callback(const sapp_event* event);
void platform_update(float dt);

// Input state query functions
bool is_key_down(int vk_code);
bool is_mouse_button_down(uint32_t button);

// Mouse state getters
float get_mouse_x(void);
float get_mouse_y(void);
float get_mouse_wheel_delta(void);
bool get_double_clicked(void);

// Window state getters  
void wGetMonitorSize(int* width, int* height);
int get_window_width(void);
int get_window_height(void);
int get_window_pos_x(void);
int get_window_pos_y(void);

// Platform initialization
void platform_init(void);
void platform_cleanup(void);

// Sokol main entry point
sapp_desc sokol_main(int argc, char* argv[]);

// Utility functions
void reset_mouse_wheel_delta(void);
void reset_double_click_state(void);

// Key state management
void clear_all_keys(void);
bool any_key_pressed(void);
uint32_t get_key_count(void);

// Advanced mouse functions
void get_mouse_pos(float* x, float* y);
void set_mouse_pos(float x, float y); // Platform dependent
bool is_mouse_in_window(void);

// Window management functions
void request_quit(void);
bool is_window_focused(void);
void set_window_title(const char* title);

void GetMousePos(float* x, float* y);
void SetMousePos(float x, float y);
void GetMouseWindowPos(float* x, float* y);
void SetMouseWindowPos(float x, float y);

// Convenience macros for common key checks
#define IsKeyDown(key)          is_key_down(key)
#define IsMouseDown(button)     is_mouse_button_down(button)
#define GetMouseX()             get_mouse_x()
#define GetMouseY()             get_mouse_y()
#define GetMouseWheel()         get_mouse_wheel_delta()
#define IsDoubleClicked()       get_double_clicked()

// Mouse button convenience macros
#define IsLeftMouseDown()       is_mouse_button_down(MouseButton_Left)
#define IsRightMouseDown()      is_mouse_button_down(MouseButton_Right)
#define IsMiddleMouseDown()     is_mouse_button_down(MouseButton_Middle)

// Common key check macros
#define IsWASDPressed()         (is_key_down(VK_W) || is_key_down(VK_A) || is_key_down(VK_S) || is_key_down(VK_D))
#define IsArrowKeyPressed()     (is_key_down(VK_LEFT) || is_key_down(VK_RIGHT) || is_key_down(VK_UP) || is_key_down(VK_DOWN))
#define IsShiftPressed()        is_key_down(VK_SHIFT)
#define IsControlPressed()      is_key_down(VK_CONTROL)
#define IsAltPressed()          is_key_down(VK_ALT)

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_H