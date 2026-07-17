#include "UI_Internal.h"
#include "Include/UIWindow.h"
#include "Include/Platform.h"
#include "Include/Random.h"
#include "Include/FileSystem.h"
#include "Include/Algorithm.h"
#include "Math/Color.h"

typedef enum UIWindowState_
{
    UIWindowState_None               = 0,
    UIWindowState_Resize_LeftEdge    = 1 << 0,
    UIWindowState_Resize_RightEdge   = 1 << 1,
    UIWindowState_Resize_TopEdge     = 1 << 2,
    UIWindowState_Resize_BottomEdge  = 1 << 3,
    UIWindowState_Resize_EdgeMask    = 0x0F,
    UIWindowState_Move_TabBar        = 1 << 8
} UIWindowState;

static UIWindow g_UIWindows[UI_MAX_WINDOWS];
static u32 g_UIWindowCount;
static s32 g_UIWindowCurrent = -1;
static s32 g_UIWindowActive = -1;
static u32 g_UIWindowState;
static u32 g_UIWindowSnapMask;
static s32 g_UIWindowDockTarget = -1;
static u32 g_UIWindowDockMask;
static s32 g_UIWindowTabTarget = -1;   // window whose center zone is hovered while dragging
static u32 g_UIWindowTabDragHash;      // tab pressed last frame, candidate for detach
static u32 g_UIWindowTabSwitchHash;    // tab clicked last frame, becomes the visible member
static float2 g_UIWindowTabDragStart;
static s32 g_UIWindowResizeHorizontalNeighbor = -1;
static s32 g_UIWindowResizeVerticalNeighbor = -1;
static f32 g_UIWindowTopInset;
static float2 g_UIWindowWorkPosOld;
static float2 g_UIWindowWorkSizeOld;
static bool g_UIWindowLayoutChanged;
static bool g_UIWindowContentOpen;
static bool g_UIWindowHadFocus = true;

// placement loaded from a layout file, applied when its window is next built
typedef struct UIWindowPlacement_
{
    u32 hash;
    u32 tabGroup;
    float2 position;
    float2 scale;
    u8 depth;
    bool collapsed;
    bool open;
    bool tabActive;
    bool openApplied;
    bool rectApplied;
} UIWindowPlacement;

static UIWindowPlacement g_UIWindowPlacements[UI_MAX_WINDOWS];
static u32 g_UIWindowPlacementCount;
static bool g_UIWindowCursorRequested;
static bool g_UIWindowCursorOwned;

#define UI_MAX_RIGHT_CLICK_EVENTS 16u

typedef struct UIRightClickEvent_
{
    const char* label;
    UIRightClickEventFn fn;
    void* data;
    u32 windowHash;
} UIRightClickEvent;

// events are re-registered every frame while their window is built
static UIRightClickEvent g_UIRightClickEvents[UI_MAX_RIGHT_CLICK_EVENTS];
static u32    g_UIRightClickEventCount;
static u32    g_UIRightClickMenuHash; // open menu's window, 0 when closed
static float2 g_UIRightClickMenuPos;
static bool   g_UIRightClickMenuDrawn;

static Texture g_UIWindowMinimizeTexture;
static Texture g_UIWindowMaximizeTexture;
static Texture g_UIWindowCloseTexture;
static UIImageData g_UIWindowMinimizeImage;
static UIImageData g_UIWindowMaximizeImage;
static UIImageData g_UIWindowCloseImage;
static u32 g_UIWindowMinimizePixels[64 * 64];
static u32 g_UIWindowMaximizePixels[64 * 64];
static u32 g_UIWindowClosePixels[64 * 64];
static bool g_UIWindowButtonIconsInitialized;

#include "WindowButtonIcons.inl"

enum
{
    UIWindowSnap_Left   = 1u << 0,
    UIWindowSnap_Right  = 1u << 1,
    UIWindowSnap_Top    = 1u << 2,
    UIWindowSnap_Bottom = 1u << 3
};

static void UIWindowEnsureButtonIcons(void)
{
    if (g_UIWindowButtonIconsInitialized) return;
    g_UIWindowButtonIconsInitialized = true;

    g_UIWindowMinimizeTexture = Create64pxBitTexture(EditorMinimizeIconRows, g_UIWindowMinimizePixels, UCOLOR_GRAY, "UIWindowMinimizeIcon");
    g_UIWindowMaximizeTexture = Create64pxBitTexture(EditorMaximizeIconRows, g_UIWindowMaximizePixels, UCOLOR_GRAY, "UIWindowMaximizeIcon");
    g_UIWindowCloseTexture    = Create64pxBitTexture(EditorCloseIconRows   , g_UIWindowClosePixels   , UCOLOR_GRAY, "UIWindowCloseIcon");
    g_UIWindowMinimizeImage = UIImageFromTexture(&g_UIWindowMinimizeTexture);
    g_UIWindowMaximizeImage = UIImageFromTexture(&g_UIWindowMaximizeTexture);
    g_UIWindowCloseImage    = UIImageFromTexture(&g_UIWindowCloseTexture);
}

static void UIWindowTitleButtonIcon(UIImageData* image)
{
    if (!image || !image->texture) return;
    CLAY(CLAY_ID_LOCAL("Icon"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(16.0f), CLAY_SIZING_FIXED(16.0f) } },
        .image = { .imageData = image }
    }) {}
}

// screen area windows may occupy, the top inset keeps them below overlays like the editor tab bar
static float2 UIWindowWorkPos(void)
{
    return (float2){ 0.0f, Minf32(g_UIWindowTopInset, g_UI.screenSize.y - 1.0f) };
}

static float2 UIWindowWorkSize(void)
{
    float2 pos = UIWindowWorkPos();
    return (float2){ Maxf32(g_UI.screenSize.x, 1.0f), Maxf32(g_UI.screenSize.y - pos.y, 1.0f) };
}

static bool UIWindowWorkAreaUsable(float2 workSize)
{
    return workSize.x >= 320.0f && workSize.y >= 180.0f;
}

void UIWindowSetTopInset(f32 inset)
{
    g_UIWindowTopInset = Maxf32(inset, 0.0f);
}

static float2 UIWindowClampScale(UIWindow* window, float2 scale)
{
    scale = F2Max(scale, window->minScale);
    if (!PlatformCtx.WindowFocused || !UIWindowWorkAreaUsable(UIWindowWorkSize())) return scale;
    return F2Min(scale, g_UI.screenSize);
}

static void UIWindowSetRect(UIWindow* window, float2 position, float2 scale)
{
    window->position = position;
    window->scale = UIWindowClampScale(window, scale);
}

static float2 UIWindowScale(const UIWindow* window)
{
    float2 result = window->scale;
    if (window->isCollapsed) result.y = window->topHeight;
    return result;
}

static bool UIWindowIsOpen(const UIWindow* window)
{
    return !window->isOpenPtr || *window->isOpenPtr;
}

// hidden members of a tab group must not draw, dock or hit-test
static bool UIWindowIsVisible(const UIWindow* window)
{
    return UIWindowIsOpen(window) && (window->tabGroup == 0u || window->tabActive);
}

static s32 UIWindowFind(u32 hash)
{
    for (u32 i = 0u; i < g_UIWindowCount; i++)
        if (g_UIWindows[i].hash == hash)
            return (s32)i;
    return (s32)g_UIWindowCount;
}

static bool UIWindowOnTopOf(s32 targetIdx, float2 pos)
{
    UIWindow* target = &g_UIWindows[targetIdx];
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* window = &g_UIWindows[i];
        if ((s32)i == targetIdx || !UIWindowIsVisible(window)) continue;
        if (window->depth > target->depth && RectPointIntersect(window->position, UIWindowScale(window), pos)) return true;
    }
    return false;
}

static void UIWindowBringToFront(s32 windowIndex)
{
    UIWindow* window = &g_UIWindows[windowIndex];
    u8 oldDepth = window->depth;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        if (g_UIWindows[i].depth > oldDepth) g_UIWindows[i].depth--;
    }
    window->depth = (u8)(g_UIWindowCount - 1u);
    g_UIWindowActive = windowIndex;
}

static bool UIHitRect(float2 pos, float2 size, float2 point)
{
    return RectPointIntersect(pos, size, point) != 0u;
}

static f32 UIWindowOverlap1D(f32 a0, f32 a1, f32 b0, f32 b1)
{
    return Maxf32(0.0f, Minf32(a1, b1) - Maxf32(a0, b0));
}

static s32 UIWindowFindSharedEdgeWindow(s32 windowIndex, u32 edgeMask)
{
    UIWindow* window = &g_UIWindows[windowIndex];
    float2 pos = window->position;
    float2 size = window->scale;
    f32 x0 = pos.x;
    f32 y0 = pos.y;
    f32 x1 = pos.x + size.x;
    f32 y1 = pos.y + size.y;
    const f32 edgeEpsilon = 2.0f;

    s32 best = -1;
    f32 bestOverlap = 0.0f;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* other = &g_UIWindows[i];
        if ((s32)i == windowIndex || !UIWindowIsVisible(other) || other->isCollapsed) continue;

        float2 otherScale = other->scale;
        f32 ox0 = other->position.x;
        f32 oy0 = other->position.y;
        f32 ox1 = other->position.x + otherScale.x;
        f32 oy1 = other->position.y + otherScale.y;
        f32 overlap = 0.0f;
        bool shared = false;

        if ((edgeMask & UIWindowState_Resize_RightEdge) != 0u)
        {
            shared = Absf32(x1 - ox0) <= edgeEpsilon;
            overlap = UIWindowOverlap1D(y0, y1, oy0, oy1);
        }
        else if ((edgeMask & UIWindowState_Resize_LeftEdge) != 0u)
        {
            shared = Absf32(x0 - ox1) <= edgeEpsilon;
            overlap = UIWindowOverlap1D(y0, y1, oy0, oy1);
        }
        else if ((edgeMask & UIWindowState_Resize_BottomEdge) != 0u)
        {
            shared = Absf32(y1 - oy0) <= edgeEpsilon;
            overlap = UIWindowOverlap1D(x0, x1, ox0, ox1);
        }
        else if ((edgeMask & UIWindowState_Resize_TopEdge) != 0u)
        {
            shared = Absf32(y0 - oy1) <= edgeEpsilon;
            overlap = UIWindowOverlap1D(x0, x1, ox0, ox1);
        }

        if (shared && overlap > bestOverlap)
        {
            best = (s32)i;
            bestOverlap = overlap;
        }
    }
    return best;
}

static u32 UIWindowDockMaskFromTarget(UIWindow* target, float2 mouse)
{
    float2 pos = target->position;
    float2 size = UIWindowScale(target);
    if (!UIHitRect(pos, size, mouse)) return 0u;

    f32 relX = (mouse.x - pos.x) / Maxf32(size.x, 1.0f);
    f32 relY = (mouse.y - pos.y) / Maxf32(size.y, 1.0f);
    f32 leftDist = relX;
    f32 rightDist = 1.0f - relX;
    f32 topDist = relY;
    f32 bottomDist = 1.0f - relY;
    f32 best = Minf32(Minf32(leftDist, rightDist), Minf32(topDist, bottomDist));

    if (best > 0.30f) return 0u;
    if (best == leftDist) return UIWindowSnap_Left;
    if (best == rightDist) return UIWindowSnap_Right;
    if (best == topDist) return UIWindowSnap_Top;
    return UIWindowSnap_Bottom;
}

static s32 UIWindowFindDockTarget(s32 draggedIndex, float2 mouse, u32* outMask)
{
    s32 bestIndex = -1;
    u8 bestDepth = 0u;
    u32 bestMask = 0u;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* target = &g_UIWindows[i];
        if ((s32)i == draggedIndex || !UIWindowIsVisible(target) || target->isCollapsed) continue;

        u32 mask = UIWindowDockMaskFromTarget(target, mouse);
        if (mask == 0u) continue;
        if (bestIndex < 0 || target->depth >= bestDepth)
        {
            bestIndex = (s32)i;
            bestDepth = target->depth;
            bestMask = mask;
        }
    }

    if (outMask) *outMask = bestMask;
    return bestIndex;
}

static float2 UIWindowSnapPosition(u32 mask)
{
    float2 workPos = UIWindowWorkPos();
    float2 work = UIWindowWorkSize();
    if ((mask & UIWindowSnap_Left) != 0u) return workPos;
    if ((mask & UIWindowSnap_Right) != 0u) return (float2){ workPos.x + work.x * 0.5f, workPos.y };
    if ((mask & UIWindowSnap_Top) != 0u) return workPos;
    if ((mask & UIWindowSnap_Bottom) != 0u) return (float2){ workPos.x, workPos.y + work.y * 0.5f };
    return workPos;
}

static float2 UIWindowSnapScale(u32 mask)
{
    float2 work = UIWindowWorkSize();
    if ((mask & (UIWindowSnap_Left | UIWindowSnap_Right)) != 0u) return (float2){ work.x * 0.5f, work.y };
    if ((mask & UIWindowSnap_Top) != 0u) return work;
    if ((mask & UIWindowSnap_Bottom) != 0u) return (float2){ work.x, work.y * 0.5f };
    return (float2){ 0.0f, 0.0f };
}

// shrinks an edge snap rect so it doesn't cover windows already anchored to a work area
// edge, e.g. snapping bottom between docked left and right windows fills only the middle
static void UIWindowShrinkSnapRectToFreeSpace(u32 mask, s32 draggedIndex, float2* pos, float2* scale)
{
    const f32 eps = 2.0f;
    float2 workPos = UIWindowWorkPos();
    float2 work = UIWindowWorkSize();

    for (u32 pass = 0u; pass < 4u; pass++)
    {
        bool shrunk = false;
        for (u32 i = 0u; i < g_UIWindowCount; i++)
        {
            UIWindow* other = &g_UIWindows[i];
            if ((s32)i == draggedIndex || !UIWindowIsVisible(other) || other->isCollapsed) continue;

            float2 oPos = other->position;
            float2 oScale = UIWindowScale(other);
            f32 overlapX = UIWindowOverlap1D(pos->x, pos->x + scale->x, oPos.x, oPos.x + oScale.x);
            f32 overlapY = UIWindowOverlap1D(pos->y, pos->y + scale->y, oPos.y, oPos.y + oScale.y);
            if (overlapX <= eps || overlapY <= eps) continue;

            bool anchored = oPos.x <= workPos.x + eps || oPos.x + oScale.x >= workPos.x + work.x - eps ||
                            oPos.y <= workPos.y + eps || oPos.y + oScale.y >= workPos.y + work.y - eps;
            if (!anchored) continue;

            if ((mask & (UIWindowSnap_Top | UIWindowSnap_Bottom)) != 0u)
            {
                f32 right = pos->x + scale->x;
                if (oPos.x + oScale.x * 0.5f < pos->x + scale->x * 0.5f) pos->x = Maxf32(pos->x, oPos.x + oScale.x);
                else right = Minf32(right, oPos.x);
                scale->x = right - pos->x;
            }
            else
            {
                f32 bottom = pos->y + scale->y;
                if (oPos.y + oScale.y * 0.5f < pos->y + scale->y * 0.5f) pos->y = Maxf32(pos->y, oPos.y + oScale.y);
                else bottom = Minf32(bottom, oPos.y);
                scale->y = bottom - pos->y;
            }
            shrunk = true;
            if (scale->x <= eps || scale->y <= eps) return;
        }
        if (!shrunk) break;
    }
}

// edge snap rect for windowIndex, shrunk into free space when that still fits the window,
// otherwise the plain half/full work area rect
static void UIWindowSnapRectForWindow(u32 mask, s32 windowIndex, float2* outPos, float2* outScale)
{
    *outPos = UIWindowSnapPosition(mask);
    *outScale = UIWindowSnapScale(mask);
    if ((u32)windowIndex >= g_UIWindowCount) return;

    float2 pos = *outPos;
    float2 scale = *outScale;
    UIWindowShrinkSnapRectToFreeSpace(mask, windowIndex, &pos, &scale);
    UIWindow* window = &g_UIWindows[windowIndex];
    if (scale.x >= window->minScale.x && scale.y >= window->minScale.y)
    {
        *outPos = pos;
        *outScale = scale;
    }
}

static float2 UIWindowDockPosition(UIWindow* target, u32 mask)
{
    float2 pos = target->position;
    float2 size = UIWindowScale(target);
    if ((mask & UIWindowSnap_Right) != 0u) pos.x += size.x * 0.5f;
    if ((mask & UIWindowSnap_Bottom) != 0u) pos.y += size.y * 0.5f;
    return pos;
}

static float2 UIWindowDockScale(UIWindow* target, u32 mask)
{
    float2 size = UIWindowScale(target);
    if ((mask & (UIWindowSnap_Left | UIWindowSnap_Right)) != 0u) size.x *= 0.5f;
    if ((mask & (UIWindowSnap_Top | UIWindowSnap_Bottom)) != 0u) size.y *= 0.5f;
    return size;
}

static void UIWindowSplitTargetRect(UIWindow* target, u32 draggedMask, float2* targetPos, float2* targetScale)
{
    *targetPos = target->position;
    *targetScale = UIWindowScale(target);
    if ((draggedMask & UIWindowSnap_Left) != 0u)
    {
        targetPos->x += targetScale->x * 0.5f;
        targetScale->x *= 0.5f;
    }
    else if ((draggedMask & UIWindowSnap_Right) != 0u)
    {
        targetScale->x *= 0.5f;
    }
    else if ((draggedMask & UIWindowSnap_Top) != 0u)
    {
        targetPos->y += targetScale->y * 0.5f;
        targetScale->y *= 0.5f;
    }
    else if ((draggedMask & UIWindowSnap_Bottom) != 0u)
    {
        targetScale->y *= 0.5f;
    }
}

static bool UIWindowDockCanSplit(UIWindow* dragged, UIWindow* target, u32 mask)
{
    float2 draggedScale = UIWindowDockScale(target, mask);
    float2 targetPos, targetScale;
    UIWindowSplitTargetRect(target, mask, &targetPos, &targetScale);
    (void)targetPos;
    return draggedScale.x >= dragged->minScale.x && draggedScale.y >= dragged->minScale.y && targetScale.x >= target->minScale.x && targetScale.y >= target->minScale.y;
}

static u32 UIWindowSnapMaskFromMouse(float2 mouse)
{
    const f32 edgeZone = 56.0f;
    const f32 centerZone = 220.0f;
    float2 workPos = UIWindowWorkPos();
    float2 work = UIWindowWorkSize();
    f32 centerX = workPos.x + work.x * 0.5f;
    f32 centerY = workPos.y + work.y * 0.5f;
    bool nearCenterX = Absf32(mouse.x - centerX) <= centerZone * 0.5f;
    bool nearCenterY = Absf32(mouse.y - centerY) <= centerZone * 0.5f;
    if (mouse.x <= workPos.x + edgeZone && nearCenterY) return UIWindowSnap_Left;
    if (mouse.x >= workPos.x + work.x - edgeZone && nearCenterY) return UIWindowSnap_Right;
    if (mouse.y <= workPos.y + edgeZone && nearCenterX) return UIWindowSnap_Top;
    if (mouse.y >= workPos.y + work.y - edgeZone && nearCenterX) return UIWindowSnap_Bottom;
    return 0u;
}

static void UIWindowDrawSnapRect(Clay_ElementId id, float2 pos, float2 size, u32 color, s16 zIndex)
{
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_FIXED(size.x), CLAY_SIZING_FIXED(size.y) } },
        .backgroundColor = UIColorToClay(color),
        .cornerRadius = CLAY_CORNER_RADIUS(6.0f),
        .border = { .color = UIGetClayColor(UIColor_SelectedBorder), .width = CLAY_BORDER_ALL(1) },
        .floating = { .offset = { pos.x, pos.y }, .zIndex = zIndex, .attachTo = CLAY_ATTACH_TO_ROOT }
    }) {}
}

static void UIWindowDrawSnapPreview(UIWindow* window)
{
    if (g_UIWindowState != UIWindowState_Move_TabBar || g_UIWindowActive != g_UIWindowCurrent) return;

    float2 workPos = UIWindowWorkPos();
    float2 work = UIWindowWorkSize();
    const f32 edgeZone = 56.0f;
    const f32 centerZone = 220.0f;
    const u32 zoneColor = 0x22E8A400u;
    s16 zIndex = (s16)(window->depth + 96u);
    f32 centerX = workPos.x + work.x * 0.5f;
    f32 centerY = workPos.y + work.y * 0.5f;

    UIWindowDrawSnapRect(CLAY_ID("UIWindowSnapLeft"), (float2){ workPos.x, centerY - centerZone * 0.5f }, (float2){ edgeZone, centerZone }, zoneColor, zIndex);
    UIWindowDrawSnapRect(CLAY_ID("UIWindowSnapRight"), (float2){ workPos.x + work.x - edgeZone, centerY - centerZone * 0.5f }, (float2){ edgeZone, centerZone }, zoneColor, zIndex);
    UIWindowDrawSnapRect(CLAY_ID("UIWindowSnapTop"), (float2){ centerX - centerZone * 0.5f, workPos.y }, (float2){ centerZone, edgeZone }, zoneColor, zIndex);
    UIWindowDrawSnapRect(CLAY_ID("UIWindowSnapBottom"), (float2){ centerX - centerZone * 0.5f, workPos.y + work.y - edgeZone }, (float2){ centerZone, edgeZone }, zoneColor, zIndex);

    // orange center zones: dropping there merges the dragged window into the target as a tab
    g_UIWindowTabTarget = -1;
    const f32 tabZone = 64.0f;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* target = &g_UIWindows[i];
        if ((s32)i == g_UIWindowCurrent || !UIWindowIsVisible(target) || target->isCollapsed || target->topHeight <= 0.0f) continue;

        float2 targetScale = UIWindowScale(target);
        float2 zonePos = { target->position.x + (targetScale.x - tabZone) * 0.5f, target->position.y + (targetScale.y - tabZone) * 0.5f };
        bool zoneHovered = UIHitRect(zonePos, F2Set1(tabZone), g_UI.mouse);
        if (zoneHovered) g_UIWindowTabTarget = (s32)i;
        UIWindowDrawSnapRect(Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowTabZone"), target->hash),
                             zonePos, F2Set1(tabZone), zoneHovered ? 0x88008CFFu : 0x44008CFFu, (s16)(zIndex + 3));
    }
    if (g_UIWindowTabTarget >= 0)
    {
        UIWindow* target = &g_UIWindows[g_UIWindowTabTarget];
        UIWindowDrawSnapRect(CLAY_ID("UIWindowTabTarget"), target->position, UIWindowScale(target), 0x44008CFFu, (s16)(zIndex + 2));
        g_UIWindowDockTarget = -1;
        g_UIWindowDockMask = 0u;
        g_UIWindowSnapMask = 0u;
        return;
    }

    g_UIWindowDockTarget = UIWindowFindDockTarget(g_UIWindowCurrent, g_UI.mouse, &g_UIWindowDockMask);
    if (g_UIWindowDockTarget >= 0)
    {
        UIWindow* target = &g_UIWindows[g_UIWindowDockTarget];
        if (UIWindowDockCanSplit(window, target, g_UIWindowDockMask))
        {
            UIWindowDrawSnapRect(CLAY_ID("UIWindowDockTarget"), UIWindowDockPosition(target, g_UIWindowDockMask), UIWindowDockScale(target, g_UIWindowDockMask), 0x55E8A400u, (s16)(zIndex + 2));
            return;
        }
        g_UIWindowDockTarget = -1;
        g_UIWindowDockMask = 0u;
    }

    g_UIWindowSnapMask = UIWindowSnapMaskFromMouse(g_UI.mouse);
    if (g_UIWindowSnapMask != 0u)
    {
        float2 snapPos, snapScale;
        UIWindowSnapRectForWindow(g_UIWindowSnapMask, g_UIWindowCurrent, &snapPos, &snapScale);
        UIWindowDrawSnapRect(CLAY_ID("UIWindowSnapTarget"), snapPos, snapScale, 0x44E8A400u, (s16)(zIndex + 1));
    }
}

static void UIWindowApplyPendingSnap(void)
{
    if (g_UIWindowActive < 0 || (u32)g_UIWindowActive >= g_UIWindowCount || g_UIWindowSnapMask == 0u) return;

    UIWindow* window = &g_UIWindows[g_UIWindowActive];
    if (!UIWindowIsOpen(window)) return;
    float2 snapPos, snapScale;
    UIWindowSnapRectForWindow(g_UIWindowSnapMask, g_UIWindowActive, &snapPos, &snapScale);
    UIWindowSetRect(window, snapPos, snapScale);
    g_UIWindowSnapMask = 0u;
}

// merges the dragged window (and its whole tab group) into the target window's tab
// group, the dragged window becomes the visible tab like dropping a tab in unity
static void UIWindowApplyPendingTabDock(void)
{
    if (g_UIWindowActive < 0 || (u32)g_UIWindowActive >= g_UIWindowCount || g_UIWindowTabTarget < 0 || (u32)g_UIWindowTabTarget >= g_UIWindowCount) return;
    if (g_UIWindowActive == g_UIWindowTabTarget) return;

    UIWindow* dragged = &g_UIWindows[g_UIWindowActive];
    UIWindow* target = &g_UIWindows[g_UIWindowTabTarget];
    if (!UIWindowIsOpen(dragged) || !UIWindowIsVisible(target)) return;

    u32 group = (target->tabGroup != 0u) ? target->tabGroup : target->hash;
    u32 draggedGroup = dragged->tabGroup;
    target->tabGroup = group;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* window = &g_UIWindows[i];
        bool joins = (s32)i == g_UIWindowActive || (draggedGroup != 0u && window->tabGroup == draggedGroup);
        if (joins) window->tabGroup = group;
        if (window->tabGroup == group)
        {
            window->tabActive = false;
            window->position = target->position;
            window->scale = target->scale;
            window->isCollapsed = target->isCollapsed;
        }
    }
    dragged->tabActive = true;
    g_UIWindowTabTarget = -1;
    g_UIWindowLayoutChanged = true;
}

static void UIWindowApplyPendingDock(void)
{
    if (g_UIWindowActive < 0 || (u32)g_UIWindowActive >= g_UIWindowCount || g_UIWindowDockTarget < 0 || (u32)g_UIWindowDockTarget >= g_UIWindowCount || g_UIWindowDockMask == 0u) return;

    UIWindow* dragged = &g_UIWindows[g_UIWindowActive];
    UIWindow* target = &g_UIWindows[g_UIWindowDockTarget];
    if (!UIWindowIsOpen(dragged) || !UIWindowIsOpen(target)) return;

    float2 draggedPos = UIWindowDockPosition(target, g_UIWindowDockMask);
    float2 draggedScale = UIWindowDockScale(target, g_UIWindowDockMask);
    float2 targetPos, targetScale;
    UIWindowSplitTargetRect(target, g_UIWindowDockMask, &targetPos, &targetScale);

    if (draggedScale.x < dragged->minScale.x || draggedScale.y < dragged->minScale.y || targetScale.x < target->minScale.x || targetScale.y < target->minScale.y) return;

    UIWindowSetRect(dragged, draggedPos, draggedScale);
    UIWindowSetRect(target, targetPos, targetScale);
    g_UIWindowDockTarget = -1;
    g_UIWindowDockMask = 0u;
}

static u32 UIWindowResizeHitMask(UIWindow* window, float2 mouse)
{
    if ((window->flags & UIWindowFlags_NoResize) != 0u || window->isCollapsed) return 0u;

    const f32 edgeDist = 6.0f;
    const f32 cornerDist = 18.0f;
    float2 pos = window->position;
    float2 size = window->scale;
    f32 contentY = pos.y + window->topHeight;
    f32 scrollLaneW = Maxf32(UIGetFloat(UIFloat_ScrollWidth) + 12.0f, edgeDist * 2.0f);
    bool inScrollbarLane = mouse.x >= pos.x + size.x - scrollLaneW && mouse.x <= pos.x + size.x && mouse.y >= contentY && mouse.y <= pos.y + size.y - cornerDist;

    bool left   = Absf32(mouse.x - pos.x) < edgeDist && mouse.y >= pos.y && mouse.y <= pos.y + size.y;
    bool right  = Absf32(mouse.x - (pos.x + size.x)) < edgeDist && mouse.y >= pos.y && mouse.y <= pos.y + size.y;
    bool top    = Absf32(mouse.y - pos.y) < edgeDist && mouse.x >= pos.x && mouse.x <= pos.x + size.x;
    bool bottom = Absf32(mouse.y - (pos.y + size.y)) < edgeDist && mouse.x >= pos.x && mouse.x <= pos.x + size.x;
    if (inScrollbarLane) right = false;

    bool nearLeft   = mouse.x >= pos.x && mouse.x <= pos.x + cornerDist;
    bool nearRight  = mouse.x <= pos.x + size.x && mouse.x >= pos.x + size.x - cornerDist;
    bool nearTop    = mouse.y >= pos.y && mouse.y <= pos.y + cornerDist;
    bool nearBottom = mouse.y <= pos.y + size.y && mouse.y >= pos.y + size.y - cornerDist;

    u32 mask = 0u;
    if      ((left  && nearTop)    || (top && nearLeft))     mask = UIWindowState_Resize_LeftEdge  | UIWindowState_Resize_TopEdge;
    else if ((right && nearTop)    || (top && nearRight))    mask = UIWindowState_Resize_RightEdge | UIWindowState_Resize_TopEdge;
    else if ((left  && nearBottom) || (bottom && nearLeft))  mask = UIWindowState_Resize_LeftEdge  | UIWindowState_Resize_BottomEdge;
    else if ((right && nearBottom) || (bottom && nearRight)) mask = UIWindowState_Resize_RightEdge | UIWindowState_Resize_BottomEdge;
    else
    {
        mask |= left   * UIWindowState_Resize_LeftEdge;
        mask |= right  * UIWindowState_Resize_RightEdge;
        mask |= top    * UIWindowState_Resize_TopEdge;
        mask |= bottom * UIWindowState_Resize_BottomEdge;
    }
    return mask;
}

static wCursor UIWindowCursorFromResizeMask(u32 mask)
{
    bool left   = (mask & UIWindowState_Resize_LeftEdge)   != 0u;
    bool right  = (mask & UIWindowState_Resize_RightEdge)  != 0u;
    bool top    = (mask & UIWindowState_Resize_TopEdge)    != 0u;
    bool bottom = (mask & UIWindowState_Resize_BottomEdge) != 0u;
    if ((left && top) || (right && bottom)) return wCursor_ResizeNWSE;
    if ((right && top) || (left && bottom)) return wCursor_ResizeNESW;
    if (left || right) return wCursor_ResizeEW;
    if (top || bottom) return wCursor_ResizeNS;
    return wCursor_Default;
}

static void UIWindowDrawResizeHighlight(UIWindow* window, u32 mask)
{
    if (mask == 0u) return;
    const f32 thickness  = 4.0f;
    const f32 cornerSize = 18.0f;
    const u32 color = UIGetColor(UIColor_SelectedBorder);
    float2 pos = window->position;
    float2 size = window->scale;
    float2 highlightPos = pos;
    float2 highlightSize = { thickness, thickness };
    bool left   = (mask & UIWindowState_Resize_LeftEdge)   != 0u;
    bool right  = (mask & UIWindowState_Resize_RightEdge)  != 0u;
    bool top    = (mask & UIWindowState_Resize_TopEdge)    != 0u;
    bool bottom = (mask & UIWindowState_Resize_BottomEdge) != 0u;

    if ((left || right) && (top || bottom))
    {
        highlightPos.x = right ? pos.x + size.x - cornerSize : pos.x;
        highlightPos.y = bottom ? pos.y + size.y - cornerSize : pos.y;
        highlightSize = F2Set1(cornerSize);
    }
    else if (left || right)
    {
        highlightPos.x = right ? pos.x + size.x - thickness : pos.x;
        highlightSize = (float2){ thickness, size.y };
    }
    else
    {
        highlightPos.y = bottom ? pos.y + size.y - thickness : pos.y;
        highlightSize = (float2){ size.x, thickness };
    }

    Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowResizeHighlight"), window->hash ^ mask);
    CLAY(id, {
        .layout = { .sizing = { CLAY_SIZING_FIXED(highlightSize.x), CLAY_SIZING_FIXED(highlightSize.y) } },
        .backgroundColor = UIColorToClay(color),
        .cornerRadius = CLAY_CORNER_RADIUS(2.0f),
        .floating = { .offset = { highlightPos.x, highlightPos.y }, .zIndex = (int16_t)(window->depth + 64u), .attachTo = CLAY_ATTACH_TO_ROOT }
    }) {}
}

static void UIWindowHandleMove(UIWindow* window, s32 windowIndex, float2 titlePos, float2 titleSize, float2 mouse, bool mouseDown)
{
    if ((window->flags & UIWindowFlags_NoMove) != 0u) return;
    if (g_UIWindowState != UIWindowState_None && g_UIWindowState != UIWindowState_Move_TabBar) return;
    if (g_UIWindowActive != windowIndex) return;

    bool titleHovered = UIHitRect(titlePos, titleSize, mouse);
    
    if (GetMousePressed(MouseButton_Left) && titleHovered)
        g_UI.canMoveWindow = true;
    if (!mouseDown)
        g_UI.canMoveWindow = false;

    if ((titleHovered || g_UIWindowState == UIWindowState_Move_TabBar) && mouseDown && g_UI.canMoveWindow)
    {
        float2 workPos = UIWindowWorkPos();
        window->position = F2Add(window->position, F2Sub(g_UI.mouse, g_UI.mouseOld));
        window->position.x = Maxf32(window->position.x, workPos.x);
        window->position.y = Maxf32(window->position.y, workPos.y);
        g_UIWindowState = UIWindowState_Move_TabBar;
    }
}

static u32 UIWindowHandleResize(UIWindow* window, s32 windowIndex, float2 mouse, bool mouseDown)
{
    if ((window->flags & UIWindowFlags_NoResize) != 0u || window->isCollapsed) return 0u;
    if (g_UIWindowState == UIWindowState_Move_TabBar) return 0u;
    if (g_UI.scrollBarActive) return 0u;
    if (g_UIWindowActive != windowIndex) return 0u;

    float2 pos = window->position;
    float2 size = window->scale;
    u32 hoverMask = UIWindowResizeHitMask(window, mouse);

    if (hoverMask != 0u || (g_UIWindowState & UIWindowState_Resize_EdgeMask) != 0u)
    {
        wSetCursor(UIWindowCursorFromResizeMask((g_UIWindowState & UIWindowState_Resize_EdgeMask) ? g_UIWindowState : hoverMask));
        g_UIWindowCursorRequested = true;
        g_UIWindowCursorOwned = true;
    }


    if (GetMousePressed(MouseButton_Left) && hoverMask != 0)
        g_UI.canResizeWindow = true;
    
    if (!mouseDown)
    {
        g_UI.canResizeWindow = false;
    }

    if (mouseDown)
    {
        if (g_UI.canResizeWindow && g_UIWindowState == UIWindowState_None)
        {
            g_UIWindowState = hoverMask;
            u32 resizeMask = g_UIWindowState & UIWindowState_Resize_EdgeMask;
            g_UIWindowResizeHorizontalNeighbor = -1;
            g_UIWindowResizeVerticalNeighbor = -1;
            if ((resizeMask & UIWindowState_Resize_RightEdge) != 0u)       g_UIWindowResizeHorizontalNeighbor = UIWindowFindSharedEdgeWindow(windowIndex, UIWindowState_Resize_RightEdge);
            else if ((resizeMask & UIWindowState_Resize_LeftEdge) != 0u)   g_UIWindowResizeHorizontalNeighbor = UIWindowFindSharedEdgeWindow(windowIndex, UIWindowState_Resize_LeftEdge);
            if      ((resizeMask & UIWindowState_Resize_BottomEdge) != 0u) g_UIWindowResizeVerticalNeighbor = UIWindowFindSharedEdgeWindow(windowIndex, UIWindowState_Resize_BottomEdge);
            else if ((resizeMask & UIWindowState_Resize_TopEdge) != 0u)    g_UIWindowResizeVerticalNeighbor = UIWindowFindSharedEdgeWindow(windowIndex, UIWindowState_Resize_TopEdge);
        }

        if ((g_UIWindowState & UIWindowState_Resize_EdgeMask) != 0u) g_UI.windowResizeActive = true;

        if ((g_UIWindowState & UIWindowState_Resize_LeftEdge) != 0u)
        {
            f32 right = pos.x + size.x;
            if (g_UIWindowResizeHorizontalNeighbor >= 0)
            {
                UIWindow* other = &g_UIWindows[g_UIWindowResizeHorizontalNeighbor];
                f32 otherLeft = other->position.x;
                f32 boundary = Clampf32(mouse.x, otherLeft + other->minScale.x, right - window->minScale.x);
                other->scale.x = boundary - otherLeft;
                pos.x = boundary;
                size.x = right - boundary;
            }
            else
            {
                f32 newX = Minf32(mouse.x, right - window->minScale.x);
                size.x += pos.x - newX;
                pos.x = newX;
            }
        }
        if ((g_UIWindowState & UIWindowState_Resize_RightEdge) != 0u)
        {
            if (g_UIWindowResizeHorizontalNeighbor >= 0)
            {
                UIWindow* other = &g_UIWindows[g_UIWindowResizeHorizontalNeighbor];
                f32 otherRight = other->position.x + other->scale.x;
                f32 boundary = Clampf32(mouse.x, pos.x + window->minScale.x, otherRight - other->minScale.x);
                size.x = boundary - pos.x;
                other->position.x = boundary;
                other->scale.x = otherRight - boundary;
            }
            else size.x = Maxf32(mouse.x - pos.x, window->minScale.x);
        }
        if ((g_UIWindowState & UIWindowState_Resize_TopEdge) != 0u)
        {
            f32 bottom = pos.y + size.y;
            f32 workTop = UIWindowWorkPos().y;
            if (g_UIWindowResizeVerticalNeighbor >= 0)
            {
                UIWindow* other = &g_UIWindows[g_UIWindowResizeVerticalNeighbor];
                f32 otherTop = other->position.y;
                f32 boundary = Clampf32(mouse.y, otherTop + other->minScale.y, bottom - window->minScale.y);
                other->scale.y = boundary - otherTop;
                pos.y = boundary;
                size.y = bottom - boundary;
            }
            else
            {
                f32 newY = Maxf32(Minf32(mouse.y, bottom - window->minScale.y), workTop);
                size.y += pos.y - newY;
                pos.y = newY;
            }
        }
        if ((g_UIWindowState & UIWindowState_Resize_BottomEdge) != 0u)
        {
            if (g_UIWindowResizeVerticalNeighbor >= 0)
            {
                UIWindow* other = &g_UIWindows[g_UIWindowResizeVerticalNeighbor];
                f32 otherBottom = other->position.y + other->scale.y;
                f32 boundary = Clampf32(mouse.y, pos.y + window->minScale.y, otherBottom - other->minScale.y);
                size.y = boundary - pos.y;
                other->position.y = boundary;
                other->scale.y = otherBottom - boundary;
            }
            else size.y = Maxf32(mouse.y - pos.y, window->minScale.y);
        }
    }

    UIWindowSetRect(window, pos, size);
    return (g_UIWindowState & UIWindowState_Resize_EdgeMask) ? g_UIWindowState : hoverMask;
}

void UIRightClickAddEvent(const char* label, UIRightClickEventFn fn, void* data)
{
    if (g_UIWindowCurrent < 0)
    {
        AX_WARN("UIRightClickAddEvent must be called between UIBeginWindow and UIEndWindow");
        return;
    }
    if (g_UIRightClickEventCount >= UI_MAX_RIGHT_CLICK_EVENTS)
    {
        AX_WARN("right click event limit reached: %u", UI_MAX_RIGHT_CLICK_EVENTS);
        return;
    }
    UIRightClickEvent* event = &g_UIRightClickEvents[g_UIRightClickEventCount++];
    event->label = label;
    event->fn = fn;
    event->data = data;
    event->windowHash = g_UIWindows[g_UIWindowCurrent].hash;
}

static void UIWindowDrawRightClickMenu(UIWindow* window)
{
    if (g_UIRightClickMenuHash == 0u || window->hash != g_UIRightClickMenuHash) return;
    g_UIRightClickMenuDrawn = true;

    Clay_ElementId menuId = CLAY_ID("UIRightClickMenu");
    Clay_ElementData menuData = Clay_GetElementData(menuId);
    bool insideMenu = menuData.found && UIHitRect((float2){ menuData.boundingBox.x, menuData.boundingBox.y },
                                                  (float2){ menuData.boundingBox.width, menuData.boundingBox.height }, g_UI.mouse);
    if ((GetMousePressed(MouseButton_Left) && !insideMenu) || GetKeyPressed(27))
    {
        g_UIRightClickMenuHash = 0u;
        return;
    }

    // keep the menu fully on screen: estimate its size (measured once laid out, otherwise
    // derived from the row count) and shift the anchor up/left so it never spills off the edge
    u32 rowCount = 0u;
    for (u32 i = 0u; i < g_UIRightClickEventCount; i++)
        if (g_UIRightClickEvents[i].windowHash == window->hash) rowCount++;
    f32 menuW = menuData.found ? menuData.boundingBox.width  : 158.0f;
    f32 menuH = menuData.found ? menuData.boundingBox.height : (8.0f + rowCount * 26.0f);
    float2 menuPos = g_UIRightClickMenuPos;
    if (menuPos.x + menuW > g_UI.screenSize.x) menuPos.x = g_UI.screenSize.x - menuW;
    if (menuPos.y + menuH > g_UI.screenSize.y) menuPos.y = g_UI.screenSize.y - menuH;
    if (menuPos.x < 0.0f) menuPos.x = 0.0f;
    if (menuPos.y < 0.0f) menuPos.y = 0.0f;

    CLAY(menuId, {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(150.0f), CLAY_SIZING_FIT(0) },
            .padding = { 4, 4, 4, 4 },
            .childGap = 2,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = UIColorToClay(UIGetColor(UIColor_Quad) | 0xFF000000u),
        .cornerRadius = CLAY_CORNER_RADIUS(6.0f),
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) },
        .floating = { .offset = { menuPos.x, menuPos.y }, .zIndex = 126, .attachTo = CLAY_ATTACH_TO_ROOT }
    }) {
        for (u32 i = 0u; i < g_UIRightClickEventCount; i++)
        {
            UIRightClickEvent* event = &g_UIRightClickEvents[i];
            if (event->windowHash != window->hash) continue;

            Clay_ElementId rowId = Clay_GetElementIdWithIndex(CLAY_STRING("UIRightClickRow"), i);
            CLAY(rowId, {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24.0f) },
                    .padding = { 8, 8, 0, 0 },
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered) : (Clay_Color){ 0 },
                .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
            }) {
                if (UIClicked())
                {
                    if (event->fn) event->fn(event->data);
                    g_UIRightClickMenuHash = 0u;
                }
                CLAY_TEXT(UIStr(event->label), CLAY_TEXT_CONFIG({
                    .fontSize = 14,
                    .textColor = UIGetClayColor(UIColor_Text),
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                }));
            }
        }
    }
}

// keeps docked and snapped layouts fitting the application window by remapping every
// ui window from the previous work area to the current one whenever it changes
static void UIWindowRemapToWorkArea(void)
{
    float2 workPos = UIWindowWorkPos();
    float2 workSize = UIWindowWorkSize();
    if (PlatformCtx.WindowFocused != g_UIWindowHadFocus)
    {
        g_UIWindowHadFocus = PlatformCtx.WindowFocused;
        if (PlatformCtx.WindowFocused && UIWindowWorkAreaUsable(workSize))
        {
            g_UIWindowWorkPosOld = workPos;
            g_UIWindowWorkSizeOld = workSize;
        }
        return;
    }
    if (!PlatformCtx.WindowFocused || !UIWindowWorkAreaUsable(workSize)) return;
    if (g_UIWindowWorkSizeOld.x <= 0.0f || g_UIWindowWorkSizeOld.y <= 0.0f) return;
    if (!UIWindowWorkAreaUsable(g_UIWindowWorkSizeOld)) return;
    bool changed = workPos.x != g_UIWindowWorkPosOld.x || workPos.y != g_UIWindowWorkPosOld.y ||
                   workSize.x != g_UIWindowWorkSizeOld.x || workSize.y != g_UIWindowWorkSizeOld.y;
    if (!changed) return;

    f32 sx = workSize.x / g_UIWindowWorkSizeOld.x;
    f32 sy = workSize.y / g_UIWindowWorkSizeOld.y;
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* window = &g_UIWindows[i];
        float2 pos = {
            workPos.x + (window->position.x - g_UIWindowWorkPosOld.x) * sx,
            workPos.y + (window->position.y - g_UIWindowWorkPosOld.y) * sy
        };
        UIWindowSetRect(window, pos, (float2){ window->scale.x * sx, window->scale.y * sy });
    }
    g_UIWindowLayoutChanged = true;
}

void UIWindowBeginFrame(void)
{
    UIWindowRemapToWorkArea();
    g_UIWindowCurrent = -1;
    g_UIWindowContentOpen = false;
    g_UIWindowCursorRequested = false;
    g_UIRightClickEventCount = 0u;
    if (!PlatformCtx.WindowFocused)
    {
        g_UIWindowState = UIWindowState_None;
        g_UIWindowSnapMask = 0u;
        g_UIWindowDockTarget = -1;
        g_UIWindowDockMask = 0u;
        g_UIWindowTabTarget = -1;
        g_UIWindowTabDragHash = 0u;
        g_UIWindowResizeHorizontalNeighbor = -1;
        g_UIWindowResizeVerticalNeighbor = -1;
    }
    if (!GetMouseDown(MouseButton_Left))
    {
        if (g_UIWindowState == UIWindowState_Move_TabBar)
        {
            if (g_UIWindowTabTarget >= 0) UIWindowApplyPendingTabDock();
            else if (g_UIWindowDockTarget >= 0) UIWindowApplyPendingDock();
            else UIWindowApplyPendingSnap();
        }
        if (g_UIWindowState != UIWindowState_None) g_UIWindowLayoutChanged = true;
        g_UIWindowState = UIWindowState_None;
        g_UIWindowSnapMask = 0u;
        g_UIWindowDockTarget = -1;
        g_UIWindowDockMask = 0u;
        g_UIWindowTabTarget = -1;
        g_UIWindowResizeHorizontalNeighbor = -1;
        g_UIWindowResizeVerticalNeighbor = -1;
    }

    // a tab was clicked last frame, make it the visible member of its group
    if (g_UIWindowTabSwitchHash != 0u)
    {
        s32 switchIndex = UIWindowFind(g_UIWindowTabSwitchHash);
        g_UIWindowTabSwitchHash = 0u;
        if ((u32)switchIndex < g_UIWindowCount && g_UIWindows[switchIndex].tabGroup != 0u)
        {
            UIWindow* window = &g_UIWindows[switchIndex];
            for (u32 i = 0u; i < g_UIWindowCount; i++)
                if (g_UIWindows[i].tabGroup == window->tabGroup) g_UIWindows[i].tabActive = false;
            window->tabActive = true;
            UIWindowBringToFront(switchIndex);
            g_UIWindowLayoutChanged = true;
        }
    }

    // dragging a pressed tab beyond the threshold tears the window out of its group
    if (g_UIWindowTabDragHash != 0u)
    {
        if (!GetMouseDown(MouseButton_Left)) g_UIWindowTabDragHash = 0u;
        else if (Absf32(g_UI.mouse.x - g_UIWindowTabDragStart.x) > 8.0f || Absf32(g_UI.mouse.y - g_UIWindowTabDragStart.y) > 8.0f)
        {
            s32 dragIndex = UIWindowFind(g_UIWindowTabDragHash);
            g_UIWindowTabDragHash = 0u;
            if ((u32)dragIndex < g_UIWindowCount && g_UIWindows[dragIndex].tabGroup != 0u)
            {
                UIWindow* window = &g_UIWindows[dragIndex];
                if (window->tabActive)
                {
                    for (u32 i = 0u; i < g_UIWindowCount; i++)
                    {
                        UIWindow* member = &g_UIWindows[i];
                        if ((s32)i == dragIndex || member->tabGroup != window->tabGroup || !UIWindowIsOpen(member)) continue;
                        member->tabActive = true;
                        break;
                    }
                }
                float2 workPos = UIWindowWorkPos();
                window->tabGroup = 0u;
                window->tabActive = false;
                window->position.x = Maxf32(g_UI.mouse.x - 60.0f, workPos.x);
                window->position.y = Maxf32(g_UI.mouse.y - window->topHeight * 0.5f, workPos.y);
                UIWindowBringToFront(dragIndex);
                g_UIWindowState = UIWindowState_Move_TabBar;
                g_UIWindowLayoutChanged = true;
            }
        }
    }
}

void UIWindowEndFrame(void)
{
    if (g_UIWindowCursorOwned && !g_UIWindowCursorRequested && !GetMouseDown(MouseButton_Right))
    {
        wSetCursor(wCursor_Default);
        g_UIWindowCursorOwned = false;
    }

    // the menu's window was closed or skipped this frame, drop the menu with it
    if (g_UIRightClickMenuHash != 0u && !g_UIRightClickMenuDrawn) g_UIRightClickMenuHash = 0u;
    g_UIRightClickMenuDrawn = false;

    // captured at frame end so the top inset set while building this frame is included,
    // UIWindowBeginFrame compares against it to remap windows after a resize
    float2 workSize = UIWindowWorkSize();
    if (PlatformCtx.WindowFocused && UIWindowWorkAreaUsable(workSize))
    {
        g_UIWindowWorkPosOld = UIWindowWorkPos();
        g_UIWindowWorkSizeOld = workSize;
    }
}

void UIWindowMarkLayoutChanged(void)
{
    g_UIWindowLayoutChanged = true;
}

bool UIWindowConsumeLayoutChanged(void)
{
    bool changed = g_UIWindowLayoutChanged;
    g_UIWindowLayoutChanged = false;
    return changed;
}

static char* UIWindowLayoutWriteText(char* p, const char* text)
{
    u32 len = (u32)StringLength(text);
    MemCopy(p, text, len);
    return p + len;
}

static char* UIWindowLayoutWriteNumber(char* p, s64 value)
{
    *p++ = ' ';
    return p + IntToString(p, value, 0);
}

bool UIWindowSaveLayout(const char* path)
{
    char text[UI_MAX_WINDOWS * 144u + 64u];
    char* p = text;
    float2 workPos = UIWindowWorkPos();
    float2 workSize = UIWindowWorkSize();

    p = UIWindowLayoutWriteText(p, "work");
    p = UIWindowLayoutWriteNumber(p, (s64)workPos.x);
    p = UIWindowLayoutWriteNumber(p, (s64)workPos.y);
    p = UIWindowLayoutWriteNumber(p, (s64)workSize.x);
    p = UIWindowLayoutWriteNumber(p, (s64)workSize.y);
    *p++ = '\n';

    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* window = &g_UIWindows[i];
        p = UIWindowLayoutWriteText(p, "window");
        p = UIWindowLayoutWriteNumber(p, (s64)window->hash);
        *p++ = '\n';
        p = UIWindowLayoutWriteText(p, "\trect");
        p = UIWindowLayoutWriteNumber(p, (s64)window->position.x);
        p = UIWindowLayoutWriteNumber(p, (s64)window->position.y);
        p = UIWindowLayoutWriteNumber(p, (s64)window->scale.x);
        p = UIWindowLayoutWriteNumber(p, (s64)window->scale.y);
        *p++ = '\n';
        p = UIWindowLayoutWriteText(p, "\tdepth");
        p = UIWindowLayoutWriteNumber(p, (s64)window->depth);
        *p++ = '\n';
        p = UIWindowLayoutWriteText(p, "\tcollapsed");
        p = UIWindowLayoutWriteNumber(p, window->isCollapsed ? 1 : 0);
        *p++ = '\n';
        p = UIWindowLayoutWriteText(p, "\topen");
        p = UIWindowLayoutWriteNumber(p, UIWindowIsOpen(window) ? 1 : 0);
        *p++ = '\n';
        if (window->tabGroup != 0u)
        {
            p = UIWindowLayoutWriteText(p, "\ttabgroup");
            p = UIWindowLayoutWriteNumber(p, (s64)window->tabGroup);
            *p++ = '\n';
            p = UIWindowLayoutWriteText(p, "\ttabactive");
            p = UIWindowLayoutWriteNumber(p, window->tabActive ? 1 : 0);
            *p++ = '\n';
        }
    }

    WriteAllBytes(path, text, (unsigned long)(p - text));
    return true;
}

static bool UIWindowLayoutLineIs(const char* line, const char* prefix)
{
    while (*prefix)
    {
        if (*line++ != *prefix++) return false;
    }
    return true;
}

bool UIWindowLoadLayout(const char* path)
{
    if (!FileExist(path)) { AX_LOG("editor window layout file is not exists"); return false; }
    u64 size = 0u;
    char* text = ReadAllTextAlloc(path, &size, NULL);
    if (!text) { AX_WARN("reading editor layout file failed"); return false; }

    g_UIWindowPlacementCount = 0u;
    float2 savedWorkPos = { 0.0f, 0.0f };
    float2 savedWorkSize = { 0.0f, 0.0f };
    UIWindowPlacement* placement = NULL;
    s64 v[4];

    const char* line = text;
    while (*line)
    {
        const char* end = line;
        while (*end && *end != '\n' && *end != '\r') end++;

        if (UIWindowLayoutLineIs(line, "window "))
        {
            placement = NULL;
            if (g_UIWindowPlacementCount < UI_MAX_WINDOWS)
            {
                ParseNumberI64(line + 7, &v[0]);
                placement = &g_UIWindowPlacements[g_UIWindowPlacementCount++];
                MemsetZero(placement, sizeof(*placement));
                placement->hash = (u32)v[0];
                placement->open = true;
                placement->depth = (u8)UI_MAX_WINDOWS; // unset, keep creation order depth
            }
        }
        else if (UIWindowLayoutLineIs(line, "work "))
        {
            const char* cursor = line + 5;
            for (u32 i = 0u; i < 4u; i++) cursor = ParseNumberI64(cursor, &v[i]);
            savedWorkPos = (float2){ (f32)v[0], (f32)v[1] };
            savedWorkSize = (float2){ (f32)v[2], (f32)v[3] };
        }
        else if (placement && UIWindowLayoutLineIs(line, "\trect "))
        {
            const char* cursor = line + 6;
            for (u32 i = 0u; i < 4u; i++) cursor = ParseNumberI64(cursor, &v[i]);
            placement->position = (float2){ (f32)v[0], (f32)v[1] };
            placement->scale = (float2){ (f32)v[2], (f32)v[3] };
        }
        else if (placement && UIWindowLayoutLineIs(line, "\tdepth "))
        {
            ParseNumberI64(line + 7, &v[0]);
            placement->depth = (u8)Clampf32((f32)v[0], 0.0f, (f32)(UI_MAX_WINDOWS - 1u));
        }
        else if (placement && UIWindowLayoutLineIs(line, "\tcollapsed "))
        {
            ParseNumberI64(line + 11, &v[0]);
            placement->collapsed = v[0] != 0;
        }
        else if (placement && UIWindowLayoutLineIs(line, "\topen "))
        {
            ParseNumberI64(line + 6, &v[0]);
            placement->open = v[0] != 0;
        }
        else if (placement && UIWindowLayoutLineIs(line, "\ttabgroup "))
        {
            ParseNumberI64(line + 10, &v[0]);
            placement->tabGroup = (u32)v[0];
        }
        else if (placement && UIWindowLayoutLineIs(line, "\ttabactive "))
        {
            ParseNumberI64(line + 11, &v[0]);
            placement->tabActive = v[0] != 0;
        }

        line = end;
        while (*line == '\n' || *line == '\r') line++;
    }
    FreeAllText(text);

    // the layout may come from a different application window size, remap the saved
    // rects from the work area they were stored in to the current one
    if (savedWorkSize.x > 0.0f && savedWorkSize.y > 0.0f)
    {
        float2 workPos = UIWindowWorkPos();
        float2 workSize = UIWindowWorkSize();
        f32 sx = workSize.x / savedWorkSize.x;
        f32 sy = workSize.y / savedWorkSize.y;
        for (u32 i = 0u; i < g_UIWindowPlacementCount; i++)
        {
            UIWindowPlacement* it = &g_UIWindowPlacements[i];
            it->position.x = workPos.x + (it->position.x - savedWorkPos.x) * sx;
            it->position.y = workPos.y + (it->position.y - savedWorkPos.y) * sy;
            it->scale.x *= sx;
            it->scale.y *= sy;
        }
    }
    return g_UIWindowPlacementCount > 0u;
}

UIWindow* UIGetWindow(Clay_ElementId id)
{
    s32 index = UIWindowFind(id.id);
    return ((u32)index < g_UIWindowCount) ? &g_UIWindows[index] : NULL;
}

f32 UIWindowRemainingHeight(Clay_ElementId windowId, Clay_ElementId elementId, f32 reserveBelow)
{
    Clay_ElementId contentId = Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowContent"), windowId.id);
    Clay_ElementData content = Clay_GetElementData(contentId);
    Clay_ElementData element = Clay_GetElementData(elementId);
    if (!content.found || !element.found) return 64.0f;

    // content bottom padding is 12, the extra pixel keeps the content sum strictly
    // inside the window so the content never competes for mouse wheel scrolling
    f32 contentBottom = content.boundingBox.y + content.boundingBox.height - 12.0f;
    return Maxf32(contentBottom - element.boundingBox.y - reserveBelow - 1.0f, 32.0f);
}

bool UIWindowPointVisible(Clay_ElementId id, float2 point)
{
    s32 index = UIWindowFind(id.id);
    if ((u32)index >= g_UIWindowCount) return false;
    UIWindow* window = &g_UIWindows[index];
    if (!UIWindowIsVisible(window) || window->isCollapsed) return false;
    if (!UIHitRect(window->position, UIWindowScale(window), point)) return false;
    return !UIWindowOnTopOf(index, point);
}

bool UIAnyWindowHovered(void)
{
    for (u32 i = 0u; i < g_UIWindowCount; i++)
    {
        UIWindow* window = &g_UIWindows[i];
        if (UIWindowIsVisible(window) && UIHitRect(window->position, UIWindowScale(window), g_UI.mouse)) return true;
    }
    return false;
}

bool UIBeginWindow(const char* title, float2 position, float2 scale, bool* open, u32 flags)
{
    u32 hash = StringToHash(title, 5381u);
    return UIBeginWindowId((Clay_ElementId){ .id = hash }, title, position, scale, open, flags);
}

static UIWindowPlacement* UIWindowPlacementFind(u32 hash)
{
    for (u32 i = 0u; i < g_UIWindowPlacementCount; i++)
        if (g_UIWindowPlacements[i].hash == hash)
            return &g_UIWindowPlacements[i];
    return NULL;
}

bool UIBeginWindowId(Clay_ElementId id, const char* title, float2 position, float2 scale, bool* open, u32 flags)
{
    UIWindowPlacement* placement = UIWindowPlacementFind(id.id);
    if (placement && !placement->openApplied)
    {
        placement->openApplied = true;
        if (open) *open = placement->open;
    }

    if (open && !*open) return false;
    if (!title) title = "Window";

    s32 windowIndex = UIWindowFind(id.id);
    if ((u32)windowIndex >= UI_MAX_WINDOWS)
    {
        AX_WARN("UI window limit reached: %u", UI_MAX_WINDOWS);
        return false;
    }

    if ((u32)windowIndex == g_UIWindowCount)
    {
        UIWindow* newWindow = &g_UIWindows[g_UIWindowCount++];
        MemsetZero(newWindow, sizeof(*newWindow));
        float2 workPos = UIWindowWorkPos();
        newWindow->position = (float2){ Maxf32(position.x, workPos.x), Maxf32(position.y, workPos.y) };
        newWindow->scale = scale;
        newWindow->hash = id.id;
        newWindow->depth = (u8)(g_UIWindowCount - 1u);
        newWindow->selectedElement = -1;
        newWindow->started = true;
        if (placement && !placement->rectApplied)
        {
            placement->rectApplied = true;
            newWindow->position = placement->position;
            newWindow->scale = placement->scale;
            newWindow->isCollapsed = placement->collapsed;
            newWindow->tabGroup = placement->tabGroup;
            newWindow->tabActive = placement->tabActive;
            if (placement->depth < (u8)UI_MAX_WINDOWS) newWindow->depth = placement->depth;
        }
        if (g_UIWindowActive < 0) g_UIWindowActive = windowIndex;
    }

    g_UIWindowCurrent = windowIndex;
    UIWindow* window = &g_UIWindows[windowIndex];
    UIWindowEnsureButtonIcons();
    window->isOpenPtr = open;
    window->flags = flags;
    window->currElement = 0;
    window->numElements = 0;
    window->elementPos = window->position;
    window->topHeight = ((flags & UIWindowFlags_NoTabBar) != 0u) ? 0.0f : 34.0f;
    float2 titleSize = SlugCalcTextSizeN(NULL, title, (u32)StringLength(title), 16.0f);
    window->minScale.x = Maxf32(360.0f, titleSize.x + 96.0f);
    window->minScale.y = Maxf32(140.0f, window->topHeight + 72.0f);
    window->scale = UIWindowClampScale(window, window->scale);

    u32 titleLen = Minu32((u32)StringLength(title), (u32)sizeof(window->tabTitle) - 1u);
    MemCopy(window->tabTitle, title, titleLen);
    window->tabTitle[titleLen] = '\0';

    if (window->tabGroup != 0u)
    {
        s32 activeMember = -1;
        for (u32 i = 0u; i < g_UIWindowCount; i++)
        {
            UIWindow* member = &g_UIWindows[i];
            if ((s32)i == windowIndex || member->tabGroup != window->tabGroup || !UIWindowIsOpen(member) || !member->tabActive) continue;
            activeMember = (s32)i;
            break;
        }
        if (window->tabActive)
        {
            // a freshly reopened member wins over the one that replaced it
            if (activeMember >= 0) g_UIWindows[activeMember].tabActive = false;
        }
        else if (activeMember >= 0)
        {
            // hidden tab, mirror the visible member's rect and skip drawing
            UIWindow* active = &g_UIWindows[activeMember];
            window->position = active->position;
            window->scale = active->scale;
            window->isCollapsed = active->isCollapsed;
            window->isFocused = false;
            return false;
        }
        else window->tabActive = true; // group lost its visible member, take over
    }

    float2 visibleScale = UIWindowScale(window);
    bool mousePressed = GetMousePressed(MouseButton_Left) != 0u;
    bool mouseDown = GetMouseDown(MouseButton_Left) != 0u;
    bool hovered = UIHitRect(window->position, visibleScale, g_UI.mouse);
    bool anyOnTop = hovered && UIWindowOnTopOf(windowIndex, g_UI.mouse);
    window->isFocused = !anyOnTop && g_UIWindowActive == windowIndex;

    if (hovered && !anyOnTop && mousePressed) UIWindowBringToFront(windowIndex);

    if ((flags & UIWindowFlags_RightClickable) != 0u && hovered && !anyOnTop && GetMousePressed(MouseButton_Right))
    {
        g_UIRightClickMenuHash = id.id;
        g_UIRightClickMenuPos = g_UI.mouse;
    }

    f32 titlePad = 10.0f;
    f32 buttonSize = 22.0f;
    float2 closePos = { window->position.x + window->scale.x - titlePad - buttonSize, window->position.y + (window->topHeight - buttonSize) * 0.5f };
    float2 collapsePos = { closePos.x - buttonSize - 8.0f, closePos.y };
    bool closeHovered = window->topHeight > 0.0f && UIHitRect(closePos, F2Set1(buttonSize), g_UI.mouse);
    bool collapseHovered = window->topHeight > 0.0f && UIHitRect(collapsePos, F2Set1(buttonSize), g_UI.mouse);

    if (!anyOnTop && mousePressed && closeHovered && open) { *open = false; g_UIWindowLayoutChanged = true; }
    if (!anyOnTop && mousePressed && collapseHovered) { window->isCollapsed = !window->isCollapsed; g_UIWindowLayoutChanged = true; }

    // tabs are hit-tested against last frame's layout, pressing one selects it and
    // arms a possible tear-off, both applied in UIWindowBeginFrame
    bool tabHovered = false;
    if (window->tabGroup != 0u && window->topHeight > 0.0f)
    {
        for (u32 i = 0u; i < g_UIWindowCount; i++)
        {
            UIWindow* member = &g_UIWindows[i];
            if (member->tabGroup != window->tabGroup || !UIWindowIsOpen(member)) continue;
            Clay_ElementData tabData = Clay_GetElementData(Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowTab"), member->hash));
            if (!tabData.found) continue;
            if (!UIHitRect((float2){ tabData.boundingBox.x, tabData.boundingBox.y },
                           (float2){ tabData.boundingBox.width, tabData.boundingBox.height }, g_UI.mouse)) continue;
            tabHovered = true;
            if (!anyOnTop && mousePressed)
            {
                g_UIWindowTabDragHash = member->hash;
                g_UIWindowTabDragStart = g_UI.mouse;
                if (!member->tabActive) g_UIWindowTabSwitchHash = member->hash;
            }
            break;
        }
    }

    u32 resizeHighlightMask = 0u;
    if (!anyOnTop)
    {
        float2 titlePos = window->position;
        float2 titleSize = { Maxf32(window->scale.x - buttonSize * 2.0f - titlePad * 3.0f, 1.0f), window->topHeight };
        resizeHighlightMask = UIWindowHandleResize(window, windowIndex, g_UI.mouse, mouseDown);
        if (!closeHovered && !collapseHovered && !tabHovered) UIWindowHandleMove(window, windowIndex, titlePos, titleSize, g_UI.mouse, mouseDown);
    }

    if (open && !*open) return false;

    visibleScale = UIWindowScale(window);

    u16 borderWidth = (u16)Maxf32(UIGetFloat(UIFloat_BorderWidth), 1.0f);
    Clay_ElementDeclaration outer = {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(window->scale.x), CLAY_SIZING_FIXED(visibleScale.y) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = UIColorToClay(UIGetColor(UIColor_Quad) & 0xF8FFFFFFu),
        .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(borderWidth) },
        .floating = { .offset = { window->position.x, window->position.y }, .zIndex = (s16)(window->depth + 1u), .attachTo = CLAY_ATTACH_TO_ROOT }
    };
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(outer);
    UIWindowDrawResizeHighlight(window, resizeHighlightMask);
    UIWindowDrawSnapPreview(window);

    if (window->topHeight > 0.0f)
    {
        CLAY(CLAY_ID_LOCAL("TitleBar"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(window->topHeight) },
                .padding = { 12, 8, 0, 0 },
                .childGap = 8,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = UIColorToClay(UIGetColor(UIColor_TextBoxBG) & 0xF0FFFFFFu)
        }) {
            u32 openMembers = 0u;
            if (window->tabGroup != 0u)
            {
                for (u32 i = 0u; i < g_UIWindowCount; i++)
                    openMembers += (g_UIWindows[i].tabGroup == window->tabGroup && UIWindowIsOpen(&g_UIWindows[i])) ? 1u : 0u;
            }
            if (openMembers >= 2u)
            {
                for (u32 i = 0u; i < g_UIWindowCount; i++)
                {
                    UIWindow* member = &g_UIWindows[i];
                    if (member->tabGroup != window->tabGroup || !UIWindowIsOpen(member)) continue;
                    CLAY(Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowTab"), member->hash), {
                        .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(24.0f) }, .padding = { 8, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                        .backgroundColor = UIColorToClay(member->tabActive ? UIGetColor(UIColor_Hovered) : (Clay_Hovered() ? UIGetColor(UIColor_Hovered) & 0x80FFFFFFu : UIGetColor(UIColor_CheckboxBG))),
                        .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
                    }) {
                        CLAY_TEXT(UIStr(member->tabTitle), CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = UIGetClayColor(UIColor_Text), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
            }
            else
            {
                Clay_String titleString = { .isStaticallyAllocated = false, .length = (s32)StringLength(title), .chars = title };
                CLAY_TEXT(titleString, CLAY_TEXT_CONFIG({ .fontSize = 16, .textColor = UIGetClayColor(UIColor_Text) }));
            }
            CLAY(CLAY_ID_LOCAL("TitleSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}
            CLAY(CLAY_ID_LOCAL("Collapse"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(buttonSize), CLAY_SIZING_FIXED(buttonSize) }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = UIColorToClay(collapseHovered ? UCOLOR_ORANGE : UIGetColor(UIColor_CheckboxBG)),
                .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
            }) {
                UIWindowTitleButtonIcon(window->isCollapsed ? &g_UIWindowMaximizeImage : &g_UIWindowMinimizeImage);
            }
            if (open)
            {
                CLAY(CLAY_ID_LOCAL("Close"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(buttonSize), CLAY_SIZING_FIXED(buttonSize) }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = UIColorToClay(closeHovered ? 0xFF3030FFu : UIGetColor(UIColor_CheckboxBG)),
                    .cornerRadius = CLAY_CORNER_RADIUS(3.0f)
                }) {
                    UIWindowTitleButtonIcon(&g_UIWindowCloseImage);
                }
            }
        }
    }

    if (window->isCollapsed)
    {
        Clay__CloseElement();
        return false;
    }

    Clay_ElementId contentId = Clay_GetElementIdWithIndex(CLAY_STRING("UIWindowContent"), id.id);
    Clay__OpenElementWithId(contentId);
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .padding = { 14, 14, 12, 12 },
            .childGap = 12,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        // horizontal clip keeps wide children from inflating the content box and its scissor past the window edge
        .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() }
    });
    g_UIWindowContentOpen = true;
    return true;
}

void UIEndWindow(void)
{
    if (!g_UIWindowContentOpen)
    {
        AX_WARN("UIEndWindow called without an open window content");
        return;
    }
    if (g_UIWindowCurrent >= 0) UIWindowDrawRightClickMenu(&g_UIWindows[g_UIWindowCurrent]);
    Clay__CloseElement();
    Clay__CloseElement();
    g_UIWindowContentOpen = false;
    g_UIWindowCurrent = -1;
}
