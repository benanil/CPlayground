
#include <SDL3/SDL_clipboard.h>
#include "Include/Graphics.h"
#include "Include/Platform.h"
#include "Include/Memory.h"
#include "Include/UIRenderer.h"
#include "UI_Internal.h"

typedef struct UITextCommand_
{
    u32 faceIndex;
    u32 glyphIndex;
    u32 codepointIndex;
    kbts_direction direction;
    float2 pos;
    f32 advanceX;
    f32 width;
} UITextCommand;

typedef struct UITextLine_
{
    u32 firstCommand;
    u32 onePastLastCommand;
    u32 minCodepoint;
    u32 maxCodepoint;
    kbts_direction direction;
    f32 x;
    f32 y;
    f32 width;
} UITextLine;

typedef struct UITextLayout_
{
    UITextCommand commands[UI_TEXT_MAX_COMMANDS];
    UITextLine lines[UI_TEXT_MAX_LINES];
    kbts_break_flags breakFlags[UI_TEXT_MAX_CODEPOINTS + 1u];
    u32 commandCount;
    u32 lineCount;
    u32 codepointCount;
    f32 lineHeight;
    f32 ascent;
    f32 width;
    f32 height;
    bool valid;
} UITextLayout;

extern SDL_Window* g_SDLWindow;

static u32 UIStringLength(const char* text, u32 capacity)
{
    if (!text || capacity == 0u) return 0u;
    return (u32)StringLengthSafe(text, capacity);
}

static u32 UIByteFromCodepoint(const char* text, u32 bytes, u32 codepointIndex)
{
    if (!text) return 0u;
    u32 cp = 0u;
    const char* at = text;
    const char* end = text + bytes;
    while (at < end && *at && cp < codepointIndex)
    {
        u32 codepoint;
        int step = CodepointFromUtf8(&codepoint, at, end);
        at += step > 0 ? step : 1;
        cp++;
    }
    return (u32)(at - text);
}

static void UISelectionRange(u32* a, u32* b)
{
    if (*a > *b)
    {
        u32 t = *a;
        *a = *b;
        *b = t;
    }
}

static u32 UITextLineCodepointAtX(const UITextLayout* layout, const UITextLine* line, f32 x)
{
    if (!layout || !line || line->firstCommand == line->onePastLastCommand) return line ? line->minCodepoint : 0u;
    u32 result = line->maxCodepoint;
    u32 prev = UINT32_MAX;
    bool found = false;
    for (u32 i = line->firstCommand; i < line->onePastLastCommand; i++)
    {
        const UITextCommand* cmd = &layout->commands[i];
        result = cmd->codepointIndex;
        if (cmd->codepointIndex != prev && x < cmd->pos.x + cmd->width * 0.5f)
        {
            found = true;
            break;
        }
        prev = cmd->codepointIndex;
    }
    if (!found) return Minu32(line->maxCodepoint, layout->codepointCount);
    if (line->direction == KBTS_DIRECTION_RTL && prev != UINT32_MAX) result = prev;
    return Minu32(result, layout->codepointCount);
}

static u32 UITextLayoutHitTest(const UITextLayout* layout, float2 point)
{
    if (!layout || !layout->valid || layout->lineCount == 0u) return 0u;
    u32 lineIndex = (u32)(Maxf32(point.y - layout->lines[0].y, 0.0f) / Maxf32(layout->lineHeight, 0.001f));
    lineIndex = Minu32(lineIndex, layout->lineCount - 1u);
    return UITextLineCodepointAtX(layout, &layout->lines[lineIndex], point.x);
}

static void UISetCaret(u32 caret, bool selecting)
{
    g_UI.caret = caret;
    if (!selecting) g_UI.selectionAnchor = caret;
}

static bool UIHasSelection(void)
{
    return g_UI.caret != g_UI.selectionAnchor;
}

static void UIDeleteRange(char* buffer, u32* len, u32 start, u32 end)
{
    UISelectionRange(&start, &end);
    if (!buffer || start >= end || end > *len) return;
    SDL_memmove(buffer + start, buffer + end, (size_t)(*len - end + 1u));
    *len -= end - start;
}

static void UIDeleteCodepointRange(char* buffer, u32 capacity, u32 start, u32 end)
{
    u32 len = UIStringLength(buffer, capacity);
    UISelectionRange(&start, &end);
    u32 byteStart = UIByteFromCodepoint(buffer, len, start);
    u32 byteEnd = UIByteFromCodepoint(buffer, len, end);
    UIDeleteRange(buffer, &len, byteStart, byteEnd);
}

static bool UIDeleteSelection(char* buffer, u32* len)
{
    if (!UIHasSelection()) return false;
    u32 start = g_UI.caret;
    u32 end = g_UI.selectionAnchor;
    UISelectionRange(&start, &end);
    u32 byteStart = UIByteFromCodepoint(buffer, *len, start);
    u32 byteEnd = UIByteFromCodepoint(buffer, *len, end);
    UIDeleteRange(buffer, len, byteStart, byteEnd);
    UISetCaret(start, false);
    return true;
}

static bool UIInsertBytesAtCaret(char* buffer, u32 capacity, const char* text, u32 textLen, bool multiline);

static bool UICopySelectionToClipboard(const char* buffer, u32 capacity)
{
    if (!buffer || !UIHasSelection()) return false;
    u32 len = UIStringLength(buffer, capacity);
    u32 start = g_UI.caret;
    u32 end = g_UI.selectionAnchor;
    UISelectionRange(&start, &end);
    u32 byteStart = UIByteFromCodepoint(buffer, len, start);
    u32 byteEnd = UIByteFromCodepoint(buffer, len, end);
    if (byteStart >= byteEnd) return false;

    u32 size = byteEnd - byteStart;
    char* text = (char*)ArenaPushGlobal((u64)size + 1u);
    MemCopy(text, buffer + byteStart, size);
    text[size] = 0;
    bool result = SDL_SetClipboardText(text);
    ArenaPopGlobal((u64)size + 1u);
    return result;
}

static bool UIPasteClipboardAtCaret(char* buffer, u32 capacity, bool multiline)
{
    if (!buffer || capacity == 0u || !SDL_HasClipboardText()) return false;
    char* clipboard = SDL_GetClipboardText();
    if (!clipboard) return false;
    u32 len = (u32)StringLength(clipboard);
    bool result = UIInsertBytesAtCaret(buffer, capacity, clipboard, len, multiline);
    SDL_free(clipboard);
    return result;
}

static bool UIInsertBytesAtCaret(char* buffer, u32 capacity, const char* text, u32 textLen, bool multiline)
{
    if (!buffer || capacity == 0u || !text || textLen == 0u) return false;
    u32 len = UIStringLength(buffer, capacity);
    UIDeleteSelection(buffer, &len);

    bool edited = false;
    u32 caretByte = UIByteFromCodepoint(buffer, len, g_UI.caret);
    for (u32 i = 0u; i < textLen && len + 1u < capacity;)
    {
        char c = text[i];
        u32 codepoint;
        int step = CodepointFromUtf8(&codepoint, text + i, text + textLen);
        u32 copy = (u32)(step > 0 ? step : 1);
        i += copy;
        if (!multiline && (c == '\r' || c == '\n')) continue;
        if (len + copy >= capacity) break;

        SDL_memmove(buffer + caretByte + copy, buffer + caretByte, (size_t)(len - caretByte + 1u));
        MemCopy(buffer + caretByte, text + i - copy, copy);
        caretByte += copy;
        len += copy;
        g_UI.caret++;
        g_UI.selectionAnchor = g_UI.caret;
        edited = true;
    }
    return edited;
}

void UIDestroyTextShape(void)
{
    if (g_UI.textShapeContext) kbts_DestroyShapeContext(g_UI.textShapeContext);
    g_UI.textShapeContext = NULL;
    g_UI.textShapeFontCount = 0u;
    SDL_zeroa(g_UI.textShapeFonts);
}

static bool UIEnsureTextShape(SlugFont* font)
{
    u32 faceCount = Minu32(SlugGetFontFaceCount(font), UI_MAX_SHAPE_FONTS);
    if (faceCount == 0u) return false;
    if (g_UI.textShapeContext && g_UI.textShapeFontCount == faceCount) return true;

    UIDestroyTextShape();
    g_UI.textShapeContext = kbts_CreateShapeContext(NULL, NULL);
    if (!g_UI.textShapeContext)
    {
        AX_WARN("UI text shaping context creation failed");
        return false;
    }

    for (u32 i = 0u; i < faceCount; i++)
    {
        u32 dataSize = 0u;
        void* data = SlugGetFontFaceData(font, i, &dataSize);
        if (!data || dataSize == 0u || dataSize > 0x7FFFFFFFu) continue;
        g_UI.textShapeFonts[i] = kbts_ShapePushFontFromMemory(g_UI.textShapeContext, data, (int)dataSize, SlugGetFontFaceCollectionIndex(font, i));
    }
    g_UI.textShapeFontCount = faceCount;
    return true;
}

static u32 UIShapeFontIndex(kbts_font* font)
{
    for (u32 i = 0u; i < g_UI.textShapeFontCount; i++)
    {
        if (g_UI.textShapeFonts[i] == font) return i;
    }
    return UINT32_MAX;
}

static bool UIAppendTextInput(char* buffer, u32 capacity, bool multiline)
{
    char text[256];
    u32 inputBytes = PlatformConsumeTextInput(text, (u32)sizeof(text));
    if (inputBytes == 0u || !buffer || capacity == 0u) return false;
    return UIInsertBytesAtCaret(buffer, capacity, text, inputBytes, multiline);
}

static bool UIBackspaceText(char* buffer, u32 capacity)
{
    u32 len = UIStringLength(buffer, capacity);
    if (UIDeleteSelection(buffer, &len)) return true;
    if (g_UI.caret == 0u) return false;
    UIDeleteCodepointRange(buffer, capacity, g_UI.caret - 1u, g_UI.caret);
    UISetCaret(g_UI.caret - 1u, false);
    return true;
}

static bool UIDeleteTextForward(char* buffer, u32 capacity)
{
    u32 len = UIStringLength(buffer, capacity);
    u32 codepoints = StringCodepointCount(buffer, len);
    if (UIDeleteSelection(buffer, &len)) return true;
    if (g_UI.caret >= codepoints) return false;
    UIDeleteCodepointRange(buffer, capacity, g_UI.caret, g_UI.caret + 1u);
    UISetCaret(g_UI.caret, false);
    return true;
}

static u32 UIMoveCaretWithBreaks(const UITextLayout* layout, u32 caret, bool forward, kbts_break_flags breakFlags)
{
    if (!layout || !layout->valid) return caret;
    s32 delta = forward ? 1 : -1;
    s32 at = (s32)caret;
    for (;;)
    {
        s32 next = at + delta;
        if (next < 0 || next > (s32)layout->codepointCount) break;
        at = next;
        if (at == (s32)layout->codepointCount || (breakFlags == 0u) || ((layout->breakFlags[at] & breakFlags) == breakFlags)) break;
    }
    return (u32)at;
}

static float2 UITextLayoutCaretPos(const UITextLayout* layout, u32 caret)
{
    if (!layout || !layout->valid || layout->lineCount == 0u) return (float2){0.0f, 0.0f};
    caret = Minu32(caret, layout->codepointCount);
    const UITextLine* line = &layout->lines[layout->lineCount - 1u];
    for (u32 li = 0u; li < layout->lineCount; li++)
    {
        if (caret <= layout->lines[li].maxCodepoint)
        {
            line = &layout->lines[li];
            break;
        }
    }

    f32 x = line->x + line->width;
    for (u32 i = line->firstCommand; i < line->onePastLastCommand; i++)
    {
        const UITextCommand* cmd = &layout->commands[i];
        if (cmd->codepointIndex >= caret)
        {
            x = cmd->pos.x;
            break;
        }
        x = cmd->pos.x + cmd->advanceX;
    }
    return (float2){ x, line->y };
}

static bool UITextEditBehavior(u64 id, char* buffer, u32 capacity, bool multiline, const UITextLayout* layout)
{
    if (g_UI.keyboardFocus != id || !buffer || capacity == 0u) return false;

    bool edited = false;
    u32 len = UIStringLength(buffer, capacity);
    u32 codepoints = StringCodepointCount(buffer, len);
    if (g_UI.caret > codepoints) UISetCaret(codepoints, false);
    if (g_UI.selectionAnchor > codepoints) g_UI.selectionAnchor = codepoints;

    PlatformTextKeyEvent keys[64];
    u32 keyCount = PlatformConsumeTextKeyEvents(keys, (u32)ARRAY_SIZE(keys));
    for (u32 i = 0u; i < keyCount; i++)
    {
        bool shift = (keys[i].mod & SDL_KMOD_SHIFT) != 0;
        bool ctrl = (keys[i].mod & SDL_KMOD_CTRL) != 0;
        switch (keys[i].key)
        {
            case SDLK_A:
                if (ctrl)
                {
                    g_UI.selectionAnchor = 0u;
                    g_UI.caret = codepoints;
                }
                break;
            case SDLK_C:
                if (ctrl) UICopySelectionToClipboard(buffer, capacity);
                break;
            case SDLK_X:
                if (ctrl && UICopySelectionToClipboard(buffer, capacity))
                {
                    edited |= UIDeleteSelection(buffer, &len);
                    codepoints = StringCodepointCount(buffer, len);
                }
                break;
            case SDLK_V:
                if (ctrl)
                {
                    edited |= UIPasteClipboardAtCaret(buffer, capacity, multiline);
                    len = UIStringLength(buffer, capacity);
                    codepoints = StringCodepointCount(buffer, len);
                }
                break;
            case SDLK_LEFT:
                if (!shift && UIHasSelection()) UISetCaret(Minu32(g_UI.caret, g_UI.selectionAnchor), false);
                else UISetCaret(UIMoveCaretWithBreaks(layout, g_UI.caret, false, ctrl ? KBTS_BREAK_FLAG_WORD : KBTS_BREAK_FLAG_GRAPHEME), shift);
                break;
            case SDLK_RIGHT:
                if (!shift && UIHasSelection()) UISetCaret(Maxu32(g_UI.caret, g_UI.selectionAnchor), false);
                else UISetCaret(UIMoveCaretWithBreaks(layout, g_UI.caret, true, ctrl ? KBTS_BREAK_FLAG_WORD : KBTS_BREAK_FLAG_GRAPHEME), shift);
                break;
            case SDLK_UP:
            case SDLK_DOWN:
                if (multiline && layout && layout->valid)
                {
                    float2 caretPos = UITextLayoutCaretPos(layout, g_UI.caret);
                    caretPos.y += keys[i].key == SDLK_UP ? -layout->lineHeight : layout->lineHeight;
                    UISetCaret(UITextLayoutHitTest(layout, caretPos), shift);
                }
                break;
            case SDLK_HOME:
                UISetCaret(0u, shift);
                break;
            case SDLK_END:
                UISetCaret(codepoints, shift);
                break;
            case SDLK_BACKSPACE:
                edited |= UIBackspaceText(buffer, capacity);
                len = UIStringLength(buffer, capacity);
                codepoints = StringCodepointCount(buffer, len);
                break;
            case SDLK_DELETE:
                edited |= UIDeleteTextForward(buffer, capacity);
                len = UIStringLength(buffer, capacity);
                codepoints = StringCodepointCount(buffer, len);
                break;
            case SDLK_RETURN:
                if (multiline)
                {
                    edited |= UIInsertBytesAtCaret(buffer, capacity, "\n", 1u, true);
                    len = UIStringLength(buffer, capacity);
                    codepoints = StringCodepointCount(buffer, len);
                }
                break;
            default:
                break;
        }
    }

    edited |= UIAppendTextInput(buffer, capacity, multiline);
    return edited;
}

static UITextLine* UILayoutBeginLine(UITextLayout* layout, f32 x, f32 y, u32 codepointIndex)
{
    if (layout->lineCount >= UI_TEXT_MAX_LINES) return NULL;
    UITextLine* line = &layout->lines[layout->lineCount++];
    *line = (UITextLine){0};
    line->firstCommand = layout->commandCount;
    line->onePastLastCommand = layout->commandCount;
    line->minCodepoint = codepointIndex;
    line->maxCodepoint = codepointIndex;
    line->direction = KBTS_DIRECTION_DONT_KNOW;
    line->x = x;
    line->y = y;
    return line;
}

static bool UITextBuildLayout(const char* text, u32 bytes, float2 pos, f32 textScale, bool multiline, UITextLayout* layout)
{
    SDL_zero(*layout);
    SlugFont* font = SlugGetDemoFont();
    if (!text || !UIEnsureTextShape(font)) return false;

    f32 sizePx = 32.0f * textScale * g_UI.uiScale;
    layout->ascent = SlugGetFontAscent(font) * sizePx / Maxf32(g_UI.windowRatio.y, 0.01f);
    layout->lineHeight = Maxf32((SlugGetFontAscent(font) - SlugGetFontDescent(font)) * sizePx, sizePx) / Maxf32(g_UI.windowRatio.y, 0.01f);
    layout->codepointCount = StringCodepointCount(text, bytes);
    if (layout->codepointCount > UI_TEXT_MAX_CODEPOINTS) return false;

    UITextLine* line = UILayoutBeginLine(layout, pos.x, pos.y, 0u);
    if (!line) return false;
    f32 penX = 0.0f;
    f32 wrapOffsetX = 0.0f;
    f32 wrapWidth = multiline ? UIGetFloat(UIFloat_TextWrapWidth) : 0.0f;
    u32 cpBase = 0u;
    u32 lineStartByte = 0u;

    while (lineStartByte <= bytes)
    {
        u32 lineBytes = 0u;
        while (lineStartByte + lineBytes < bytes && text[lineStartByte + lineBytes] != '\n') lineBytes++;

        if (lineBytes == 0u)
        {
            line->onePastLastCommand = layout->commandCount;
            line->width = 0.0f;
            line->maxCodepoint = cpBase;
            if (!multiline || lineStartByte >= bytes) break;

            lineStartByte += 1u;
            cpBase += 1u;
            penX = 0.0f;
            line = UILayoutBeginLine(layout, pos.x, pos.y + layout->lineHeight * (f32)layout->lineCount, cpBase);
            if (!line) break;
            continue;
        }

        kbts_ShapeBegin(g_UI.textShapeContext, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
        kbts_ShapeUtf8(g_UI.textShapeContext, text + lineStartByte, (int)lineBytes, KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
        kbts_ShapeEnd(g_UI.textShapeContext);
        if (kbts_ShapeError(g_UI.textShapeContext) != KBTS_SHAPE_ERROR_NONE) return false;

        kbts_run run;
        while (kbts_ShapeRun(g_UI.textShapeContext, &run))
        {
            u32 faceIndex = UIShapeFontIndex(run.Font);
            if (faceIndex == UINT32_MAX) return false;
            f32 emScale = SlugGetFontFaceEmScale(font, faceIndex);
            if (emScale <= 0.0f) return false;
            f32 scaleX = emScale * sizePx / Maxf32(g_UI.windowRatio.x, 0.01f);
            f32 scaleY = emScale * sizePx / Maxf32(g_UI.windowRatio.y, 0.01f);

            if (line->direction == KBTS_DIRECTION_DONT_KNOW) line->direction = run.ParagraphDirection;
            s32 cursorX = 0;
            s32 cursorY = 0;
            kbts_glyph* glyph;
            while (kbts_GlyphIteratorNext(&run.Glyphs, &glyph))
            {
                if (!line) break;
                u32 cp = cpBase + (u32)glyph->UserIdOrCodepointIndex;
                f32 glyphX = pos.x + penX + (f32)(cursorX + glyph->OffsetX) * scaleX - wrapOffsetX;
                f32 advanceX = (f32)glyph->AdvanceX * scaleX;
                f32 glyphWidth = Absf32(advanceX);
                bool canWrap = wrapWidth > 1.0f && line->firstCommand != layout->commandCount;
                if (canWrap && glyphX + glyphWidth > pos.x + wrapWidth)
                {
                    line->onePastLastCommand = layout->commandCount;
                    line->width = wrapWidth;
                    layout->width = Maxf32(layout->width, line->width);
                    wrapOffsetX += glyphX - pos.x;
                    line = UILayoutBeginLine(layout, pos.x, pos.y + layout->lineHeight * (f32)layout->lineCount, cp);
                    if (!line) break;
                    glyphX = pos.x;
                }

                if (cp < UI_TEXT_MAX_CODEPOINTS)
                {
                    kbts_shape_codepoint shapeCodepoint = {0};
                    if (kbts_ShapeGetShapeCodepoint(g_UI.textShapeContext, glyph->UserIdOrCodepointIndex, &shapeCodepoint))
                    {
                        layout->breakFlags[cp] = shapeCodepoint.BreakFlags;
                    }
                }
                if (layout->commandCount < UI_TEXT_MAX_COMMANDS)
                {
                    UITextCommand* cmd = &layout->commands[layout->commandCount++];
                    cmd->faceIndex = faceIndex;
                    cmd->glyphIndex = glyph->Id;
                    cmd->codepointIndex = cp;
                    cmd->direction = run.Direction;
                    cmd->pos.x = glyphX;
                    cmd->pos.y = line->y + layout->ascent - (f32)(cursorY + glyph->OffsetY) * scaleY;
                    cmd->advanceX = advanceX;
                    cmd->width = glyphWidth;
                    line->maxCodepoint = Maxu32(line->maxCodepoint, cp + 1u);
                }
                cursorX += glyph->AdvanceX;
                cursorY += glyph->AdvanceY;
            }
            if (!line) break;
            penX += (f32)cursorX * emScale * sizePx / Maxf32(g_UI.windowRatio.x, 0.01f);
        }
        if (!line) break;

        line->onePastLastCommand = layout->commandCount;
        line->width = Maxf32(penX - wrapOffsetX, 0.0f);
        layout->width = Maxf32(layout->width, line->width);
        if (!multiline || lineStartByte + lineBytes >= bytes) break;

        lineStartByte += lineBytes + 1u;
        cpBase += StringCodepointCount(text + lineStartByte - lineBytes - 1u, lineBytes + 1u);
        penX = 0.0f;
        wrapOffsetX = 0.0f;
        line = UILayoutBeginLine(layout, pos.x, pos.y + layout->lineHeight * (f32)layout->lineCount, cpBase);
        if (!line) break;
    }

    layout->height = layout->lineHeight * Maxf32((f32)layout->lineCount, 1.0f);
    layout->valid = true;
    return true;
}

static void UITextDrawLayout(const UITextLayout* layout)
{
    if (!layout || !layout->valid) return;
    SlugFont* font = SlugGetDemoFont();
    f32 sizePx = 32.0f * UIGetFloat(UIFloat_TextScale) * g_UI.uiScale;
    for (u32 i = 0u; i < layout->commandCount; i++)
    {
        const UITextCommand* cmd = &layout->commands[i];
        SlugAppendGlyph2D(font, cmd->faceIndex, cmd->glyphIndex, cmd->pos, sizePx, UIGetColor(UIColor_Text));
    }
}

static void UITextDrawSelection(const UITextLayout* layout)
{
    if (!layout || !layout->valid || !UIHasSelection()) return;
    u32 selStart = g_UI.caret;
    u32 selEnd = g_UI.selectionAnchor;
    UISelectionRange(&selStart, &selEnd);
    for (u32 li = 0u; li < layout->lineCount; li++)
    {
        const UITextLine* line = &layout->lines[li];
        f32 minX = FLT_MAX;
        f32 maxX = -FLT_MAX;
        for (u32 i = line->firstCommand; i < line->onePastLastCommand; i++)
        {
            const UITextCommand* cmd = &layout->commands[i];
            if (cmd->codepointIndex >= selStart && cmd->codepointIndex < selEnd)
            {
                minX = Minf32(minX, Minf32(cmd->pos.x, cmd->pos.x + cmd->advanceX));
                maxX = Maxf32(maxX, Maxf32(cmd->pos.x, cmd->pos.x + cmd->advanceX));
            }
        }
        if (minX != FLT_MAX && maxX > minX)
        {
            UIPushRoundedRect((float2){ minX, line->y + 1.0f }, (float2){ maxX - minX, layout->lineHeight - 2.0f }, 2.0f, 0x884A90E2u);
        }
    }
}

static void UISetKeyboardFocus(u64 id)
{
    if (g_UI.keyboardFocus == id) return;
    g_UI.keyboardFocus = id;
    if (id) SDL_StartTextInput(g_SDLWindow);
    else SDL_StopTextInput(g_SDLWindow);
}

bool UITextAreaFlags(const char* label, float2 pos, char* buffer, u32 capacity, float2 size, u32 flags, f32* outDragDelta)
{
    f32 labelFontSize = 32.0f * UIGetFloat(UIFloat_TextScale) * g_UI.uiScale;
    if (label) SlugAppendText2D(NULL, label, pos, labelFontSize, UIGetColor(UIColor_Text));
    float2 labelSize = SlugCalcTextSize(SlugGetDemoFont(), label, labelFontSize);
    float2 boxPos = label ? (float2){ pos.x, pos.y + labelSize.y + 8.0f } : pos;
    u64 id = UIAutoID(buffer);
    bool hovered = RectPointIntersect(boxPos, size, g_UI.mouse) != 0u;
    bool focused = g_UI.keyboardFocus == id;

    UIPushRoundedRect(boxPos, size, 6.0f, UIGetColor(UIColor_TextBoxBG));
    UIPushBorder(UIGetFloat(UIFloat_BorderWidth), focused ? UIGetColor(UIColor_SelectedBorder) : UIGetColor(UIColor_Border));
    UIPushFloat(UIFloat_TextScale, 0.5f);

    float2 textPos = { boxPos.x + 10.0f, boxPos.y + 8.0f };
    float2 textSize = SlugCalcTextSize(NULL, buffer, 32.0f * UIGetFloat(UIFloat_TextScale));
    if (flags & UITextAreaFlags_CenterX)
        textPos.x = boxPos.x + (size.x * 0.5f - (textSize.x * 0.5f));

    if (flags & UITextAreaFlags_CenterY)
        textPos.y = boxPos.y + (size.y * 0.5f - (textSize.y * 0.5f));

    bool multiline = (flags & UITextAreaFlags_NoWrap) == 0u;
    if (flags & UITextAreaFlags_Clip)
        UIPushClipRect(boxPos, size);

    UIPushFloat(UIFloat_TextWrapWidth, multiline ? Maxf32(size.x - 20.0f, 1.0f) : 0.0f);
    u32 len = UIStringLength(buffer, capacity);
    static UITextLayout layout;
    UITextBuildLayout(buffer ? buffer : "?", len, textPos, UIGetFloat(UIFloat_TextScale), multiline, &layout);
    bool numericDrag = (flags & UITextAreaFlags_NumericDrag) != 0u;
    bool shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (GetMousePressed(MouseButton_Left))
    {
        if (hovered)
        {
            if (numericDrag && shiftHeld)
            {
                UISetKeyboardFocus(0u);
                g_UI.numericDragId = id;
            }
            else
            {
                UISetKeyboardFocus(id);
                UISetCaret(UITextLayoutHitTest(&layout, g_UI.mouse), shiftHeld);
                g_UI.textDragFocus = id;
            }
        }
        else if (g_UI.keyboardFocus == id) UISetKeyboardFocus(0u);
    }
    if (g_UI.textDragFocus == id && GetMouseDown(MouseButton_Left))
    {
        UISetCaret(UITextLayoutHitTest(&layout, g_UI.mouse), true);
    }
    if (numericDrag && g_UI.numericDragId == id)
    {
        if (GetMouseDown(MouseButton_Left))
        {
            if (outDragDelta) *outDragDelta = g_UI.mouse.x - g_UI.mouseOld.x;
        }
        else g_UI.numericDragId = 0u;
    }
    focused = g_UI.keyboardFocus == id;

    bool edited = UITextEditBehavior(id, buffer, capacity, multiline, &layout);
    len = UIStringLength(buffer, capacity);
    UITextBuildLayout(buffer ? buffer : "?", len, textPos, UIGetFloat(UIFloat_TextScale), multiline, &layout);
    g_UI.wasHovered = hovered;

    if (focused) UITextDrawSelection(&layout);

    SlugFont* font = SlugGetDemoFont();
    u32 firstBatch = font->numBatches;
    UITextDrawLayout(&layout);
    UIRecordTextBatches(firstBatch, font->numBatches - firstBatch);

    if (edited || focused && FModf(TimeSinceStartup(), 0.5f) > 0.20f)
    {
        float2 caretPos = UITextLayoutCaretPos(&layout, g_UI.caret);
        f32 cursorX = Minf32(caretPos.x + 2.0f, boxPos.x + size.x - 4.0f);
        UIPushRect((float2){ cursorX, caretPos.y }, (float2){ 1.5f, layout.lineHeight }, UIGetColor(UIColor_TextBoxCursor));
    }
    UIPopFloat(UIFloat_TextWrapWidth);
    if (flags & UITextAreaFlags_Clip)
        UIPopClipRect();
    UIPopFloat(UIFloat_TextScale);
    return edited;
}

bool UITextArea(const char* label, float2 pos, char* buffer, u32 capacity, float2 size)
{
    return UITextAreaFlags(label, pos, buffer, capacity, size, 0, NULL);
}
