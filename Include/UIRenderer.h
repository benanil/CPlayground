#ifndef CP_UI_RENDERER_H
#define CP_UI_RENDERER_H

#include "Graphics.h"
#include "Extern/clay/clay.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define UI_MAX_SHAPES 8192u
#define UI_MAX_IMAGES 1024u
#define UI_MAX_TEXTS  4096u
#define UI_MAX_BATCHES (UI_MAX_IMAGES + UI_MAX_TEXTS)


typedef enum UICustomType_
{
    UICustomType_TextArea = 1
} UICustomType;

typedef enum UITextAreaFlagBits_
{
    UITextAreaFlags_CenterX     = 1u << 0,
    UITextAreaFlags_CenterY     = 1u << 1,
    UITextAreaFlags_NoWrap      = 1u << 2,
    UITextAreaFlags_Clip        = 1u << 3,
    // shift+drag scrubs the value instead of placing a caret / selecting text.
    // consumer (UIEditFloat/UIEditInt) reads back the accumulated pixel delta
    // from UITextAreaCustomData.dragDeltaX next frame
    UITextAreaFlags_NumericDrag = 1u << 4
} UITextAreaFlagBits;

typedef enum UITreeNodeFlags_
{
    UITreeNodeFlags_Leaf     = 1u << 0, // no expand arrow, never reports a toggle
    UITreeNodeFlags_Selected = 1u << 1
} UITreeNodeFlags;

typedef enum UIButtonFlagBits_
{
    UIButtonFlag_None    = 0,
    UIButtonFlag_FitText = 1u << 0
} UIButtonFlagBits;

typedef enum UIColor_
{
    UIColor_Text = 0,
    UIColor_Quad,
    UIColor_Hovered,
    UIColor_Line,
    UIColor_Border,
    UIColor_CheckboxBG,
    UIColor_TextBoxBG,
    UIColor_SliderInside,
    UIColor_TextBoxCursor,
    UIColor_SelectedBorder,
    UIColor_SubText ,
    UIColor_Count
} UIColor;

typedef enum UIFloat_
{
    UIFloat_BorderWidth = 0,
    UIFloat_ContentStart,
    UIFloat_ButtonSize,
    UIFloat_TextScale,
    UIFloat_TextBoxWidth,
    UIFloat_SliderHeight,
    UIFloat_Depth,
    UIFloat_FieldWidth,
    UIFloat_TextWrapWidth,
    UIFloat_ScrollWidth,
    UIFloat_CornerRadius,
    UIFloat_Count
} UIFloat;

typedef enum UIShapeType_
{
    UIShapeType_Rect = 0,
    UIShapeType_RoundedRect,
    UIShapeType_Circle,
    UIShapeType_Capsule
} UIShapeType;

typedef struct UIShape_
{
    f32 rect[4];   // x, y, width, height in framebuffer pixels.
    f32 params[4]; // radius, border width, softness, unused.
    u32 color;
    u32 borderColor;
    u32 shape;
    u32 flags;
    f32 clip[4];   // x0, y0, x1, y1 in framebuffer pixels.
} UIShape;

typedef struct UIImageData_
{
    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;
    f32 uv[4]; // x, y, width, height. Zero width/height means full image.
} UIImageData;

typedef struct UITextAreaCustomData_
{
    u32 type;
    char* buffer;
    u32 capacity;
    u32 flags;
    u32 edited;
    f32 dragDeltaX; // UITextAreaFlags_NumericDrag: pixel delta accumulated this frame
} UITextAreaCustomData;

void UIInit(void);
void UIDestroy(void);
void UIBeginFrame(void);
void UIEndFrame(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget);
void UIClear(void);
char* UIFrameStringAlloc(u32 size);
void UIBeginBatch(void);
void UIEndBatch(void);

bool UIPushRect(float2 pos, float2 size, u32 color);
bool UIPushRoundedRect(float2 pos, float2 size, f32 radius, u32 color);
bool UIPushCircle(float2 center, f32 radius, u32 color);
bool UIPushCapsule(float2 pos, float2 size, u32 color);
void UIPushBorder(f32 thickness, u32 color);

void UIPushClipRect(float2 pos, float2 size);
void UIPopClipRect(void);
void UIGetClipRect(f32 outClip[4]);

void UISetColor(UIColor what, u32 color);
u32  UIGetColor(UIColor what);
Clay_Color UIGetClayColor(UIColor what);
void UIPushColor(UIColor what, u32 color);
void UIPopColor(UIColor what);
void UISetFloat(UIFloat what, f32 value);
f32  UIGetFloat(UIFloat what);
void UIPushFloat(UIFloat what, f32 value);
void UIPushFloatAdd(UIFloat what, f32 value);
void UIPopFloat(UIFloat what);
bool UIFloatStackZero(UIFloat what);

void UIButtonPushColors(u32 hovered, u32 selected, u32 color);
void UIButtonPopColors();

Clay_Color UIPanelColor(void);

bool   UITextArea(const char* label, float2 pos, char* buffer, u32 capacity, float2 size);
// outDragDelta, when non-null, receives this frame's shift+drag pixel delta while
// UITextAreaFlags_NumericDrag is set (0 otherwise); callers not scrubbing pass NULL
bool   UITextAreaFlags(const char* label, float2 pos, char* buffer, u32 capacity, float2 size, u32 flags, f32* outDragDelta);
void   UIRender(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget);
UIImageData UIImageFromTexture(Texture* texture);

Clay_RenderCommandArray UIEndLayout(void);
void UIRenderCommands(Clay_RenderCommandArray* commands);
bool UIClicked(void);
bool UIButton(Clay_ElementId id, Clay_String label, Clay_Dimensions size, bool selected);
bool UIButtonFlags(Clay_ElementId id, Clay_String label, Clay_Dimensions size, bool selected, u32 flags);
bool UICheckbox(Clay_ElementId id, Clay_String label, bool* value);
void UIProgressBar(Clay_ElementId id, Clay_String label, f32 value01);
bool UISliderFloat(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue);
bool UISliderFloatValue(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue, int decimals);
bool UIEditInt(Clay_ElementId id, Clay_String label, f32* value, s32 minValue, s32 maxValue);
bool UIEditFloat(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue, f32 step, int decimals);

// multi component numeric edit rows for vectors, 1..4 text boxes side by side.
// out: true when any component changed
bool UIEditFloatN(Clay_ElementId id, Clay_String label, f32* values, u32 numComponents, f32 minValue, f32 maxValue, int decimals);
bool UIEditIntN(Clay_ElementId id, Clay_String label, s32* values, u32 numComponents, s32 minValue, s32 maxValue);

// color swatch row that opens a floating hsv picker (sv field, hue bar, rgb boxes).
// rgb is 3 floats 0..1. out: true while the color is being edited
bool UIColorEdit3(Clay_ElementId id, Clay_String label, f32* rgb);

// wraps a null terminated c string, the string must stay alive until UIEndLayout
Clay_String UIStr(const char* chars);

void UISectionHeader(const char* title);
void UITextU32(const char* label, u32 value);
void UIDivider(Clay_ElementId id);
void UISpacing(Clay_ElementId id, f32 pixels);

// vertical scrollable panel, height <= 0 grows to fill the remaining space.
// must be evaluated inside the CLAY() macro of the panel element, the scroll
// offset binds to the element that is open at call time.
// warning: inside a window content (itself a clip element) grow inflates to the
// panel's own content and scrolling breaks, use UIWindowRemainingHeight instead
Clay_ElementDeclaration UIScrollPanelDeclaration(f32 height, u16 childGap);

// one tree row at the given indent depth, the open state is owned by the caller:
//   open[i] ^= UITreeNode(id, label, depth, flags, open[i], &selected);
// out: true when the expand arrow was clicked. outSelected (optional) reports a
// click on the row body for selection
bool UITreeNode(Clay_ElementId id, Clay_String label, u32 depth, u32 flags, bool open, bool* outSelected);

// full width section header, the open state is owned by the caller.
// out: true when clicked, flip your open flag with it
bool UICollapsingHeader(Clay_ElementId id, Clay_String label, bool open);

// labeled selection box that opens a floating option list under it, only one
// dropdown is open at a time. options must outlive the frame.
// out: true when the selection changed
bool UIDropdown(Clay_ElementId id, Clay_String label, const char** options, u32 numOptions, u32* selectedIndex);

typedef struct UIMenuItem_ { const char* label; bool checked; } UIMenuItem;

// a button that drops a checklist menu. each click toggles an item and the menu stays open
// (only one menu button is open at a time). returns the index clicked this frame, or -1; the
// caller updates its own state and item->checked for the highlight shown next frame.
s32  UIMenuButton(Clay_ElementId id, Clay_String label, Clay_Dimensions size, const UIMenuItem* items, u32 count);
bool UIMenuButtonIsOpen(Clay_ElementId id);

u64 UIAutoID(const void* ptr);

static inline Clay_Color UIColorToClay(u32 color)
{
    v128u lanes = VeciSrl(VeciSet1(color), VeciSetR(0, 8, 16, 24));
    lanes = VeciAnd(lanes, VeciSet1(0xFFu));
    Clay_Color result;
    VecStore(&result.r, VecI32ToF32(lanes));
    return result;
}

static inline u32 UIPackClayColor(Clay_Color color)
{
    v128f v = VecLoad(&color.r);
    v = VecClamp(v, VecZero(), VecSet1(255.0f));
    v128u i = VecF32ToU32(v);
    i = VeciSll(i, VeciSetR(0, 8, 16, 24));
    i = VeciOr(i, VecSwapHalvesU(i));
    i = VeciOr(i, VecSwapPairsU(i));
    return VeciGetX(i);
}

#if defined(__cplusplus)
}
#endif

#endif
