#define CLAY_IMPLEMENTATION
#include "UI_Internal.h"
#include "Include/Memory.h"
#include "Include/Platform.h" // PlatformContext.lastTime for milisecond text
#include "Include/Random.h"
#include "Include/String.h"
#include "Include/Algorithm.h"
#include "Include/Rendering.h"
#include "Include/UIWindow.h"
#include "Extern/kb/kb_text_shape.h"

UIRenderer g_UIRenderer;
UIContext g_UI;
UILayoutContext g_UILayout;

extern WindowState  g_WindowState;
extern RenderState  g_RenderState;

static char                 g_UISliderValueLabels[64][96];
static u32                  g_UISliderValueLabelIndex;
static UIShape              g_UIShapeStorage[UI_MAX_SHAPES];
static UIImageCommand       g_UIImageStorage[UI_MAX_IMAGES];
static UIOrderedTextCommand g_UITextStorage[UI_MAX_TEXTS];
static UIBatch              g_UIBatchStorage[UI_MAX_BATCHES];
static u8                   g_UILayoutMemory[8u * 1024u * 1024u];
static char                 g_UIFrameStringMemory[64u * 1024u];
static Arena                g_UIFrameStringArena;

void UIRecordTextBatches(u32 firstBatch, u32 batchCount)
{
    if (batchCount == 0u) return;
    UIBeginBatch();
    if (!g_UIRenderer.texts || g_UIRenderer.textCount >= g_UIRenderer.textCapacity)
    {
        AX_WARN("UI text command batch full: count=%u capacity=%u", g_UIRenderer.textCount, g_UIRenderer.textCapacity);
        return;
    }

    UIOrderedTextCommand* out = &g_UIRenderer.texts[g_UIRenderer.textCount++];
    out->firstBatch = firstBatch;
    out->batchCount = batchCount;
}

static void UIRenderLayoutImage(const Clay_RenderCommand* command);

static void UILayoutHandleError(Clay_ErrorData errorData)
{
    AX_WARN("Clay error: %.*s", errorData.errorText.length, errorData.errorText.chars);
}

static Clay_Dimensions UIMeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData)
{
    (void)userData;
    f32 size = (f32)Maxu32(config && config->fontSize ? config->fontSize : 16u, 1u);
    float2 measured = SlugCalcTextSizeN(SlugGetDemoFont(), text.chars, (u32)Maxs32(text.length, 0), size);
    if (config && config->lineHeight > 0u) measured.y = Maxf32(measured.y, (f32)config->lineHeight);
    return (Clay_Dimensions){ measured.x, measured.y };
}

static void UILayoutInit(void)
{
    if (g_UILayout.initialized) return;

    u32 memorySize = Clay_MinMemorySize();
    if (memorySize > (u32)sizeof(g_UILayoutMemory))
    {
        AX_WARN("Clay init skipped, static arena too small: needed=%u capacity=%u", memorySize, (u32)sizeof(g_UILayoutMemory));
        return;
    }

    g_UILayout.memorySize = memorySize;
    g_UILayout.memory = g_UILayoutMemory;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(sizeof(g_UILayoutMemory), g_UILayout.memory);
    Clay_Initialize(arena, (Clay_Dimensions){ (f32)Maxu32(g_WindowState.prev_width, 1u), (f32)Maxu32(g_WindowState.prev_height, 1u) },
                           (Clay_ErrorHandler){ UILayoutHandleError });
    Clay_SetMeasureTextFunction(UIMeasureText, NULL);
    g_UILayout.initialized = true;
}

void UIInit(void)
{
    MemsetZero(&g_UIRenderer, sizeof(UIRenderer));
    MemsetZero(&g_UI, sizeof(UIContext));
    g_UIRenderer.activeBatch   = UINT32_MAX;
    g_UIRenderer.capacity      = UI_MAX_SHAPES;
    g_UIRenderer.imageCapacity = UI_MAX_IMAGES;
    g_UIRenderer.textCapacity  = UI_MAX_TEXTS;
    g_UIRenderer.batchCapacity = UI_MAX_BATCHES;
    g_UIRenderer.shapes  = g_UIShapeStorage;
    g_UIRenderer.images  = g_UIImageStorage;
    g_UIRenderer.texts   = g_UITextStorage;
    g_UIRenderer.batches = g_UIBatchStorage;
    g_UIFrameStringArena.buf = g_UIFrameStringMemory;
    g_UIFrameStringArena.buffLen = sizeof(g_UIFrameStringMemory);
    g_UIFrameStringArena.currOffset = 0u;
    g_RenderState.uiShapeBuffer = CreateBuffer(NULL, (size_t)g_UIRenderer.capacity * sizeof(UIShape), BReadRasterBit, "UIShapeBuffer");
    g_RenderState.uiShapeDrawArgsBuffer = CreateBuffer(NULL, sizeof(SDL_GPUIndirectDrawCommand), BIndirectBit, "UIShapeDrawArgsBuffer");

    for (u32 i = 0; i < UIColor_Count; i++) g_UI.colorStackCount[i] = -1;
    for (u32 i = 0; i < UIFloat_Count; i++) g_UI.floatStackCount[i] = -1;
    g_UI.colors[UIColor_Text]           = 0xFFEAEAEAu;
    g_UI.colors[UIColor_SubText]        = UIPackClayColor((Clay_Color){ 140, 140, 140, 255 });
    g_UI.colors[UIColor_Quad]           = 0xFF1A1A1Au;
    g_UI.colors[UIColor_Hovered]        = 0x28E8A400u;
    g_UI.colors[UIColor_Line]           = 0xFF3D3D3Du;
    g_UI.colors[UIColor_Border]         = UIPackClayColor((Clay_Color){ 55, 55, 55, 200 });
    g_UI.colors[UIColor_CheckboxBG]     = 0xFF141414u;
    g_UI.colors[UIColor_TextBoxBG]      = 0xFF141414u;
    g_UI.colors[UIColor_SliderInside]   = 0xFFE8A400u;
    g_UI.colors[UIColor_TextBoxCursor]  = 0xFFE8A400u;
    g_UI.colors[UIColor_SelectedBorder] = 0xFFE8A400u;
    g_UI.floats[UIFloat_BorderWidth]    = 3.0f;
    g_UI.floats[UIFloat_ContentStart]   = 160.0f;
    g_UI.floats[UIFloat_ButtonSize]     = 100.0f;
    g_UI.floats[UIFloat_TextScale]      = 1.0f;
    g_UI.floats[UIFloat_TextBoxWidth]   = 175.0f;
    g_UI.floats[UIFloat_SliderHeight]   = 16.0f;
    g_UI.floats[UIFloat_Depth]          = 0.9f;
    g_UI.floats[UIFloat_FieldWidth]     = 98.0f;
    g_UI.floats[UIFloat_TextWrapWidth]  = 100.0f;
    g_UI.floats[UIFloat_ScrollWidth]    = 14.0f;
    g_UI.floats[UIFloat_CornerRadius]   = 4.0f;
    UILayoutInit();
}

static void UILayoutBeginFrame(void)
{
    if (!g_UILayout.initialized) return;

    u32 drawableW = Maxu32(g_WindowState.prev_width, 1u);
    u32 drawableH = Maxu32(g_WindowState.prev_height, 1u);
    Clay_SetLayoutDimensions((Clay_Dimensions){ (f32)drawableW, (f32)drawableH });
    Clay_SetPointerState((Clay_Vector2){ g_UI.mouse.x, g_UI.mouse.y }, GetMouseDown(MouseButton_Left) != 0u);

    f32 wheel = GetMouseWheelDelta();
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ 0.0f, wheel * 24.0f }, GetDeltaTime());
    PlatformCtx.MouseWheelDelta = 0.0f;
}

Clay_RenderCommandArray UIEndLayout(void)
{
    if (!g_UILayout.initialized) return (Clay_RenderCommandArray){0};
    return Clay_EndLayout(GetDeltaTime());
}

u64 UIAutoID(const void* ptr)
{
    u64 x = (u64)(uintptr_t)ptr ^ ((u64)g_UI.nextAutoID++ * 0x9E3779B97F4A7C15ull);
    return MurmurHash(x);
}

static Clay_Color UIButtonColor(bool hovered, bool selected)
{
    if (hovered) return UIGetClayColor(UIColor_Hovered);
    if (selected) return UIGetClayColor(UIColor_SelectedBorder);
    return UIGetClayColor(UIColor_Quad);
}

void UIButtonPushColors(u32 hovered, u32 selected, u32 color)
{
    UIPushColor(UIColor_Hovered, hovered);
    UIPushColor(UIColor_SelectedBorder, selected);
    UIPushColor(UIColor_Quad, color);
}

void UIButtonPopColors()
{
    UIPopColor(UIColor_Hovered);
    UIPopColor(UIColor_SelectedBorder);
    UIPopColor(UIColor_Quad);
}

Clay_Color UIPanelColor(void)
{
    return UIGetClayColor(UIColor_TextBoxBG);
}

static void UIRenderLayoutRectangle(const Clay_RenderCommand* command)
{
    const Clay_RectangleRenderData* data = &command->renderData.rectangle;
    f32 radius = Maxf32(Maxf32(data->cornerRadius.topLeft, data->cornerRadius.topRight), Maxf32(data->cornerRadius.bottomLeft, data->cornerRadius.bottomRight));
    UIPushRoundedRect((float2){ command->boundingBox.x, command->boundingBox.y }, (float2){ command->boundingBox.width, command->boundingBox.height }, radius, UIPackClayColor(data->backgroundColor));
}

static void UIRenderLayoutBorder(const Clay_RenderCommand* command)
{
    const Clay_BorderRenderData* data = &command->renderData.border;
    f32 radius = Maxf32(Maxf32(data->cornerRadius.topLeft, data->cornerRadius.topRight), Maxf32(data->cornerRadius.bottomLeft, data->cornerRadius.bottomRight));
    f32 width = Maxf32(Maxf32((f32)data->width.left, (f32)data->width.right), Maxf32((f32)data->width.top, (f32)data->width.bottom));
    if (width <= 0.0f) return;

    bool uniformWidth = data->width.left == data->width.right && data->width.left == data->width.top && data->width.left == data->width.bottom;
    if (!uniformWidth)
    {
        static int warningCount = 0;
        if (warningCount++ < 4) AX_WARN("Clay non-uniform border is not supported yet");
        return;
    }

    UIPushRoundedRect((float2){ command->boundingBox.x, command->boundingBox.y }, (float2){ command->boundingBox.width, command->boundingBox.height }, radius, 0u);
    UIPushBorder(width, UIPackClayColor(data->color));
}

static void UIRenderLayoutText(const Clay_RenderCommand* command)
{
    const Clay_TextRenderData* data = &command->renderData.text;
    Clay_StringSlice text = data->stringContents;
    SlugFont* font = SlugGetDemoFont();
    u32 firstBatch = font->numBatches;
    SlugAppendText2DN(NULL, text.chars, (u32)Maxs32(text.length, 0), (float2){ command->boundingBox.x, command->boundingBox.y }, (f32)Maxu32(data->fontSize, 1u), UIPackClayColor(data->textColor));
    UIRecordTextBatches(firstBatch, font->numBatches - firstBatch);
}

static void UIRenderLayoutCustom(const Clay_RenderCommand* command)
{
    const Clay_CustomRenderData* data = &command->renderData.custom;
    UITextAreaCustomData* custom = (UITextAreaCustomData*)data->customData;
    if (custom && custom->type == UICustomType_TextArea)
    {
        if (UITextAreaFlags(NULL,
                            (float2){ command->boundingBox.x, command->boundingBox.y },
                            custom->buffer,
                            custom->capacity,
                            (float2){ command->boundingBox.width, command->boundingBox.height },
                            custom->flags,
                            &custom->dragDeltaX))
            custom->edited = 1u;
        return;
    }

    if (!g_UILayout.warnedCustom)
    {
        AX_WARN("Clay custom commands are not rendered yet");
        g_UILayout.warnedCustom = true;
    }
}

static void UIRenderLayoutScrollBar(const Clay_RenderCommand* command)
{
    static struct { u32 id; f32 dragOffsetY; } drag;

    Clay_ElementId id = { .id = command->id };
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(id);
    if (!scroll.found || !scroll.scrollPosition) return;

    f32 containerH = Maxf32(scroll.scrollContainerDimensions.height, 1.0f);
    f32 contentH   = Maxf32(scroll.contentDimensions.height, containerH);
    f32 maxScroll = contentH - containerH;
    if (maxScroll <= 1.0f) return;

    if (!GetMouseDown(MouseButton_Left) && drag.id == command->id) drag.id = 0u;

    // clamp the bar to the active clip so containers laid out wider or taller than
    // their parent window still show it at the visible edge
    f32 clip[4];
    UIGetClipRect(clip);
    f32 trackW = Maxf32(UIGetFloat(UIFloat_ScrollWidth) * 0.45f, 5.0f);
    f32 trackX = Minf32(command->boundingBox.x + command->boundingBox.width, clip[2]) - trackW - 2.0f;
    f32 trackY = Maxf32(command->boundingBox.y, clip[1]);
    f32 trackH = Minf32(command->boundingBox.y + containerH, clip[3]) - trackY;
    if (trackH <= 8.0f) return;

    f32 thumbH = Minf32(Maxf32(trackH * (containerH / contentH), 28.0f), trackH);
    f32 t = Saturatef32(-scroll.scrollPosition->y / maxScroll);
    f32 thumbY = trackY + t * (trackH - thumbH);

    float2 mouse = g_UI.mouse;
    float2 thumbPos  = { trackX - 4.0f, thumbY };
    float2 thumbSize = { trackW + 8.0f, thumbH };
    float2 trackPos  = { trackX - 4.0f, trackY };
    float2 trackSize = { trackW + 8.0f, trackH };
    bool thumbHovered = RectPointIntersect(thumbPos, thumbSize, mouse) != 0u;
    bool trackHovered = RectPointIntersect(trackPos, trackSize, mouse) != 0u;

    if (!g_UI.windowResizeActive && GetMousePressed(MouseButton_Left) && (thumbHovered || trackHovered))
    {
        drag.id = command->id;
        drag.dragOffsetY = thumbHovered ? mouse.y - thumbY : thumbH * 0.5f;
        g_UI.scrollBarActive = true;
    }
    if (!g_UI.windowResizeActive && drag.id == command->id && GetMouseDown(MouseButton_Left))
    {
        g_UI.scrollBarActive = true;
        f32 scrollRange = Maxf32(trackH - thumbH, 1.0f);
        f32 newT = Saturatef32((mouse.y - trackY - drag.dragOffsetY) / scrollRange);
        scroll.scrollPosition->y = -newT * maxScroll;
        thumbY = trackY + newT * scrollRange;
        thumbHovered = true;
    }

    UIPushRoundedRect((float2){ trackX, trackY }, (float2){ trackW, trackH }, trackW * 0.5f, 0x33404040u);
    u32 thumbColor = (drag.id == command->id || thumbHovered) ? UIGetColor(UIColor_SelectedBorder) : 0xAA808080u;
    UIPushRoundedRect((float2){ trackX, thumbY }, (float2){ trackW, thumbH }, trackW * 0.5f, thumbColor);
}

static void UIRenderLayoutImage(const Clay_RenderCommand* command)
{
    UIBeginBatch();
    const Clay_ImageRenderData* data = &command->renderData.image;
    const UIImageData* image = (const UIImageData*)data->imageData;
    if (!image || !image->texture)
    {
        if (!g_UILayout.warnedImage)
        {
            AX_WARN("Clay image command skipped, imageData must point to UIImageData with a valid texture");
            g_UILayout.warnedImage = true;
        }
        return;
    }
    if (!g_UIRenderer.images || g_UIRenderer.imageCount >= g_UIRenderer.imageCapacity)
    {
        AX_WARN("UI image batch full: count=%u capacity=%u", g_UIRenderer.imageCount, g_UIRenderer.imageCapacity);
        return;
    }

    UIImageCommand* out = &g_UIRenderer.images[g_UIRenderer.imageCount++];
    out->texture = image->texture;
    out->sampler = image->sampler ? image->sampler : g_RenderState.sampler;
    out->rect[0] = command->boundingBox.x;
    out->rect[1] = command->boundingBox.y;
    out->rect[2] = command->boundingBox.width;
    out->rect[3] = command->boundingBox.height;
    out->uv[0] = image->uv[0];
    out->uv[1] = image->uv[1];
    out->uv[2] = image->uv[2] != 0.0f ? image->uv[2] : 1.0f;
    out->uv[3] = image->uv[3] != 0.0f ? image->uv[3] : 1.0f;
    MemCopy(out->clip, g_UI.clipRect, sizeof(out->clip));
    out->tintColor = UIPackClayColor(data->backgroundColor);
    out->radius = Maxf32(Maxf32(data->cornerRadius.topLeft, data->cornerRadius.topRight), Maxf32(data->cornerRadius.bottomLeft, data->cornerRadius.bottomRight));
}

void UIClear(void)
{
    g_UIRenderer.count = 0u;
    g_UIRenderer.imageCount = 0u;
    g_UIRenderer.textCount = 0u;
    g_UIRenderer.batchCount = 0u;
    g_UIRenderer.activeBatch = UINT32_MAX;
}

void UIBeginBatch(void)
{
    if (g_UIRenderer.activeBatch != UINT32_MAX) return;
    if (!g_UIRenderer.batches || g_UIRenderer.batchCount >= g_UIRenderer.batchCapacity)
    {
        AX_WARN("UI batch stream full: count=%u capacity=%u", g_UIRenderer.batchCount, g_UIRenderer.batchCapacity);
        return;
    }

    SlugFont* font = SlugGetDemoFont();
    SlugBeginTextBatch(font);
    u32 index = g_UIRenderer.batchCount++;
    UIBatch* batch = &g_UIRenderer.batches[index];
    batch->firstShape = g_UIRenderer.count;
    batch->shapeCount = 0u;
    batch->firstImage = g_UIRenderer.imageCount;
    batch->imageCount = 0u;
    batch->firstTextBatch = font->numBatches;
    batch->textBatchCount = 0u;
    g_UIRenderer.activeBatch = index;
}

void UIEndBatch(void)
{
    if (g_UIRenderer.activeBatch == UINT32_MAX) return;

    SlugFont* font = SlugGetDemoFont();
    UIBatch* batch = &g_UIRenderer.batches[g_UIRenderer.activeBatch];
    batch->shapeCount = g_UIRenderer.count - batch->firstShape;
    batch->imageCount = g_UIRenderer.imageCount - batch->firstImage;
    batch->textBatchCount = font->numBatches - batch->firstTextBatch;
    SlugEndTextBatch(font);

    if (batch->shapeCount == 0u && batch->imageCount == 0u && batch->textBatchCount == 0u && g_UIRenderer.activeBatch + 1u == g_UIRenderer.batchCount)
    {
        g_UIRenderer.batchCount--;
    }
    g_UIRenderer.activeBatch = UINT32_MAX;
}

void UIBeginFrame(void)
{
    UIClear();
    g_UISliderValueLabelIndex = 0u;
    g_UI.nextAutoID = 1u;
    g_UI.wasHovered = false;
    g_UI.anyElementClicked = false;
    if (!GetMouseDown(MouseButton_Left))
    {
        g_UI.scrollBarActive = false;
        g_UI.windowResizeActive = false;
    }
    g_UI.screenSize = (float2){ (f32)Maxu32(g_WindowState.prev_width, 1u), (f32)Maxu32(g_WindowState.prev_height, 1u) };
    g_UI.windowRatio = (float2){ 1.0f, 1.0f };
    g_UI.uiScale = 1.0f;
    g_UI.clipStackCount = 0;
    g_UI.clipRect[0] = 0.0f;
    g_UI.clipRect[1] = 0.0f;
    g_UI.clipRect[2] = g_UI.screenSize.x;
    g_UI.clipRect[3] = g_UI.screenSize.y;

    f32 mx, my;
    wGetMouseWindowPos(&mx, &my);
    if (PlatformCtx.WindowWidth > 0 && PlatformCtx.WindowHeight > 0)
    {
        mx *= g_UI.screenSize.x / (f32)PlatformCtx.WindowWidth;
        my *= g_UI.screenSize.y / (f32)PlatformCtx.WindowHeight;
    }
    g_UI.mouse = (float2){ mx, my };
    if (!GetMouseDown(MouseButton_Left)) g_UI.active = 0u;
    if (!GetMouseDown(MouseButton_Left)) g_UI.textDragFocus = 0u;
    if (!g_UI.keyboardFocus) PlatformConsumeTextKeyEvents(NULL, UINT32_MAX);
    UIWindowBeginFrame();
    UILayoutBeginFrame();
}

char* UIFrameStringAlloc(u32 size)
{
    if (size == 0u) return NULL;
    if (ArenaRemaining(&g_UIFrameStringArena) < size)
    {
        AX_WARN("UI frame string arena full: requested=%u remaining=%llu", size, (u64)ArenaRemaining(&g_UIFrameStringArena));
        return NULL;
    }
    return (char*)ArenaAllocAlign(&g_UIFrameStringArena, size, 1u);
}

void UIEndFrame(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget)
{
    UIWindowEndFrame();
    UIRender(cmd, colorTarget);
    SlugFont* font = SlugGetDemoFont();
    SlugRender2DBatches(cmd, colorTarget, font, 0u, font->numBatches, true);
    SlugClear(font);
    g_UI.mouseOld = g_UI.mouse;
    ArenaReset(&g_UIFrameStringArena);
}

static bool UIPushShape(float2 pos, float2 size, f32 radius, u32 color, UIShapeType shape)
{
    UIBeginBatch();
    if (!g_UIRenderer.shapes)
    {
        AX_WARN("UI push skipped, resources not initialized");
        return false;
    }
    if (g_UIRenderer.count >= g_UIRenderer.capacity)
    {
        AX_WARN("UI shape batch full: count=%u capacity=%u", g_UIRenderer.count, g_UIRenderer.capacity);
        return false;
    }

    size.x = Maxf32(size.x, 0.0f);
    size.y = Maxf32(size.y, 0.0f);
    UIShape* s = &g_UIRenderer.shapes[g_UIRenderer.count++];
    s->rect[0] = pos.x;
    s->rect[1] = pos.y;
    s->rect[2] = size.x;
    s->rect[3] = size.y;
    s->params[0] = radius;
    s->params[1] = 0.0f;
    s->params[2] = 1.0f;
    s->params[3] = 0.0f;
    s->color = color;
    s->borderColor = color;
    s->shape = (u32)shape;
    s->flags = 0u;
    s->clip[0] = g_UI.clipRect[0];
    s->clip[1] = g_UI.clipRect[1];
    s->clip[2] = g_UI.clipRect[2];
    s->clip[3] = g_UI.clipRect[3];
    return true;
}

bool UIPushRect(float2 pos, float2 size, u32 color)
{
    return UIPushShape(pos, size, 0.0f, color, UIShapeType_Rect);
}

bool UIPushRoundedRect(float2 pos, float2 size, f32 radius, u32 color)
{
    return UIPushShape(pos, size, radius, color, UIShapeType_RoundedRect);
}

bool UIPushCircle(float2 center, f32 radius, u32 color)
{
    float2 pos = { center.x - radius, center.y - radius };
    float2 size = { radius * 2.0f, radius * 2.0f };
    return UIPushShape(pos, size, radius, color, UIShapeType_Circle);
}

bool UIPushCapsule(float2 pos, float2 size, u32 color)
{
    return UIPushShape(pos, size, Minf32(size.x, size.y) * 0.5f, color, UIShapeType_Capsule);
}

void UIPushBorder(f32 thickness, u32 color)
{
    if (g_UIRenderer.count == 0) return;
    UIShape* s = &g_UIRenderer.shapes[g_UIRenderer.count - 1u];
    s->borderColor = color;
    s->params[1] = thickness;
}

void UIPushClipRect(float2 pos, float2 size)
{
    if (g_UI.clipStackCount >= (s32)UI_STACK_SIZE)
    {
        AX_WARN("UI clip stack full");
        return;
    }

    MemCopy(g_UI.clipStack[g_UI.clipStackCount++], g_UI.clipRect, sizeof(g_UI.clipRect));
    f32 x0 = pos.x;
    f32 y0 = pos.y;
    f32 x1 = pos.x + Maxf32(size.x, 0.0f);
    f32 y1 = pos.y + Maxf32(size.y, 0.0f);
    g_UI.clipRect[0] = Maxf32(g_UI.clipRect[0], x0);
    g_UI.clipRect[1] = Maxf32(g_UI.clipRect[1], y0);
    g_UI.clipRect[2] = Minf32(g_UI.clipRect[2], x1);
    g_UI.clipRect[3] = Minf32(g_UI.clipRect[3], y1);
}

void UIPopClipRect(void)
{
    if (g_UI.clipStackCount <= 0)
    {
        AX_WARN("UI clip stack underflow");
        return;
    }

    MemCopy(g_UI.clipRect, g_UI.clipStack[--g_UI.clipStackCount], sizeof(g_UI.clipRect));
}

void UIGetClipRect(f32 outClip[4])
{
    if (!outClip) return;
    MemCopy(outClip, g_UI.clipRect, sizeof(g_UI.clipRect));
}


bool UIClicked(void)
{
    Clay_PointerData pointer = Clay_GetPointerState();
    return Clay_Hovered() && pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME;
}

bool UIButtonFlags(Clay_ElementId id, Clay_String label, Clay_Dimensions size, bool selected, u32 flags)
{
    bool clicked = false;
    u32 fontSize = Maxu32((u32)(17.0f * UIGetFloat(UIFloat_TextScale)), 1u);
    if (flags & UIButtonFlag_FitText)
    {
        float2 textSize = SlugCalcTextSizeN(SlugGetDemoFont(), label.chars, (u32)Maxs32(label.length, 0), (f32)fontSize);
        size.width  = Maxf32(size.width, textSize.x + 22.0f);
        size.height = Maxf32(size.height, textSize.y + 8.0f);
    }
    float radius = UIFloatStackZero(UIFloat_CornerRadius) ? size.height * 0.5f : UIGetFloat(UIFloat_CornerRadius);
    Clay_CornerRadius cr = (Clay_CornerRadius) { radius, radius, radius, radius };
    
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(size.width), CLAY_SIZING_FIXED(size.height) },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = UIButtonColor(Clay_Hovered(), selected),
        .cornerRadius = cr,
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(UIGetFloat(UIFloat_BorderWidth)) } 
    }) {
        if (UIClicked()) clicked = true;
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)fontSize,
            .textColor = UIGetClayColor(UIColor_Text)
        }));
    }
    return clicked;
}

bool UIButton(Clay_ElementId id, Clay_String label, Clay_Dimensions size, bool selected)
{
    return UIButtonFlags(id, label, size, selected, UIButtonFlag_None);
}

bool UICheckbox(Clay_ElementId id, Clay_String label, bool* value)
{
    bool checked = value && *value;
    bool changed = false;

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        bool rowHovered = Clay_Hovered();
        if (value && UIClicked())
        {
            *value = !*value;
            checked = *value;
            changed = true;
        }

        CLAY(CLAY_ID_LOCAL("Box"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(18.0f), CLAY_SIZING_FIXED(18.0f) },
                .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = UIColorToClay(rowHovered ? UIGetColor(UIColor_Hovered) : UIGetColor(UIColor_CheckboxBG)),
            .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
            .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(UIGetFloat(UIFloat_BorderWidth)) }
        }) {
            if (checked)
            {
                CLAY(CLAY_ID_LOCAL("Mark"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(10.0f), CLAY_SIZING_FIXED(10.0f) } },
                    .backgroundColor = UIGetClayColor(UIColor_SliderInside),
                    .cornerRadius = CLAY_CORNER_RADIUS(2.0f)
                }) {}
            }
        }

        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));
    }

    return changed;
}

void UIProgressBar(Clay_ElementId id, Clay_String label, f32 value01)
{
    value01 = Saturatef32(value01);
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Track"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(12.0f) },
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = UIPanelColor(),
            .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius))
        }) {
            CLAY(CLAY_ID_LOCAL("Fill"), {
                .layout = { .sizing = { CLAY_SIZING_PERCENT(value01), CLAY_SIZING_FIXED(12.0f) } },
                .backgroundColor = UIGetClayColor(UIColor_SliderInside),
                .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius))
            }) {}
        }
    }
}

Clay_String UIStr(const char* chars)
{
    Clay_String result = { .isStaticallyAllocated = false, .length = (s32)StringLength(chars), .chars = chars };
    return result;
}

void UISectionHeader(const char* title)
{
    CLAY_TEXT(UIStr(title), CLAY_TEXT_CONFIG({
        .fontSize = 16,
        .textColor = { 232, 164, 0, 255 }
    }));
}

void UITextU32(const char* label, u32 value)
{
    u32 len = (u32)StringLength(label);
    char* text = UIFrameStringAlloc(len + 16u);
    if (!text) return;
    MemCopy(text, label, len);
    text[len++] = ':';
    text[len++] = ' ';
    len += (u32)IntToString(text + len, (int64_t)value, 0);
    text[len] = '\0';

    Clay_String string = { .isStaticallyAllocated = false, .length = (s32)len, .chars = text };
    CLAY_TEXT(string, CLAY_TEXT_CONFIG({
        .fontSize = 14,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
}

void UIDivider(Clay_ElementId id)
{
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } },
        .backgroundColor = { 55, 55, 55, 160 }
    }) {}
}

void UISpacing(Clay_ElementId id, f32 pixels)
{
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(pixels) } }
    }) {}
}

Clay_ElementDeclaration UIScrollPanelDeclaration(f32 height, u16 childGap)
{
    // horizontal clip keeps wide children from inflating the panel and its scissor rect
    Clay_ElementDeclaration declaration = {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), height > 0.0f ? CLAY_SIZING_FIXED(height) : CLAY_SIZING_GROW(0) },
            .padding = { 0, 20, 0, 0 },
            .childGap = childGap,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() }
    };
    return declaration;
}

bool UITreeNode(Clay_ElementId id, Clay_String label, u32 depth, u32 flags, bool open, bool* outSelected)
{
    bool leaf     = (flags & UITreeNodeFlags_Leaf) != 0u;
    bool selected = (flags & UITreeNodeFlags_Selected) != 0u;
    bool toggled  = false;
    if (outSelected) *outSelected = false;

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24.0f) },
            .padding = { (u16)(4u + depth * 18u), 4, 0, 0 },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = selected ? UIColorToClay((UIGetColor(UIColor_SelectedBorder) & 0x00FFFFFFu) | 0x48000000u)
                                    : (Clay_Hovered() ? UIGetClayColor(UIColor_Hovered) : (Clay_Color){ 0 }),
        .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius))
    }) {
        bool rowClicked = UIClicked();
        bool arrowClicked = false;

        CLAY(CLAY_ID_LOCAL("Arrow"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(16.0f), CLAY_SIZING_FIXED(16.0f) },
                .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
            }
        }) {
            arrowClicked = UIClicked();
            if (!leaf)
            {
                CLAY_TEXT(open ? CLAY_STRING("-") : CLAY_STRING("+"), CLAY_TEXT_CONFIG({
                    .fontSize = 14,
                    .textColor = UIGetClayColor(UIColor_SubText)
                }));
            }
        }

        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        }));

        if (!leaf && arrowClicked)
        {
            toggled = true;
        }
        else if (rowClicked && outSelected)
        {
            *outSelected = true;
        }
    }
    return toggled;
}

bool UICollapsingHeader(Clay_ElementId id, Clay_String label, bool open)
{
    bool clicked = false;

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26.0f) },
            .padding = { 6, 6, 0, 0 },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered) : UIPanelColor(),
        .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius))
    }) {
        clicked = UIClicked();
        CLAY_TEXT(open ? CLAY_STRING("-") : CLAY_STRING("+"), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = 16,
            .textColor = { 232, 164, 0, 255 },
            .wrapMode = CLAY_TEXT_WRAP_NONE
        }));
    }
    return clicked;
}

static u64 g_UIDropdownOpenId; // only one dropdown list is open at a time

bool UIDropdown(Clay_ElementId id, Clay_String label, const char** options, u32 numOptions, u32* selectedIndex)
{
    if (!options || numOptions == 0u || !selectedIndex) return false;
    if (*selectedIndex >= numOptions) *selectedIndex = 0u;

    bool open = g_UIDropdownOpenId == (u64)id.id;
    bool changed = false;

    Clay_ElementId boxId  = Clay_GetElementIdWithIndex(CLAY_STRING("UIDropdownBox"), id.id);
    Clay_ElementId listId = Clay_GetElementIdWithIndex(CLAY_STRING("UIDropdownList"), id.id);

    // close on escape or a click outside the box and the list, last frame's layout
    if (open && (GetMousePressed(MouseButton_Left) || GetKeyPressed(SDLK_ESCAPE)))
    {
        Clay_ElementData box  = Clay_GetElementData(boxId);
        Clay_ElementData list = Clay_GetElementData(listId);
        bool insideBox  = box.found  && RectPointIntersect((float2){ box.boundingBox.x, box.boundingBox.y },
                                                           (float2){ box.boundingBox.width, box.boundingBox.height }, g_UI.mouse) != 0u;
        bool insideList = list.found && RectPointIntersect((float2){ list.boundingBox.x, list.boundingBox.y },
                                                           (float2){ list.boundingBox.width, list.boundingBox.height }, g_UI.mouse) != 0u;
        if (GetKeyPressed(SDLK_ESCAPE) || (!insideBox && !insideList))
        {
            g_UIDropdownOpenId = 0u;
            open = false;
        }
    }

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}

        CLAY(boxId, {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(UIGetFloat(UIFloat_TextBoxWidth)), CLAY_SIZING_FIXED(26.0f) },
                .padding = { 8, 8, 0, 0 },
                .childGap = 6,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered) : UIGetClayColor(UIColor_TextBoxBG),
            .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
            // outside only, BORDER_ALL would draw separator lines between the children
            .border = { .color = open ? UIGetClayColor(UIColor_SelectedBorder) : UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) }
        }) {
            if (UIClicked())
            {
                open = !open;
                g_UIDropdownOpenId = open ? (u64)id.id : 0u;
            }

            CLAY_TEXT(UIStr(options[*selectedIndex]), CLAY_TEXT_CONFIG({
                .fontSize = (u16)Maxu32((u32)(14.0f * UIGetFloat(UIFloat_TextScale)), 1u),
                .textColor = UIGetClayColor(UIColor_Text),
                .wrapMode = CLAY_TEXT_WRAP_NONE
            }));
            CLAY(CLAY_ID_LOCAL("ArrowSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}
            CLAY_TEXT(open ? CLAY_STRING("-") : CLAY_STRING("+"), CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = UIGetClayColor(UIColor_SubText)
            }));

            if (open)
            {
                CLAY(listId, {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(UIGetFloat(UIFloat_TextBoxWidth)), CLAY_SIZING_FIT(0) },
                        .padding = { 4, 4, 4, 4 },
                        .childGap = 2,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .backgroundColor = UIColorToClay(UIGetColor(UIColor_Quad) | 0xFF000000u),
                    .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
                    .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) },
                    .floating = {
                        .zIndex = 125,
                        .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM },
                        .attachTo = CLAY_ATTACH_TO_PARENT
                    }
                }) {
                    for (u32 i = 0u; i < numOptions; i++)
                    {
                        Clay_ElementId rowId = Clay_GetElementIdWithIndex(CLAY_STRING("UIDropdownRow"), id.id + i + 1u);
                        bool isSelected = i == *selectedIndex;
                        CLAY(rowId, {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22.0f) },
                                .padding = { 6, 6, 0, 0 },
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                            },
                            .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered)
                                             : (isSelected ? UIColorToClay((UIGetColor(UIColor_SelectedBorder) & 0x00FFFFFFu) | 0x48000000u) : (Clay_Color){ 0 }),
                            .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
                        }) {
                            if (UIClicked())
                            {
                                changed = i != *selectedIndex;
                                *selectedIndex = i;
                                g_UIDropdownOpenId = 0u;
                            }
                            CLAY_TEXT(UIStr(options[i]), CLAY_TEXT_CONFIG({
                                .fontSize = (u16)Maxu32((u32)(14.0f * UIGetFloat(UIFloat_TextScale)), 1u),
                                .textColor = UIGetClayColor(UIColor_Text),
                                .wrapMode = CLAY_TEXT_WRAP_NONE
                            }));
                        }
                    }
                }
            }
        }
    }
    return changed;
}

static u64 g_UIMenuOpenId; // only one menu button list is open at a time

bool UIMenuButtonIsOpen(Clay_ElementId id)
{
    return g_UIMenuOpenId == (u64)id.id;
}

// a tab-bar style button that drops a checklist menu of items. unlike UIDropdown it has no
// persistent selection: each click toggles an item and the menu stays open so several entries
// can be toggled in a row. returns the index clicked this frame, or -1. the caller flips its
// own state and updates item->checked for the highlight shown next frame.
s32 UIMenuButton(Clay_ElementId id, Clay_String label, Clay_Dimensions size, const UIMenuItem* items, u32 count)
{
    s32 clickedIndex = -1;
    bool open = g_UIMenuOpenId == (u64)id.id;

    Clay_ElementId listId = Clay_GetElementIdWithIndex(CLAY_STRING("UIMenuList"), id.id);

    // close on escape or a click outside the button and the list, last frame's layout
    if (open && (GetMousePressed(MouseButton_Left) || GetKeyPressed(SDLK_ESCAPE)))
    {
        Clay_ElementData box  = Clay_GetElementData(id);
        Clay_ElementData list = Clay_GetElementData(listId);
        bool insideBox  = box.found  && RectPointIntersect((float2){ box.boundingBox.x, box.boundingBox.y },
                                                           (float2){ box.boundingBox.width, box.boundingBox.height }, g_UI.mouse) != 0u;
        bool insideList = list.found && RectPointIntersect((float2){ list.boundingBox.x, list.boundingBox.y },
                                                           (float2){ list.boundingBox.width, list.boundingBox.height }, g_UI.mouse) != 0u;
        if (GetKeyPressed(SDLK_ESCAPE) || (!insideBox && !insideList))
        {
            g_UIMenuOpenId = 0u;
            open = false;
        }
    }

    // position the list under the button in root space. attaching to the parent would make
    // the list inherit the (short) tab-bar clip rect, which scissors the rows' text away while
    // the background quad (drawn with a full scissor) still shows. clamp it onto the screen too.
    float2 menuPos = { 0.0f, size.height };
    {
        Clay_ElementData box = Clay_GetElementData(id);
        if (box.found) menuPos = (float2){ box.boundingBox.x, box.boundingBox.y + box.boundingBox.height };
        f32 menuW = 168.0f;
        f32 menuH = 8.0f + (f32)count * 26.0f;
        if (menuPos.x + menuW > g_UI.screenSize.x) menuPos.x = g_UI.screenSize.x - menuW;
        if (menuPos.y + menuH > g_UI.screenSize.y) menuPos.y = g_UI.screenSize.y - menuH;
        if (menuPos.x < 0.0f) menuPos.x = 0.0f;
        if (menuPos.y < 0.0f) menuPos.y = 0.0f;
    }

    float radius = UIFloatStackZero(UIFloat_CornerRadius) ? size.height * 0.5f : UIGetFloat(UIFloat_CornerRadius);
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(size.width), CLAY_SIZING_FIXED(size.height) },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = UIButtonColor(Clay_Hovered(), open),
        .cornerRadius = CLAY_CORNER_RADIUS(radius),
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(UIGetFloat(UIFloat_BorderWidth)) }
    }) {
        if (UIClicked())
        {
            open = !open;
            g_UIMenuOpenId = open ? (u64)id.id : 0u;
        }
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(17.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        if (open && items && count != 0u)
        {
            CLAY(listId, {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(160.0f), CLAY_SIZING_FIT(0) },
                    .padding = { 4, 4, 4, 4 },
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = UIColorToClay(UIGetColor(UIColor_Quad) | 0xFF000000u),
                .cornerRadius = CLAY_CORNER_RADIUS(6.0f),
                .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) },
                .floating = {
                    .offset = { menuPos.x, menuPos.y },
                    .zIndex = 125,
                    .attachTo = CLAY_ATTACH_TO_ROOT
                }
            }) {
                for (u32 i = 0u; i < count; i++)
                {
                    Clay_ElementId rowId = Clay_GetElementIdWithIndex(CLAY_STRING("UIMenuRow"), id.id + i + 1u);
                    bool checked = items[i].checked;
                    CLAY(rowId, {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24.0f) },
                            .padding = { 8, 8, 0, 0 },
                            .childGap = 6,
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered)
                                         : (checked ? UIColorToClay((UIGetColor(UIColor_SelectedBorder) & 0x00FFFFFFu) | 0x48000000u) : (Clay_Color){ 0 }),
                        .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
                    }) {
                        if (UIClicked()) clickedIndex = (s32)i;
                        // fixed-width check slot. never draw a space-only glyph here: whitespace
                        // text produces no vertices but still consumes the slug batch's "force new
                        // batch" flag, which makes the following label merge into the previous
                        // (lower z) text batch and render underneath this menu's panel.
                        CLAY(Clay_GetElementIdWithIndex(CLAY_STRING("UIMenuCheck"), id.id + i + 1u), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(14.0f), CLAY_SIZING_GROW(0) },
                                        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } }
                        }) {
                            if (checked)
                                CLAY_TEXT(CLAY_STRING("\xE2\x9C\x93"), CLAY_TEXT_CONFIG({
                                    .fontSize = 14,
                                    .textColor = UIGetClayColor(UIColor_SelectedBorder)
                                }));
                        }
                        CLAY_TEXT(UIStr(items[i].label), CLAY_TEXT_CONFIG({
                            .fontSize = 14,
                            .textColor = UIGetClayColor(UIColor_Text),
                            .wrapMode = CLAY_TEXT_WRAP_NONE
                        }));
                    }
                }
            }
        }
    }
    return clickedIndex;
}

bool UISliderFloat(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue)
{
    if (!value || !IsFiniteF32(minValue) || !IsFiniteF32(maxValue) || maxValue <= minValue) return false;

    bool changed = false;
    f32 currentValue = IsFiniteF32(*value) ? *value : minValue;
    f32 clampedValue = Clampf32(currentValue, minValue, maxValue);
    if (clampedValue != *value)
    {
        *value = clampedValue;
        changed = true;
    }

    Clay_ElementId trackId = Clay_GetElementIdWithIndex(CLAY_STRING("UISliderTrack"), id.id);
    Clay_ElementData trackData = Clay_GetElementData(trackId);
    u64 activeId = (u64)id.id;
    if (trackData.found && Clay_PointerOver(trackId) && GetMousePressed(MouseButton_Left))
    {
        g_UI.active = activeId;
    }
    if (trackData.found && g_UI.active == activeId && GetMouseDown(MouseButton_Left))
    {
        f32 t = (g_UI.mouse.x - trackData.boundingBox.x) / Maxf32(trackData.boundingBox.width, 1.0f);
        f32 newValue = Clampf32(minValue + t * (maxValue - minValue), minValue, maxValue);
        if (newValue != *value)
        {
            *value = newValue;
            changed = true;
        }
    }

    f32 value01 = Saturatef32((Clampf32(*value, minValue, maxValue) - minValue) / (maxValue - minValue));
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
        }) {}

        f32 knobOffset = value01 * Maxf32(trackData.found ? trackData.boundingBox.width : 0.0f, 0.0f);
        CLAY(trackId, {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(UIGetFloat(UIFloat_TextBoxWidth)), CLAY_SIZING_FIXED(14.0f) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = UIGetClayColor(UIColor_TextBoxBG),
            .cornerRadius = CLAY_CORNER_RADIUS(7.0f),
        }) {
            CLAY(CLAY_ID_LOCAL("Fill"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(knobOffset), CLAY_SIZING_FIXED(14.0f) } },
                .backgroundColor = UIGetClayColor(UIColor_SliderInside),
                .cornerRadius = CLAY_CORNER_RADIUS(7.0f)
            }) {}

            CLAY(CLAY_ID_LOCAL("Remainder"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
            }) {}
        }
    }

    return changed;
}

bool UISliderFloatValue(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue, int decimals)
{
    if (!value) return false;

    u32 index = g_UISliderValueLabelIndex++ % (u32)(sizeof(g_UISliderValueLabels) / sizeof(g_UISliderValueLabels[0]));
    char* text = g_UISliderValueLabels[index];
    u32 capacity = (u32)sizeof(g_UISliderValueLabels[0]);
    u32 length = 0u;

    u32 labelLength = (u32)Maxs32(label.length, 0);
    for (u32 i = 0u; i < labelLength && length + 1u < capacity; i++) text[length++] = label.chars[i];
    if (length + 2u < capacity)
    {
        text[length++] = ':';
        text[length++] = ' ';
    }

    f32 displayValue = IsFiniteF32(*value) ? *value : minValue;
    if (IsFiniteF32(minValue) && IsFiniteF32(maxValue) && maxValue > minValue) displayValue = Clampf32(displayValue, minValue, maxValue);
    if (length < capacity)
    {
        length += (u32)FloatToString(text + length, displayValue, Maxs32(decimals, 0));
        if (length >= capacity) length = capacity - 1u;
    }
    text[length] = '\0';

    Clay_String valueLabel = { .isStaticallyAllocated = false, .length = (s32)length, .chars = text };
    return UISliderFloat(id, valueLabel, value, minValue, maxValue);
}

typedef struct UIEditSlot_
{
    u64 id;
    char buffer[32];
    f32 lastValue;
    f32 dragAccum; // UIEditInt: leftover shift+drag pixels below the per-step threshold
    UITextAreaCustomData textData;
} UIEditSlot;

static UIEditSlot* UIGetEditSlot(Clay_ElementId id)
{
    static UIEditSlot slots[64];
    UIEditSlot* empty = NULL;
    u64 editId = (u64)id.id;

    for (u32 i = 0u; i < (u32)(sizeof(slots) / sizeof(slots[0])); i++)
    {
        if (slots[i].id == editId) return &slots[i];
        if (!slots[i].id && !empty) empty = &slots[i];
    }

    if (!empty)
    {
        AX_WARN("UI numeric edit slot limit reached");
        return NULL;
    }

    empty->id = editId;
    empty->buffer[0] = '\0';
    empty->lastValue = FLT_MAX;
    empty->dragAccum = 0.0f;
    return empty;
}

static bool UIEditSlotFocused(const UIEditSlot* slot)
{
    return slot && g_UI.keyboardFocus == UIAutoID(slot->buffer);
}

static void UIFormatInt(char* buffer, u32 capacity, s32 value)
{
    if (!buffer || capacity == 0u) return;
    int len = IntToString(buffer, (int64_t)value, 0);
    if ((u32)len >= capacity) len = (int)capacity - 1;
    buffer[len] = '\0';
}

static void UIFormatFloat(char* buffer, u32 capacity, f32 value, int decimals)
{
    if (!buffer || capacity == 0u) return;
    int len = FloatToString(buffer, value, Maxs32(decimals, 0));
    if ((u32)len >= capacity) len = (int)capacity - 1;
    buffer[len] = '\0';
}

static bool UIFilterNumericBuffer(char* buffer, u32 capacity, bool allowFloat)
{
    if (!buffer || capacity == 0u) return false;

    bool changed = false;
    bool hasDot = false;
    u32 write = 0u;
    for (u32 read = 0u; read + 1u < capacity && buffer[read]; read++)
    {
        char c = buffer[read];
        bool keep = false;
        if (c >= '0' && c <= '9') keep = true;
        else if (c == '-' && write == 0u) keep = true;
        else if (allowFloat && c == '.' && !hasDot) { keep = true; hasDot = true; }

        if (keep) buffer[write++] = c;
        else changed = true;
    }

    changed |= buffer[write] != '\0';
    buffer[write] = '\0';
    return changed;
}

static bool UIParseIntBuffer(const char* buffer, s32* outValue)
{
    if (!buffer || !outValue) return false;

    s32 sign = 1;
    u32 i = 0u;
    if (buffer[i] == '-') { sign = -1; i++; }

    bool hasDigits = false;
    s32 value = 0;
    for (; buffer[i]; i++)
    {
        char c = buffer[i];
        if (c < '0' || c > '9') return false;
        hasDigits = true;
        value = value * 10 + (s32)(c - '0');
    }

    if (!hasDigits) return false;
    *outValue = value * sign;
    return true;
}

static bool UIParseFloatBuffer(const char* buffer, f32* outValue)
{
    if (!buffer || !outValue) return false;

    f32 sign = 1.0f;
    u32 i = 0u;
    if (buffer[i] == '-') { sign = -1.0f; i++; }

    bool hasDigits = false;
    f32 value = 0.0f;
    for (; buffer[i] >= '0' && buffer[i] <= '9'; i++)
    {
        hasDigits = true;
        value = value * 10.0f + (f32)(buffer[i] - '0');
    }

    if (buffer[i] == '.')
    {
        f32 scale = 0.1f;
        i++;
        for (; buffer[i] >= '0' && buffer[i] <= '9'; i++)
        {
            hasDigits = true;
            value += (f32)(buffer[i] - '0') * scale;
            scale *= 0.1f;
        }
    }

    if (buffer[i] || !hasDigits) return false;
    *outValue = value * sign;
    return true;
}

bool UIEditInt(Clay_ElementId id, Clay_String label, f32* value, s32 minValue, s32 maxValue)
{
    if (!value || maxValue < minValue) return false;

    UIEditSlot* slot = UIGetEditSlot(id);
    if (!slot) return false;

    bool changed = false;
    bool textEdited = slot->textData.edited != 0u;
    slot->textData.edited = 0u;
    f32 dragDelta = slot->textData.dragDeltaX;
    slot->textData.dragDeltaX = 0.0f;
    s32 current = Clamps32((s32)(*value), minValue, maxValue);
    if (dragDelta != 0.0f)
    {
        const f32 pixelsPerStep = 4.0f;
        slot->dragAccum += dragDelta;
        while (slot->dragAccum >= pixelsPerStep) { current = Clamps32(current + 1, minValue, maxValue); slot->dragAccum -= pixelsPerStep; }
        while (slot->dragAccum <= -pixelsPerStep) { current = Clamps32(current - 1, minValue, maxValue); slot->dragAccum += pixelsPerStep; }
        *value = (f32)current;
        changed = true;
    }
    if ((f32)current != *value)
    {
        *value = (f32)current;
        changed = true;
    }

    bool focused = UIEditSlotFocused(slot);
    bool refreshed = false;
    if (slot->buffer[0] == '\0' || slot->lastValue != *value)
    {
        UIFormatInt(slot->buffer, (u32)sizeof(slot->buffer), current);
        slot->lastValue = *value;
        refreshed = true;
    }

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
        }) {}

        if (UIButton(CLAY_ID_LOCAL("Dec"), CLAY_STRING("<"), (Clay_Dimensions){ 28.0f, 28.0f }, false))
        {
            current = Clamps32(current - 1, minValue, maxValue);
            *value = (f32)current;
            UIFormatInt(slot->buffer, (u32)sizeof(slot->buffer), current);
            changed = true;
        }

        slot->textData.type = UICustomType_TextArea;
        slot->textData.buffer = slot->buffer;
        slot->textData.capacity = sizeof(slot->buffer);
        slot->textData.flags = UITextAreaFlags_CenterX | UITextAreaFlags_CenterY | UITextAreaFlags_NoWrap | UITextAreaFlags_Clip | UITextAreaFlags_NumericDrag;
        CLAY(CLAY_ID_LOCAL("Text"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(72.0f), CLAY_SIZING_FIXED(28.0f) } },
            .custom = { .customData = &slot->textData }
        }) {}

        if (UIButton(CLAY_ID_LOCAL("Inc"), CLAY_STRING(">"), (Clay_Dimensions){ 28.0f, 28.0f }, false))
        {
            current = Clamps32(current + 1, minValue, maxValue);
            *value = (f32)current;
            UIFormatInt(slot->buffer, (u32)sizeof(slot->buffer), current);
            changed = true;
        }
    }

    UIFilterNumericBuffer(slot->buffer, (u32)sizeof(slot->buffer), false);
    s32 parsed;
    if ((focused || textEdited) && !refreshed && UIParseIntBuffer(slot->buffer, &parsed))
    {
        parsed = Clamps32(parsed, minValue, maxValue);
        if ((f32)parsed != *value)
        {
            *value = (f32)parsed;
            changed = true;
        }
        slot->lastValue = *value;
    }

    return changed;
}

bool UIEditFloat(Clay_ElementId id, Clay_String label, f32* value, f32 minValue, f32 maxValue, f32 step, int decimals)
{
    if (!value || !IsFiniteF32(minValue) || !IsFiniteF32(maxValue) || maxValue < minValue) return false;

    UIEditSlot* slot = UIGetEditSlot(id);
    if (!slot) return false;

    bool changed = false;
    bool textEdited = slot->textData.edited != 0u;
    slot->textData.edited = 0u;
    f32 dragDelta = slot->textData.dragDeltaX;
    slot->textData.dragDeltaX = 0.0f;
    if (dragDelta != 0.0f && IsFiniteF32(*value))
    {
        *value = Clampf32(*value + dragDelta * step, minValue, maxValue);
        changed = true;
    }
    f32 current = Clampf32(IsFiniteF32(*value) ? *value : minValue, minValue, maxValue);
    if (current != *value)
    {
        *value = current;
        changed = true;
    }

    bool focused = UIEditSlotFocused(slot);
    bool refreshed = false;
    if (slot->buffer[0] == '\0' || slot->lastValue != *value)
    {
        UIFormatFloat(slot->buffer, (u32)sizeof(slot->buffer), current, decimals);
        slot->lastValue = *value;
        refreshed = true;
    }

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
        }) {}

        if (UIButton(CLAY_ID_LOCAL("Dec"), CLAY_STRING("<"), (Clay_Dimensions){ 28.0f, 28.0f }, false))
        {
            *value = Clampf32(*value - step, minValue, maxValue);
            UIFormatFloat(slot->buffer, (u32)sizeof(slot->buffer), *value, decimals);
            changed = true;
        }

        slot->textData.type = UICustomType_TextArea;
        slot->textData.buffer = slot->buffer;
        slot->textData.capacity = sizeof(slot->buffer);
        slot->textData.flags = UITextAreaFlags_CenterX | UITextAreaFlags_CenterY | UITextAreaFlags_NoWrap | UITextAreaFlags_Clip | UITextAreaFlags_NumericDrag;
        CLAY(CLAY_ID_LOCAL("Text"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(82.0f), CLAY_SIZING_FIXED(28.0f) } },
            .custom = { .customData = &slot->textData }
        }) {}

        if (UIButton(CLAY_ID_LOCAL("Inc"), CLAY_STRING(">"), (Clay_Dimensions){ 28.0f, 28.0f }, false))
        {
            *value = Clampf32(*value + step, minValue, maxValue);
            UIFormatFloat(slot->buffer, (u32)sizeof(slot->buffer), *value, decimals);
            changed = true;
        }
    }

    UIFilterNumericBuffer(slot->buffer, (u32)sizeof(slot->buffer), true);
    f32 parsed;
    if ((focused || textEdited) && !refreshed && UIParseFloatBuffer(slot->buffer, &parsed))
    {
        parsed = Clampf32(parsed, minValue, maxValue);
        if (parsed != *value)
        {
            *value = parsed;
            changed = true;
        }
        slot->lastValue = *value;
    }

    return changed;
}

// one text box of a multi component edit row. out: true when the value changed
static bool UIEditComponent(Clay_ElementId compId, f32* value, f32 minValue, f32 maxValue,
                            int decimals, bool isFloat, f32 boxWidth)
{
    UIEditSlot* slot = UIGetEditSlot(compId);
    if (!slot) return false;

    bool changed = false;
    bool textEdited = slot->textData.edited != 0u;
    slot->textData.edited = 0u;
    f32 current = Clampf32(IsFiniteF32(*value) ? *value : minValue, minValue, maxValue);
    if (!isFloat) current = (f32)(s32)current;
    if (current != *value)
    {
        *value = current;
        changed = true;
    }

    bool focused = UIEditSlotFocused(slot);
    bool refreshed = false;
    if (slot->buffer[0] == '\0' || slot->lastValue != *value)
    {
        if (isFloat) UIFormatFloat(slot->buffer, (u32)sizeof(slot->buffer), current, decimals);
        else         UIFormatInt(slot->buffer, (u32)sizeof(slot->buffer), (s32)current);
        slot->lastValue = *value;
        refreshed = true;
    }

    slot->textData.type = UICustomType_TextArea;
    slot->textData.buffer = slot->buffer;
    slot->textData.capacity = sizeof(slot->buffer);
    slot->textData.flags = UITextAreaFlags_CenterX | UITextAreaFlags_CenterY | UITextAreaFlags_NoWrap | UITextAreaFlags_Clip;
    CLAY(compId, {
        .layout = { .sizing = { CLAY_SIZING_FIXED(boxWidth), CLAY_SIZING_FIXED(28.0f) } },
        .custom = { .customData = &slot->textData }
    }) {}

    UIFilterNumericBuffer(slot->buffer, (u32)sizeof(slot->buffer), isFloat);
    f32 parsed;
    bool parsedOk;
    if (isFloat)
    {
        parsedOk = UIParseFloatBuffer(slot->buffer, &parsed);
    }
    else
    {
        s32 parsedInt;
        parsedOk = UIParseIntBuffer(slot->buffer, &parsedInt);
        parsed = (f32)parsedInt;
    }
    if ((focused || textEdited) && !refreshed && parsedOk)
    {
        parsed = Clampf32(parsed, minValue, maxValue);
        if (!isFloat) parsed = (f32)(s32)parsed;
        if (parsed != *value)
        {
            *value = parsed;
            changed = true;
        }
        slot->lastValue = *value;
    }
    return changed;
}

bool UIEditFloatN(Clay_ElementId id, Clay_String label, f32* values, u32 numComponents,
                  f32 minValue, f32 maxValue, int decimals)
{
    if (!values || numComponents == 0u || numComponents > 4u ||
        !IsFiniteF32(minValue) || !IsFiniteF32(maxValue) || maxValue < minValue) return false;

    bool changed = false;
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
        }) {}

        for (u32 i = 0u; i < numComponents; i++)
        {
            Clay_ElementId compId = Clay_GetElementIdWithIndex(CLAY_STRING("UIEditFloatNComp"), id.id + i);
            changed |= UIEditComponent(compId, &values[i], minValue, maxValue, decimals, true, 75.0f);
        }
    }
    return changed;
}

bool UIEditIntN(Clay_ElementId id, Clay_String label, s32* values, u32 numComponents,
                s32 minValue, s32 maxValue)
{
    if (!values || numComponents == 0u || numComponents > 4u || maxValue < minValue) return false;

    bool changed = false;
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } }
        }) {}

        for (u32 i = 0u; i < numComponents; i++)
        {
            Clay_ElementId compId = Clay_GetElementIdWithIndex(CLAY_STRING("UIEditIntNComp"), id.id + i);
            f32 value = (f32)values[i];
            changed |= UIEditComponent(compId, &value, (f32)minValue, (f32)maxValue, 0, false, 75.0f);
            values[i] = (s32)value;
        }
    }
    return changed;
}

/*//////////////////////////////////////////////////////////////////////////*/
/*                               Color Edit                                 */
/*//////////////////////////////////////////////////////////////////////////*/

#include "Shaders/spv/UI/ColorPickCompute.spv.h"
#include "Math/Color.h"

extern SDL_GPUDevice* g_GPUDevice;

// hsv picker state, one picker open at a time like the dropdowns
static u64    g_UIColorEditOpenId;
static float3 g_UIColorEditHSV;

// the picker visuals come from the old engine's color pick shader, rendered into a
// small texture whenever the hsv selection changes
enum { UIColorPickWidth = 256, UIColorPickHeight = 176 };
static SDL_GPUComputePipeline* g_UIColorPickPipeline;
static SDL_GPUTexture* g_UIColorPickTexture;
static UIImageData g_UIColorPickImage;

static void UIColorPickRender(void)
{
    if (!g_UIColorPickPipeline)
    {
        g_UIColorPickPipeline = SDL_CreateGPUComputePipeline(g_GPUDevice, &(SDL_GPUComputePipelineCreateInfo){
            .code = Shaders_UI_ColorPickCompute_spv,
            .code_size = sizeof(Shaders_UI_ColorPickCompute_spv),
            .entrypoint = AX_GPU_COMPUTE_ENTRYPOINT,
            .format = AX_GPU_SHADER_FORMAT,
            .num_readwrite_storage_textures = 1,
            .num_uniform_buffers = 1,
            .threadcount_x = 8,
            .threadcount_y = 8,
            .threadcount_z = 1,
        });
        if (!g_UIColorPickPipeline)
        {
            AX_ERROR("color pick pipeline creation failed");
            return;
        }
        g_UIColorPickTexture = CreateTexture2D(UIColorPickWidth, UIColorPickHeight, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                               SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                                               SDL_GPU_SAMPLECOUNT_1, 1u, "UIColorPick");
        g_UIColorPickImage = (UIImageData){ .texture = g_UIColorPickTexture };
    }
    if (!g_UIColorPickTexture) return;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_GPUDevice);
    SDL_GPUStorageTextureReadWriteBinding rwTexture = { .texture = g_UIColorPickTexture };
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &rwTexture, 1, NULL, 0);
    SDL_BindGPUComputePipeline(pass, g_UIColorPickPipeline);
    struct { f32 hsv[3]; f32 pad0; f32 size[2]; f32 pad1[2]; } params = {
        { g_UIColorEditHSV.x, g_UIColorEditHSV.y, g_UIColorEditHSV.z }, 0.0f,
        { (f32)UIColorPickWidth, (f32)UIColorPickHeight }, { 0.0f, 0.0f }
    };
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, (UIColorPickWidth + 7u) / 8u, (UIColorPickHeight + 7u) / 8u, 1);
    SDL_EndGPUComputePass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
}

// drag interaction over an element's last frame bounding box.
// out: true while dragging, outX/outY are 0..1 inside the box
static bool UIColorDragBox(Clay_ElementId boxId, f32* outX, f32* outY)
{
    if (!GetMouseDown(MouseButton_Left)) return false;
    Clay_ElementData box = Clay_GetElementData(boxId);
    if (!box.found) return false;
    if (!RectPointIntersect((float2){ box.boundingBox.x, box.boundingBox.y },
                            (float2){ box.boundingBox.width, box.boundingBox.height }, g_UI.mouse))
        return false;
    *outX = Clampf32((g_UI.mouse.x - box.boundingBox.x) / Maxf32(box.boundingBox.width, 1.0f), 0.0f, 1.0f);
    *outY = Clampf32((g_UI.mouse.y - box.boundingBox.y) / Maxf32(box.boundingBox.height, 1.0f), 0.0f, 1.0f);
    return true;
}

bool UIColorEdit3(Clay_ElementId id, Clay_String label, f32* rgb)
{
    if (!rgb) return false;

    bool open = g_UIColorEditOpenId == (u64)id.id;
    bool changed = false;

    Clay_ElementId swatchId = Clay_GetElementIdWithIndex(CLAY_STRING("UIColorSwatch"), id.id);
    Clay_ElementId panelId  = Clay_GetElementIdWithIndex(CLAY_STRING("UIColorPanel"), id.id);
    Clay_ElementId pickId   = Clay_GetElementIdWithIndex(CLAY_STRING("UIColorPick"), id.id);

    // close on escape or a click outside the swatch and the panel, last frame's layout
    if (open && (GetMousePressed(MouseButton_Left) || GetKeyPressed(SDLK_ESCAPE)))
    {
        Clay_ElementData swatch = Clay_GetElementData(swatchId);
        Clay_ElementData panel  = Clay_GetElementData(panelId);
        bool insideSwatch = swatch.found && RectPointIntersect((float2){ swatch.boundingBox.x, swatch.boundingBox.y },
                                                               (float2){ swatch.boundingBox.width, swatch.boundingBox.height }, g_UI.mouse) != 0u;
        bool insidePanel  = panel.found && RectPointIntersect((float2){ panel.boundingBox.x, panel.boundingBox.y },
                                                              (float2){ panel.boundingBox.width, panel.boundingBox.height }, g_UI.mouse) != 0u;
        if (GetKeyPressed(SDLK_ESCAPE) || (!insideSwatch && !insidePanel))
        {
            g_UIColorEditOpenId = 0u;
            open = false;
        }
    }

    if (open)
    {
        // the picker edits the hsv cache, the color follows. the regions match the
        // shader layout: bottom 12% is the hue strip, the left 88% is the sv field
        f32 dragX, dragY;
        if (UIColorDragBox(pickId, &dragX, &dragY))
        {
            if (dragY > 0.88f)
            {
                g_UIColorEditHSV.x = dragX;
            }
            else if (dragX < 0.88f)
            {
                g_UIColorEditHSV.y = dragX / 0.88f;
                g_UIColorEditHSV.z = 1.0f - dragY / 0.88f;
            }
            HSVToRGB(g_UIColorEditHSV, rgb);
            UIColorPickRender();
            changed = true;
        }
    }

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .padding = { 0, 4, 0, 0 },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontSize = (u16)Maxu32((u32)(15.0f * UIGetFloat(UIFloat_TextScale)), 1u),
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}

        CLAY(swatchId, {
            .layout = { .sizing = { CLAY_SIZING_FIXED(64.0f), CLAY_SIZING_FIXED(24.0f) } },
            .backgroundColor = { Clampf32(rgb[0], 0.0f, 1.0f) * 255.0f,
                                 Clampf32(rgb[1], 0.0f, 1.0f) * 255.0f,
                                 Clampf32(rgb[2], 0.0f, 1.0f) * 255.0f, 255.0f },
            .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
            .border = { .color = open ? UIGetClayColor(UIColor_SelectedBorder) : UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) }
        }) {
            if (UIClicked())
            {
                open = !open;
                g_UIColorEditOpenId = open ? (u64)id.id : 0u;
                if (open)
                {
                    g_UIColorEditHSV = RGBToHSV((float3){ rgb[0], rgb[1], rgb[2] });
                    UIColorPickRender();
                }
            }

            if (open)
            {
                CLAY(panelId, {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(UIColorPickWidth + 16.0f), CLAY_SIZING_FIT(0) },
                        .padding = { 8, 8, 8, 8 },
                        .childGap = 6,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .backgroundColor = UIColorToClay(UIGetColor(UIColor_Quad) | 0xFF000000u),
                    .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
                    .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) },
                    .floating = {
                        .zIndex = 125,
                        .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM },
                        .attachTo = CLAY_ATTACH_TO_PARENT
                    }
                }) {
                    // sv field, hue strip and selection indicator all live in the shader
                    // rendered texture (the old engine's color pick shader)
                    CLAY(pickId, {
                        .layout = { .sizing = { CLAY_SIZING_FIXED((f32)UIColorPickWidth), CLAY_SIZING_FIXED((f32)UIColorPickHeight) } },
                        .image = { .imageData = &g_UIColorPickImage },
                        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) }
                    }) {}

                    Clay_ElementId rgbRowId = Clay_GetElementIdWithIndex(CLAY_STRING("UIColorRGBRow"), id.id);
                    if (UIEditFloatN(rgbRowId, CLAY_STRING("RGB"), rgb, 3u, 0.0f, 1.0f, 3))
                    {
                        g_UIColorEditHSV = RGBToHSV((float3){ rgb[0], rgb[1], rgb[2] });
                        UIColorPickRender();
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

void UISetColor(UIColor what, u32 color)
{
    if ((u32)what < UIColor_Count) g_UI.colors[what] = color;
}

u32 UIGetColor(UIColor what)
{
    if ((u32)what >= UIColor_Count) return 0xFFFFFFFFu;
    s32 count = g_UI.colorStackCount[what];
    return count >= 0 ? g_UI.colorStack[what][count] : g_UI.colors[what];
}

Clay_Color UIGetClayColor(UIColor what)
{
    return UIColorToClay(UIGetColor(what));
}

void UIPushColor(UIColor what, u32 color)
{
    if ((u32)what >= UIColor_Count) return;
    s32 count = g_UI.colorStackCount[what] + 1;
    if ((u32)count >= UI_STACK_SIZE) { AX_WARN("UI color stack full: %u", (u32)what); return; }
    g_UI.colorStackCount[what] = count;
    g_UI.colorStack[what][count] = color;
}

void UIPopColor(UIColor what)
{
    if ((u32)what >= UIColor_Count) return;
    if (g_UI.colorStackCount[what] < 0) { AX_WARN("UI color stack underflow: %u", (u32)what); return; }
    g_UI.colorStackCount[what]--;
}

void UISetFloat(UIFloat what, f32 value)
{
    if ((u32)what < UIFloat_Count) g_UI.floats[what] = value;
}

f32 UIGetFloat(UIFloat what)
{
    if ((u32)what >= UIFloat_Count) return 0.0f;
    s32 count = g_UI.floatStackCount[what];
    return count >= 0 ? g_UI.floatStack[what][count] : g_UI.floats[what];
}

void UIPushFloat(UIFloat what, f32 value)
{
    if ((u32)what >= UIFloat_Count) return;
    s32 count = g_UI.floatStackCount[what] + 1;
    if ((u32)count >= UI_STACK_SIZE) { AX_WARN("UI float stack full: %u", (u32)what); return; }
    g_UI.floatStackCount[what] = count;
    g_UI.floatStack[what][count] = value;
}

void UIPushFloatAdd(UIFloat what, f32 value)
{
    UIPushFloat(what, UIGetFloat(what) + value);
}

void UIPopFloat(UIFloat what)
{
    if ((u32)what >= UIFloat_Count) return;
    if (g_UI.floatStackCount[what] < 0) { AX_WARN("UI float stack underflow: %u", (u32)what); return; }
    g_UI.floatStackCount[what]--;
}

bool UIFloatStackZero(UIFloat what)
{
    return g_UI.floatStackCount[what] == 0;
}

UIImageData UIImageFromTexture(Texture* texture)
{
    UIImageData image = {0};
    static int warnCount = 0;
    image.uv[2] = 1.0f;
    image.uv[3] = 1.0f;
    if (!texture || !texture->handle )
    {
        if (warnCount++ < 8) AX_WARN("UIImageFromTexture got null texture");
        image.texture = NULL;
    }
    else image.texture = texture->handle;
    return image;
}

void UIRenderCommands(Clay_RenderCommandArray* commands)
{
    if (!g_UILayout.initialized || !commands || commands->length <= 0) return;

    s32 currentZ = Clay_RenderCommandArray_Get(commands, 0)->zIndex;
    s32 batchStarted = false;

    for (s32 i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand* command = Clay_RenderCommandArray_Get(commands, i);
        s32 commandCanDraw = (command->commandType != CLAY_RENDER_COMMAND_TYPE_SCISSOR_END) &
                             (command->commandType != CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START) &
                             (command->commandType != CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END);
        if (command->zIndex != currentZ)
        {
            if (batchStarted) UIEndBatch();
            currentZ = command->zIndex;
            batchStarted = false;
        }
        if (commandCanDraw && !batchStarted)
        {
            UIBeginBatch();
            batchStarted = true;
        }
        switch (command->commandType)
        {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: UIRenderLayoutRectangle(command); break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER:    UIRenderLayoutBorder(command); break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT:      UIRenderLayoutText(command); break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                UIRenderLayoutScrollBar(command);
                UIPushClipRect((float2){ command->boundingBox.x, command->boundingBox.y }, (float2){ command->boundingBox.width, command->boundingBox.height });
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                UIPopClipRect();
                break;
            case CLAY_RENDER_COMMAND_TYPE_IMAGE:     UIRenderLayoutImage(command); break;
            case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                UIRenderLayoutCustom(command);
                break;
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
                if (!g_UILayout.warnedOverlay)
                {
                    AX_WARN("Clay overlay color commands are not rendered yet");
                    g_UILayout.warnedOverlay = true;
                }
                break;
            case CLAY_RENDER_COMMAND_TYPE_NONE:
            default:
                break;
        }
    }
    if (batchStarted) UIEndBatch();
}


static SDL_GPURenderPass* UIBeginPassIfNeeded(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget, SDL_GPURenderPass* pass)
{
    return pass ? pass : SDL_BeginGPURenderPass(cmd, colorTarget, 1, NULL);
}

static void UISetFullScissor(SDL_GPURenderPass* pass)
{
    SDL_SetGPUScissor(pass, &(SDL_Rect){ 0, 0, (int)Maxu32(g_WindowState.prev_width, 1u), (int)Maxu32(g_WindowState.prev_height, 1u) });
}

static void UIDrawShapeRange(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, UIParams* params, u32 firstShape, u32 shapeCount, bool* shapePipelineBound)
{
    if (shapeCount == 0u) return;
    UISetFullScissor(pass);
    if (!*shapePipelineBound)
    {
        SDL_BindGPUGraphicsPipeline(pass, g_RenderState.uiShapePipeline);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &g_RenderState.uiShapeBuffer, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, params, sizeof(*params));
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof(*params));
        *shapePipelineBound = true;
    }
    SDL_DrawGPUPrimitives(pass, 6, shapeCount, 0, firstShape);
}

static void UIDrawImageCommand(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, UIImageCommand* image, UIParams* params)
{
    UISetFullScissor(pass);
    SDL_BindGPUGraphicsPipeline(pass, g_RenderState.uiImagePipeline);

    UIImageParams imageParams = {0};
    MemCopy(imageParams.screenScale, params->screenScale, sizeof(imageParams.screenScale));
    MemCopy(imageParams.rect, image->rect, sizeof(imageParams.rect));
    MemCopy(imageParams.uv, image->uv, sizeof(imageParams.uv));
    MemCopy(imageParams.clip, image->clip, sizeof(imageParams.clip));
    imageParams.tintColor = image->tintColor;
    imageParams.radius = image->radius;

    SDL_GPUTextureSamplerBinding binding = {
        .texture = image->texture,
        .sampler = image->sampler ? image->sampler : g_RenderState.sampler
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &imageParams, sizeof(imageParams));
    SDL_PushGPUFragmentUniformData(cmd, 0, &imageParams, sizeof(imageParams));
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

void UIRender(SDL_GPUCommandBuffer* cmd, SDL_GPUColorTargetInfo* colorTarget)
{
    UIEndBatch();
    if (g_UIRenderer.count == 0u && g_UIRenderer.imageCount == 0u && g_UIRenderer.textCount == 0u && g_UIRenderer.batchCount == 0u) return;
    if (g_UIRenderer.count > 0u && (!g_RenderState.uiShapePipeline || !g_RenderState.uiShapeBuffer))
    {
        AX_WARN("UI render skipped, resources not initialized");
        UIClear();
        return;
    }

    UIParams params = {0};
    params.screenScale[0] = (f32)Maxu32(g_WindowState.prev_width, 1u);
    params.screenScale[1] = (f32)Maxu32(g_WindowState.prev_height, 1u);
    params.screenScale[2] = 1.0f;
    params.screenScale[3] = 0.0f;

    if (g_UIRenderer.count > 0u)
    {
        UpdateGPUBufferCycle(g_RenderState.uiShapeBuffer, g_UIRenderer.shapes, (size_t)g_UIRenderer.count * sizeof(UIShape), 0ull, true);
    }
    if (g_UIRenderer.imageCount > 0u && (!g_RenderState.uiImagePipeline || !g_RenderState.sampler))
    {
        AX_WARN("UI image render skipped, resources not initialized");
        UIClear();
        return;
    }

    SlugFont* font = SlugGetDemoFont();
    bool hasText = g_UIRenderer.textCount > 0u;
    if (hasText && !SlugPrepare2D(cmd, font)) hasText = false;

    SDL_GPURenderPass* pass = NULL;
    bool shapePipelineBound = false;
    bool slugPipelineBound = false;

    for (u32 batchIndex = 0u; batchIndex < g_UIRenderer.batchCount; batchIndex++)
    {
        UIBatch* batch = &g_UIRenderer.batches[batchIndex];
        if (batch->shapeCount > 0u)
        {
            pass = UIBeginPassIfNeeded(cmd, colorTarget, pass);
            UIDrawShapeRange(cmd, pass, &params, batch->firstShape, batch->shapeCount, &shapePipelineBound);
            slugPipelineBound = false;
        }

        for (u32 i = 0u; i < batch->imageCount; i++)
        {
            u32 imageIndex = batch->firstImage + i;
            if (imageIndex >= g_UIRenderer.imageCount) continue;
            pass = UIBeginPassIfNeeded(cmd, colorTarget, pass);
            UIDrawImageCommand(cmd, pass, &g_UIRenderer.images[imageIndex], &params);
            shapePipelineBound = false;
            slugPipelineBound = false;
        }

        if (batch->textBatchCount > 0u)
        {
            if (hasText)
            {
                pass = UIBeginPassIfNeeded(cmd, colorTarget, pass);
                if (!slugPipelineBound)
                {
                    SlugBind2D(cmd, pass, font);
                    slugPipelineBound = true;
                }
                SlugDraw2DBatches(pass, font, batch->firstTextBatch, batch->textBatchCount, true);
                shapePipelineBound = false;
            }
        }
    }

    if (pass) SDL_EndGPURenderPass(pass);

    UIClear();
}

static void UILayoutDestroy(void)
{
    g_UILayout = (UILayoutContext){0};
}


void UIDestroy(void)
{
    UILayoutDestroy();
    UIDestroyTextShape();
    g_UIRenderer = (UIRenderer){0};
}
