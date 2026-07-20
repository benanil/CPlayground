#include "Include/Slug.h"
#include "Include/UIRenderer.h"
#include "Include/UIWindow.h"
#include "Include/Platform.h"
#include "Include/Camera.h"
#include "Include/FileSystem.h"
#include "Include/Graphics.h"
#include "Include/Random.h"
#include "Include/Rendering.h"
#include "Include/Algorithm.h"
#include "Include/Scene.h"
#include "Math/Color.h"
#include "EditorInternal.h"

extern WindowState g_WindowState;
extern RenderState g_RenderState;
extern SDL_Window* g_SDLWindow;
extern Camera g_Camera;

static bool editorOpen    = true;
static bool sceneOpen     = false;
static bool texturesOpen  = false;
static bool settingsOpen  = false;
static bool assetsOpen    = false;
static bool consoleOpen   = false;
static bool testOpen      = false;
static bool sceneViewOpen = false; // closed = the scene fills the whole window like before
static bool terrainOpen   = false;
static bool importTestOpen = false;
static bool showFps = false;

// scene view content rect from last frame's layout, the renderer sizes the scene to it
static bool   sceneViewVisible;
static float2 sceneViewContentPos;
static float2 sceneViewContentSize;
static UIImageData sceneViewImage;
static Texture editorLogoTexture;
static UIImageData editorLogoImage;
static Texture editorMinimizeTexture;
static Texture editorMaximizeTexture;
static Texture editorRestoreTexture;
static Texture editorCloseTexture;
static UIImageData editorMinimizeImage;
static UIImageData editorMaximizeImage;
static UIImageData editorRestoreImage;
static UIImageData editorCloseImage;
static u32 editorMinimizePixels[64 * 64];
static u32 editorMaximizePixels[64 * 64];
static u32 editorRestorePixels[64 * 64];
static u32 editorClosePixels[64 * 64];
#define SCENE_VIEW_TITLE "Scene View"

#define EDITOR_SETTINGS_PATH "EditorSettings.txt"
#define EDITOR_UI_LAYOUT_PATH "EditorUI.txt"
#define EDITOR_TAB_BAR_HEIGHT 40.0f

#include "../UI/WindowButtonIcons.inl"

static bool editorSettingsLoaded;
static bool editorOpenLastScene;
static bool editorContinueLastUI = true;
static char editorLastScene[512];

static bool EditorLineStartsWith(const char* line, const char* prefix)
{
    while (*prefix)
    {
        if (*line++ != *prefix++) return false;
    }
    return true;
}

static void EditorSettingsSave(void)
{
    char text[1200];
    char* p = text;

    const char* openLine = editorOpenLastScene ? "open_last_scene 1\n" : "open_last_scene 0\n";
    u32 openLen = (u32)StringLength(openLine);
    MemCopy(p, openLine, openLen); p += openLen;

    const char* uiLine = editorContinueLastUI ? "continue_last_ui 1\n" : "continue_last_ui 0\n";
    u32 uiLen = (u32)StringLength(uiLine);
    MemCopy(p, uiLine, uiLen); p += uiLen;

    const char* lastPrefix = "last_scene ";
    u32 prefixLen = (u32)StringLength(lastPrefix);
    MemCopy(p, lastPrefix, prefixLen); p += prefixLen;
    u32 pathLen = Minu32((u32)StringLength(editorLastScene), 511u);
    MemCopy(p, editorLastScene, pathLen); p += pathLen;
    *p++ = '\n';
    
    const char* path = ConcatWithTempPath(EDITOR_SETTINGS_PATH, sizeof(EDITOR_SETTINGS_PATH));
    if (!path) return;
    WriteAllBytes(path, text, (unsigned long)(p - text));
    ArenaPopGlobal(4096);
}

static void EditorSettingsLoad(void)
{
    if (editorSettingsLoaded) return;
    editorSettingsLoaded = true;

    uint64_t size = 0;
    const char* path = ConcatWithTempPath(EDITOR_SETTINGS_PATH, sizeof(EDITOR_SETTINGS_PATH));
    if (!path || !FileExist(path)) return;
    char* text = ReadAllTextAlloc(path, &size, NULL);
    ArenaPopGlobal(4096);
    if (!text) return;

    const char* line = text;
    while (*line)
    {
        const char* end = line;
        while (*end && *end != '\n' && *end != '\r') end++;

        if (EditorLineStartsWith(line, "open_last_scene "))
            editorOpenLastScene = line[16] == '1';
        else if (EditorLineStartsWith(line, "continue_last_ui "))
            editorContinueLastUI = line[17] == '1';
        else if (EditorLineStartsWith(line, "last_scene "))
        {
            const char* value = line + 11;
            u32 len = (u32)Minu64((u64)(end - value), sizeof(editorLastScene) - 1u);
            MemCopy(editorLastScene, value, len);
            editorLastScene[len] = '\0';
        }

        line = end;
        while (*line == '\n' || *line == '\r') line++;
    }
    FreeAllText(text);
}

bool EditorSettingsOpenLastScene(void)
{
    EditorSettingsLoad();
    return editorOpenLastScene;
}

const char* EditorSettingsLastScene(void)
{
    EditorSettingsLoad();
    return editorLastScene;
}

void EditorSettingsSetLastScene(const char* path)
{
    EditorSettingsLoad();
    if (!path) return;
    u32 len = Minu32((u32)StringLength(path), sizeof(editorLastScene) - 1u);
    MemCopy(editorLastScene, path, len);
    editorLastScene[len] = '\0';
    EditorSettingsSave();
}

static void ShowFps(void)
{
    static char fpsText[32] = "fps:0";
    static char msText[128] = "ms:0";
    static f32 accumulatedTime = 0.0f;
    static u32 accumulatedFrames = 0u;
    static bool initialized = false;
    static int fpsLen = 1, msLen = 1;
    f32 dt = GetDeltaTime();

    if (dt > 0.0f)
    {
        accumulatedTime += dt;
        accumulatedFrames++;
    }

    if (!initialized || accumulatedTime >= 0.25f)
    {
        f32 averageDt = (accumulatedFrames > 0u) ? accumulatedTime / (f32)accumulatedFrames : dt;
        int fps = (averageDt > 1.0e-6f) ? (int)((1.0f / averageDt) + 0.5f) : 0;
        f32 ms = averageDt * 1000.0f;
        fpsLen = IntToString(fpsText + 4, (int64_t)fps, 0);
        fpsText[4 + fpsLen] = '\0';
        msLen = FloatToString(msText + 3, ms, 2);
        msText[3 + msLen] = '\0';
        accumulatedTime = 0.0f;
        accumulatedFrames = 0u;
        initialized = true;
    }

    u32 w = g_WindowState.prev_width;
    float2 fpsSize = SlugCalcTextSizeN(NULL, fpsText, fpsLen + 4, 32.0f);
    float2 msSize = SlugCalcTextSizeN(NULL, msText, msLen + 3, 32.0f);
    SlugAppendText2DN(NULL, fpsText, fpsLen + 4, (float2){ w - fpsSize.x - 12.0f, 52.0f }, 32.0f, 0xFFCCCCFF);
    SlugAppendText2DN(NULL, msText, msLen + 3, (float2){ w - msSize.x - 12.0f, 88.0f }, 32.0f, 0xFFCCCCFF);
}

// renders a few UTF-8 strings across scripts to eyeball glyph coverage and multi-byte
// decoding. strings are stored as raw utf-8 byte escapes (\xHH) so the source stays ascii
// and the compiler's charset handling can't mangle them; the comment shows the intent.
static void WindowTestTextUnicode(void)
{
    static const char* lines[] = {
        "\x4C\x61\x74\x69\x6E\x3A\x20\x63\x61\x66\xC3\xA9\x20\x6E\x61\xC3\xAF\x76\x65\x20\x5A\xC3\xBC\x72\x69\x63\x68\x20\xC5\x92\x75\x76\x72\x65\x20\xC3\x9F\x20\xC3\xA0", // Latin: café naïve Zürich Œuvre ß à
        "\x47\x72\x65\x65\x6B\x3A\x20\xCE\xB1\xCE\xB2\xCE\xB3\x20\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC\x20\xCF\x80\x20\xCE\xA9", // Greek: αβγ Ελληνικά π Ω
        "\x43\x79\x72\x69\x6C\x6C\x69\x63\x3A\x20\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\x20\xD0\xBC\xD0\xB8\xD1\x80", // Cyrillic: Привет мир
        "\x53\x79\x6D\x62\x6F\x6C\x73\x3A\x20\xE2\x86\x92\x20\xE2\x89\x88\x20\xE2\x89\xA4\x20\xE2\x89\xA5\x20\xE2\x88\x91\x20\xE2\x88\x9A\x20\xE2\x9C\x93\x20\xE2\x9C\x97\x20\xE2\x98\x85\x20\xE2\x82\xAC\x20\xE2\x80\x94\x20\xC2\xB0", // Symbols: → ≈ ≤ ≥ ∑ √ ✓ ✗ ★ € — °
        "\x4D\x69\x73\x63\x54\x65\x63\x68\x3A\x20\xE2\x8C\x80\x20\xE2\x8C\x82\x20\xE2\x8C\x83\x20\xE2\x8C\x84\x20\xE2\x8C\x98\x20\xE2\x8C\xA5\x20\xE2\x8C\xAB\x20\xE2\x8C\xA6\x20\xE2\x8E\x87\x20\xE2\x8F\x8E\x20\xE2\x8F\x8F\x20\xE2\x8C\xA8\x20\xE2\x8C\x9A\x20\xE2\x8C\x9B\x20\xE2\x8F\xB0\x20\xE2\x8F\xB3\x20\xE2\x8F\xA9\x20\xE2\x8F\xB8\x20\xE2\x8F\xAF\x20\xE2\x8E\x88", // MiscTech: ⌀ ⌂ ⌃ ⌄ ⌘ ⌥ ⌫ ⌦ ⎇ ⏎ ⏏ ⌨ ⌚ ⌛ ⏰ ⏳ ⏩ ⏸ ⏯ ⎈
        "\x41\x72\x72\x6F\x77\x73\x3A\x20\xE2\x86\x90\x20\xE2\x86\x91\x20\xE2\x86\x92\x20\xE2\x86\x93\x20\xE2\x86\x94\x20\xE2\x86\x95\x20\xE2\x86\xA9\x20\xE2\x86\xAA\x20\xE2\x86\xBA\x20\xE2\x86\xBB\x20\xE2\x87\x90\x20\xE2\x87\x92\x20\xE2\x87\x94\x20\xE2\x87\xA6\x20\xE2\x87\xA8\x20\xE2\xAC\x85\x20\xE2\xAC\x86", // Arrows: ← ↑ → ↓ ↔ ↕ ↩ ↪ ↺ ↻ ⇐ ⇒ ⇔ ⇦ ⇨ ⬅ ⬆
        "\x4D\x61\x74\x68\x3A\x20\xE2\x88\x80\x20\xE2\x88\x82\x20\xE2\x88\x83\x20\xE2\x88\x85\x20\xE2\x88\x87\x20\xE2\x88\x88\x20\xE2\x88\x89\x20\xE2\x88\x8F\x20\xE2\x88\x91\x20\xE2\x88\x9A\x20\xE2\x88\x9E\x20\xE2\x88\xA7\x20\xE2\x88\xA8\x20\xE2\x88\xA9\x20\xE2\x88\xAA\x20\xE2\x88\xAB\x20\xE2\x89\x88\x20\xE2\x89\xA0\x20\xE2\x89\xA1\x20\xE2\x89\xA4\x20\xE2\x89\xA5\x20\xE2\x8A\x95\x20\xE2\x8A\x97", // Math: ∀ ∂ ∃ ∅ ∇ ∈ ∉ ∏ ∑ √ ∞ ∧ ∨ ∩ ∪ ∫ ≈ ≠ ≡ ≤ ≥ ⊕ ⊗
        "\x4C\x65\x74\x74\x65\x72\x6C\x69\x6B\x65\x3A\x20\xE2\x84\x82\x20\xE2\x84\x85\x20\xE2\x84\x8F\x20\xE2\x84\x93\x20\xE2\x84\x96\x20\xE2\x84\xA2\x20\xE2\x84\xA6\x20\xE2\x84\xAB\x20\xE2\x84\xAE\x20\xE2\x85\x88\x20\xE2\x85\x93\x20\xE2\x85\x9B", // Letterlike: ℂ ℅ ℏ ℓ № ™ Ω Å ℮ ⅈ ⅓ ⅛
        "\x43\x75\x72\x72\x65\x6E\x63\x79\x3A\x20\xE2\x82\xA0\x20\xE2\x82\xA3\x20\xE2\x82\xA4\x20\xE2\x82\xA6\x20\xE2\x82\xA8\x20\xE2\x82\xA9\x20\xE2\x82\xAA\x20\xE2\x82\xAB\x20\xE2\x82\xAC\x20\xE2\x82\xB9\x20\xE2\x82\xBD\x20\xE2\x82\xBF", // Currency: ₠ ₣ ₤ ₦ ₨ ₩ ₪ ₫ € ₹ ₽ ₿
        "\x42\x6F\x78\x44\x72\x61\x77\x3A\x20\xE2\x94\x80\x20\xE2\x94\x82\x20\xE2\x94\x8C\x20\xE2\x94\x90\x20\xE2\x94\x94\x20\xE2\x94\x98\x20\xE2\x94\x9C\x20\xE2\x94\xA4\x20\xE2\x94\xAC\x20\xE2\x94\xB4\x20\xE2\x94\xBC\x20\xE2\x95\x90\x20\xE2\x95\x91\x20\xE2\x95\x94\x20\xE2\x95\x97\x20\xE2\x95\x9A\x20\xE2\x95\x9D", // BoxDraw: ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼ ═ ║ ╔ ╗ ╚ ╝
        "\x42\x6C\x6F\x63\x6B\x73\x3A\x20\xE2\x96\x80\x20\xE2\x96\x84\x20\xE2\x96\x88\x20\xE2\x96\x8C\x20\xE2\x96\x90\x20\xE2\x96\x91\x20\xE2\x96\x92\x20\xE2\x96\x93\x20\xE2\x96\xA0\x20\xE2\x96\xA1\x20\xE2\x96\xB2\x20\xE2\x96\xBC\x20\xE2\x97\x86\x20\xE2\x97\x8B\x20\xE2\x97\x8F\x20\xE2\x97\x90", // Blocks: ▀ ▄ █ ▌ ▐ ░ ▒ ▓ ■ □ ▲ ▼ ◆ ○ ● ◐
        "\x4D\x69\x73\x63\x53\x79\x6D\x3A\x20\xE2\x98\x80\x20\xE2\x98\x81\x20\xE2\x98\x82\x20\xE2\x98\x83\x20\xE2\x98\x85\x20\xE2\x98\x86\x20\xE2\x98\x8E\x20\xE2\x98\x91\x20\xE2\x99\xA0\x20\xE2\x99\xA3\x20\xE2\x99\xA5\x20\xE2\x99\xA6\x20\xE2\x99\xAA\x20\xE2\x99\xAB\x20\xE2\x9A\x90\x20\xE2\x9A\x98\x20\xE2\x9A\xA0\x20\xE2\x9A\xA1\x20\xE2\x9A\xBD\x20\xE2\x9A\x93", // MiscSym: ☀ ☁ ☂ ☃ ★ ☆ ☎ ☑ ♠ ♣ ♥ ♦ ♪ ♫ ⚐ ⚘ ⚠ ⚡ ⚽ ⚓
        "\x44\x69\x6E\x67\x62\x61\x74\x73\x3A\x20\xE2\x9C\x81\x20\xE2\x9C\x82\x20\xE2\x9C\x88\x20\xE2\x9C\x89\x20\xE2\x9C\x8C\x20\xE2\x9C\x8F\x20\xE2\x9C\x94\x20\xE2\x9C\x96\x20\xE2\x9C\xA8\x20\xE2\x9C\xB4\x20\xE2\x9D\x84\x20\xE2\x9D\xA4\x20\xE2\x9D\xB6\x20\xE2\x9E\x9C\x20\xE2\x9E\xA1", // Dingbats: ✁ ✂ ✈ ✉ ✌ ✏ ✔ ✖ ✨ ✴ ❄ ❤ ❶ ➜ ➡
        "\x53\x75\x70\x53\x75\x62\x3A\x20\xE2\x81\xB0\x20\xC2\xB9\x20\xC2\xB2\x20\xC2\xB3\x20\xE2\x81\xB4\x20\xE2\x81\xB5\x20\xE2\x81\xB9\x20\xE2\x81\xBA\x20\xE2\x81\xBB\x20\xE2\x82\x80\x20\xE2\x82\x81\x20\xE2\x82\x82\x20\xE2\x82\x93\x20\xE2\x82\x8A", // SupSub: ⁰ ¹ ² ³ ⁴ ⁵ ⁹ ⁺ ⁻ ₀ ₁ ₂ ₓ ₊
        "\x43\x4A\x4B\x3A\x20\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\x20\xE4\xB8\xAD\xE6\x96\x87\x20\xED\x95\x9C\xEA\xB8\x80", // CJK: 日本語 中文 한글
    };

    for (u32 i = 0u; i < ARRAY_SIZE(lines); i++)
    {
        CLAY_TEXT(UIStr(lines[i]), CLAY_TEXT_CONFIG({
            .fontSize = 24,
            .textColor = UIGetClayColor(UIColor_Text),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        }));
    }
}

static void WindowTestUI(void)
{
    static bool enabled = true;
    static f32 testValue = 0.35f;
    static f32 exposure = 1.0f;
    static u32 customDataIndex;

    if (!UIBeginWindow("Window Test", (float2){ 560.0f, 80.0f }, (float2){ 380.0f, 360.0f }, &testOpen, 0u)) return;

    CLAY_TEXT(CLAY_STRING("Example floating window"), CLAY_TEXT_CONFIG({
        .fontSize = 22,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    CLAY_TEXT(CLAY_STRING("Drag title bar, resize edges/corners, collapse or close."), CLAY_TEXT_CONFIG({
        .fontSize = 14,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));

    Clay_ElementData windowElement = Clay_GetElementData(CLAY_ID("Window Test"));
    UIDivider(CLAY_ID("WindowTestDivider0"));
    WindowTestTextUnicode();
    UIDivider(CLAY_ID("WindowTestDivider1"));
    UICheckbox(CLAY_ID("WindowTestEnabled"), CLAY_STRING("Enable test option"), &enabled);
    UISliderFloatValue(CLAY_ID("WindowTestSlider"), CLAY_STRING("Window value"), &testValue, 0.0f, 1.0f, 2);
    UISliderFloatValue(CLAY_ID("WindowTestExposure"), CLAY_STRING("Local exposure"), &exposure, 0.1f, 4.0f, 2);

    static const char* dropdownOptions[] = { "Alpha", "Beta", "Gamma", "Delta" };
    static u32 dropdownIndex = 1u;
    UIDropdown(CLAY_ID("WindowTestDropdown"), CLAY_STRING("Dropdown test"), dropdownOptions, 4u, &dropdownIndex);

    static char textBuffer[512] = "Text area in a dockable window.\nTry selecting, typing, and overlapping windows.\0";
    static UITextAreaCustomData textCustomData;
    textCustomData.type = UICustomType_TextArea;
    textCustomData.buffer = textBuffer;
    textCustomData.capacity = sizeof(textBuffer);
    CLAY(CLAY_ID("WindowTestTextArea"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(100.0f) } },
        .custom = { .customData = &textCustomData }
    }) {}

    CLAY(CLAY_ID("WindowTestButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("WindowTestReset"), CLAY_STRING("Reset"), (Clay_Dimensions){ 96.0f, 34.0f }, false))
        {
            enabled = true;
            testValue = 0.35f;
            exposure = 1.0f;
        }
		if (UIButton(CLAY_ID("ToggleClayTest"), CLAY_STRING("ClayTest"), (Clay_Dimensions) { 96.0f, 34.0f }, false))
		{
			Clay_SetDebugModeEnabled(!Clay_IsDebugModeEnabled());
		}
    }

    UIEndWindow();
}

static u32 editorQualityIndex = 2u; // High matches the startup defaults

// performance presets, artistic settings (sun, exposure, gamma) stay untouched.
// the lod modifier scales the projected screen size, higher keeps detailed lods longer
static void EditorApplyQualityPreset(RenderSettings* settings, u32 quality)
{
    settings->enableOcclusion           = true;
    settings->enableLightFrustumCulling = true;
    settings->enableLightOcclusionCulling = true;
    switch (quality)
    {
    case 0: // Low
        settings->enableHBAO            = false;
        settings->enableContactShadows  = false;
        settings->enableMLAA            = false;
        settings->enableBloom           = false;
        settings->msaaSamples           = 1u;
        settings->enableLocalLights     = false;
        settings->lodDistanceModifier   = 0.5f;
        settings->godRaySamples         = 16.0f;
        settings->hbaoDirections        = 4.0f;
        settings->shadowMaxDistance     = SHADOW_MAX_DISTANCE * 0.35f;
        settings->maxVisiblePointShadows = 0.0f;
        settings->maxVisibleSpotShadows  = 0.0f;
        break;
    case 1: // Medium
        settings->enableHBAO            = true;
        settings->enableContactShadows  = true;
        settings->enableMLAA            = true;
        settings->enableBloom           = true;
        settings->msaaSamples           = 2u;
        settings->enableLocalLights     = true;
        settings->lodDistanceModifier   = 0.75f;
        settings->godRaySamples         = 32.0f;
        settings->hbaoDirections        = 4.0f;
        settings->shadowMaxDistance     = SHADOW_MAX_DISTANCE * 0.6f;
        settings->maxVisiblePointShadows = 4.0f;
        settings->maxVisibleSpotShadows  = 4.0f;
        break;
    case 2: // High, matches the startup defaults
        settings->enableHBAO            = true;
        settings->enableContactShadows  = true;
        settings->enableMLAA            = true;
        settings->enableBloom           = true;
        settings->msaaSamples           = 4u;
        settings->enableLocalLights     = true;
        settings->lodDistanceModifier   = 1.0f;
        settings->godRaySamples         = 48.0f;
        settings->hbaoDirections        = 8.0f;
        settings->shadowMaxDistance     = SHADOW_MAX_DISTANCE;
        settings->maxVisiblePointShadows = 8.0f;
        settings->maxVisibleSpotShadows  = 8.0f;
        break;
    case 3: // Ultra
        settings->enableHBAO            = true;
        settings->enableContactShadows  = true;
        settings->enableMLAA            = true;
        settings->enableBloom           = true;
        settings->msaaSamples           = 4u;
        settings->enableLocalLights     = true;
        settings->lodDistanceModifier   = 1.5f;
        settings->godRaySamples         = 64.0f;
        settings->hbaoDirections        = 12.0f;
        settings->shadowMaxDistance     = SHADOW_MAX_DISTANCE;
        settings->maxVisiblePointShadows = (f32)POINT_SHADOW_MAX_LIGHTS;
        settings->maxVisibleSpotShadows  = (f32)SPOT_SHADOW_MAX_LIGHTS;
        break;
    }
}

static void DrawGraphicsWindow()
{
    RenderSettings* settings = &g_RenderSettings;

    Clay_ElementDeclaration EditorPanelBoxDeclaration =
    {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
            .padding = { 14, 14, 12, 12 },
            .childGap = 10,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = { 36, 36, 36, 220 },
        .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(UIGetFloat(UIFloat_BorderWidth)) }
    };

    Clay_ElementId windowID = (Clay_ElementId) { .id = StringToHash("Graphics Editor", 5381u) };
    if (UIBeginWindowId(windowID,"Graphics Editor", (float2){ 18.0f, 18.0f }, (float2){ 500.0f, 760.0f }, &editorOpen, 0u))
    {
        //UISpacing(CLAY_ID("GraphicsEditorDivider0"), 6.0f);
        // reserve the buttons row (38) plus the content child gap (12)
        CLAY(CLAY_ID("GraphicsEditorScroll"), UIScrollPanelDeclaration(UIWindowRemainingHeight(windowID, CLAY_ID("GraphicsEditorScroll"), 50.0f), 12u)) {
            
            CLAY(CLAY_ID("GraphicsEditorFeatureBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("Features");
                static const char* qualityOptions[] = { "Low", "Medium", "High", "Ultra" };
                if (UIDropdown(CLAY_ID("EditorQualityPreset"), CLAY_STRING("Quality"), qualityOptions, 4u, &editorQualityIndex))
                    EditorApplyQualityPreset(settings, editorQualityIndex);
                UICheckbox(CLAY_ID("EditorEnableOcclusion"), CLAY_STRING("Hi-Z occlusion culling"), &settings->enableOcclusion);
                UICheckbox(CLAY_ID("EditorEnableHBAO")     , CLAY_STRING("HBAO ambient occlusion"), &settings->enableHBAO);
                UICheckbox(CLAY_ID("EditorEnableContactShadows"), CLAY_STRING("Contact shadows"), &settings->enableContactShadows);
                UICheckbox(CLAY_ID("EditorEnableMLAA")     , CLAY_STRING("Anti-aliasing (MLAA)"), &settings->enableMLAA);
                UICheckbox(CLAY_ID("EditorEnableBloom")    , CLAY_STRING("Bloom"), &settings->enableBloom);
                static const char* msaaOptions[] = { "Off", "2x", "4x", "8x" };
				u32 msaaIndex = TrailingZeroCount32(settings->msaaSamples);
                if (UIDropdown(CLAY_ID("EditorMSAA"), CLAY_STRING("MSAA"), msaaOptions, 4u, &msaaIndex))
                {
                    settings->msaaSamples = 1 << msaaIndex;
                }
                UICheckbox(CLAY_ID("EditorShowMLAAEdges")  , CLAY_STRING("Show MLAA edge mask"), &settings->showMLAAEdges);
                UICheckbox(CLAY_ID("EditorTerrainWireframe"), CLAY_STRING("Terrain wireframe"), &settings->terrainWireframe);
                UISliderFloatValue(CLAY_ID("EditorTerrainLodFactor"), CLAY_STRING("Terrain LOD factor"), &settings->terrainLodFactor, 0.5f, 2.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorLODDistanceModifier"), CLAY_STRING("LOD distance"), &settings->lodDistanceModifier, 0.05f, 4.0f, 2);
                // scene resolution multiplier, the ui stays at native resolution
                UIEditFloat(CLAY_ID("EditorRenderScale"), CLAY_STRING("Render scale"), &settings->renderScale, 0.25f, 2.0f, 0.25, 3);
            }

            CLAY(CLAY_ID("GraphicsEditorLightBox"), EditorPanelBoxDeclaration) {
                RenderLightDebugInfo lightInfo = RendererGetLightDebugInfo();
                UISectionHeader("Lights");
                UICheckbox(CLAY_ID("EditorEnableLocalLights"), CLAY_STRING("Local lights"), &settings->enableLocalLights);
                UICheckbox(CLAY_ID("EditorLightFrustumCull"), CLAY_STRING("Light frustum culling"), &settings->enableLightFrustumCulling);
                UICheckbox(CLAY_ID("EditorLightOcclusionCull"), CLAY_STRING("Light occlusion culling"), &settings->enableLightOcclusionCulling);
                UICheckbox(CLAY_ID("EditorShowLightRects"), CLAY_STRING("Show light rects"), &settings->showLightRects);
                UITextU32("Total lights", lightInfo.totalLights);
                UITextU32("Submitted lights", lightInfo.submittedLights);
                UITextU32("Max lights", lightInfo.maxLights);
                UIEditInt(CLAY_ID("EditorMaxVisiblePointShadows"), CLAY_STRING("Point shadow maps"), &settings->maxVisiblePointShadows, 0, POINT_SHADOW_MAX_LIGHTS);
                UIEditInt(CLAY_ID("EditorMaxVisibleSpotShadows"), CLAY_STRING("Spot shadow maps"), &settings->maxVisibleSpotShadows, 0, SPOT_SHADOW_MAX_LIGHTS);
            }
            CLAY(CLAY_ID("GraphicsEditorSunBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("Sun");
                UISliderFloatValue(CLAY_ID("EditorSunYaw")  , CLAY_STRING("Yaw")  , &settings->sunYaw  , -180.0f, 180.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorSunPitch"), CLAY_STRING("Pitch"), &settings->sunPitch, -10.0f, 89.0f, 1);
            }
            CLAY(CLAY_ID("GraphicsEditorShadowBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("Shadows");
                UISliderFloatValue(CLAY_ID("EditorShadowMaxDistance")   , CLAY_STRING("Max distance")   , &settings->shadowMaxDistance      , 25.0f, 1000.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorShadowCameraDistance"), CLAY_STRING("Camera distance"), &settings->shadowCameraDistance   , 10.0f,  500.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorShadowCasterMargin")  , CLAY_STRING("Caster margin")  , &settings->shadowCasterDepthMargin, 10.0f,  500.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorShadowCascadeOverlap"), CLAY_STRING("Cascade overlap"), &settings->shadowCascadeOverlap   ,  0.0f,   80.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorShadowSplitNear")     , CLAY_STRING("Split near")     , &settings->shadowSplitNearDistance,  1.0f,   80.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorShadowPSSM")          , CLAY_STRING("PSSM lambda")    , &settings->shadowPSSMLambda       ,  0.0f,    1.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorContactShadowThickness"), CLAY_STRING("Contact thickness"), &settings->SSSThickness, 0.001f, 0.04f, 3);
                UISliderFloatValue(CLAY_ID("EditorContactShadowBilinear"), CLAY_STRING("Contact edge threshold"), &settings->SSSBilinearThreshold, 0.001f, 0.08f, 3);
                UISliderFloatValue(CLAY_ID("EditorContactShadowContrast"), CLAY_STRING("Contact contrast"), &settings->SSSContrast, 1.0f, 8.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorContactShadowIntensity"), CLAY_STRING("Contact intensity"), &settings->SSSIntensity, 0.0f, 1.0f, 2);
            }
            CLAY(CLAY_ID("GraphicsEditorHBAOBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("HBAO");
                UISliderFloatValue(CLAY_ID("EditorHBAORadius")   , CLAY_STRING("Radius")   , &settings->hbaoRadius   , 0.05f, 5.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorHBAOBias")     , CLAY_STRING("Bias")     , &settings->hbaoBias     ,  0.0f, 1.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorHBAOIntensity"), CLAY_STRING("Intensity"), &settings->hbaoIntensity,  0.0f, 6.0f, 2);
                UISliderFloatValue(CLAY_ID("EditorHBAOPower")    , CLAY_STRING("Power")    , &settings->hbaoPower    , 0.25f, 6.0f, 2);
                UIEditInt(CLAY_ID("EditorHBAODirections"), CLAY_STRING("Directions"), &settings->hbaoDirections, 2, 16);
            }
            CLAY(CLAY_ID("GraphicsEditorPostBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("Post / AA");
                UISliderFloatValue(CLAY_ID("EditorMLAAThreshold"), CLAY_STRING("MLAA threshold"), &settings->mlaaThreshold  , 0.01f, 0.25f, 3);
                UISliderFloatValue(CLAY_ID("EditorBloomThreshold"), CLAY_STRING("Bloom threshold"), &settings->bloomThreshold, 0.00f, 8.00f, 2);
                UISliderFloatValue(CLAY_ID("EditorBloomKnee")     , CLAY_STRING("Bloom knee")     , &settings->bloomKnee     , 0.01f, 4.00f, 2);
                UISliderFloatValue(CLAY_ID("EditorBloomClamp")    , CLAY_STRING("Bloom clamp")    , &settings->bloomClamp    , 1.00f, 128.0f, 1);
                UISliderFloatValue(CLAY_ID("EditorBloomIntensity"), CLAY_STRING("Bloom intensity"), &settings->bloomIntensity, 0.00f, 1.00f, 3);
                UISliderFloatValue(CLAY_ID("EditorBloomRadius")   , CLAY_STRING("Bloom radius")   , &settings->bloomRadius   , 0.25f, 4.00f, 2);
                UISliderFloatValue(CLAY_ID("EditorExposure")     , CLAY_STRING("Exposure")      , &settings->exposure       , 0.10f, 4.00f, 2);
                UISliderFloatValue(CLAY_ID("EditorGamma")        , CLAY_STRING("Gamma")         , &settings->gamma          , 1.00f, 3.20f, 2);
                UISliderFloatValue(CLAY_ID("EditorGodRays")      , CLAY_STRING("God rays")      , &settings->godRayIntensity, 0.00f, 8.00f, 2);
                UIEditInt(CLAY_ID("EditorGodRaySamples"), CLAY_STRING("God ray samples"), &settings->godRaySamples, 0, 128);
            }
            CLAY(CLAY_ID("GraphicsEditorFogBox"), EditorPanelBoxDeclaration) {
                UISectionHeader("Height fog");
                UICheckbox(CLAY_ID("EditorEnableHeightFog"), CLAY_STRING("Enable height fog"), &settings->enableHeightFog);
                UISliderFloatValue(CLAY_ID("EditorFogDensity")   , CLAY_STRING("Density")    , &settings->fogDensity   , 0.00f,  1.00f, 2);
                UISliderFloatValue(CLAY_ID("EditorFogHeight")    , CLAY_STRING("Base height") , &settings->fogHeight    , -50.0f, 50.00f, 1);
                UISliderFloatValue(CLAY_ID("EditorFogFalloff")   , CLAY_STRING("Falloff")    , &settings->fogFalloff   , 0.001f, 0.50f, 3);
                UISliderFloatValue(CLAY_ID("EditorFogSunScatter"), CLAY_STRING("Sun scatter"), &settings->fogSunScatter, 0.00f,  1.00f, 2);
                UIColorEdit3(CLAY_ID("EditorFogColor"), CLAY_STRING("Color"), settings->fogColor);
            }
        }
        CLAY(CLAY_ID("GraphicsEditorButtons"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38.0f) },
                .childGap = 10,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            if (UIButton(CLAY_ID("EditorResetGraphics"), CLAY_STRING("Reset"), (Clay_Dimensions){ 100.0f, 34.0f }, false))
            {
                editorQualityIndex = 2u; // defaults are the High preset
                *settings = (RenderSettings){
                    .enableOcclusion = true,
                    .enableHBAO = true,
                    .enableContactShadows = true,
                    .enableMLAA = true,
                    .enableBloom = true,
                    .msaaSamples = 4u,
                    .showMLAAEdges = false,
                    .enableLocalLights = true,
                    .enableLightFrustumCulling = true,
                    .enableLightOcclusionCulling = true,
                    .showLightRects = false,
                    .hbaoRadius = 1.3f,
                    .hbaoBias = 0.5f,
                    .hbaoIntensity = 2.0f,
                    .hbaoPower = 2.0f,
                    .SSSBilinearThreshold = 0.025f,
                    .SSSThickness = 0.005f,
                    .SSSIntensity = 0.75f,
                    .SSSContrast  = 4.0f,
                    .mlaaThreshold = 0.08f,
                    .bloomThreshold = 1.0f,
                    .bloomKnee = 0.5f,
                    .bloomClamp = 64.0f,
                    .bloomIntensity = 0.08f,
                    .bloomRadius = 1.0f,
                    .exposure = 1.0f,
                    .gamma = 2.2f,
                    .godRayIntensity = 2.5f,
                    .godRaySamples = 64.0f,
                    .enableHeightFog = true,
                    .fogColor = { 0.62f, 0.70f, 0.80f },
                    .fogDensity = 0.1f,
                    .fogHeight = 0.0f,
                    .fogFalloff = 0.04f,
                    .fogSunScatter = 0.6f,
                    .hbaoDirections = 8.0f,
                    .lodDistanceModifier = 1.0f,
                    .renderScale = 1.0f,
                    .sunYaw = 116.565f,
                    .sunPitch = 63.435f,
                    .shadowMaxDistance       = SHADOW_MAX_DISTANCE,
                    .shadowCameraDistance    = SHADOW_CAMERA_DISTANCE,
                    .shadowCasterDepthMargin = SHADOW_CASTER_DEPTH_MARGIN,
                    .shadowCascadeOverlap    = SHADOW_CASCADE_OVERLAP,
                    .shadowSplitNearDistance = SHADOW_SPLIT_NEAR_DISTANCE,
                    .shadowPSSMLambda        = SHADOW_PSSM_LAMBDA,
                    .maxVisiblePointShadows  = (f32)POINT_SHADOW_MAX_LIGHTS,
                    .maxVisibleSpotShadows   = (f32)SPOT_SHADOW_MAX_LIGHTS
                };
            }
        }
        UIEndWindow();
    }
}

static void DrawSettingsWindow()
{
    Clay_ElementId windowID = (Clay_ElementId) { .id = StringToHash("SettingsWindow", 5381u) };
    if (UIBeginWindowId(windowID, "Settings", (float2) { 18.0f, 18.0f }, (float2) { 500.0f, 760.0f }, &settingsOpen, 0u))
    {
        EditorSettingsLoad();

        CLAY_TEXT(CLAY_STRING("Editor"), CLAY_TEXT_CONFIG({
            .fontSize = 18,
            .textColor = UIGetClayColor(UIColor_Text)
        }));
        bool openLast = editorOpenLastScene;
        if (UICheckbox(CLAY_ID("SettingsOpenLastScene"), CLAY_STRING("Open last active scene on startup"), &openLast))
        {
            editorOpenLastScene = openLast;
            EditorSettingsSave();
        }
        bool continueUI = editorContinueLastUI;
        if (UICheckbox(CLAY_ID("SettingsContinueLastUI"), CLAY_STRING("Continue from last UI"), &continueUI))
        {
            editorContinueLastUI = continueUI;
            EditorSettingsSave();
            
            if (continueUI)
            {
                const char* layoutPath = ConcatWithTempPath(EDITOR_UI_LAYOUT_PATH, sizeof(EDITOR_UI_LAYOUT_PATH));
                if (layoutPath)
                {
                    UIWindowSaveLayout(layoutPath);
                    ArenaPopGlobal(4096);
                }
            }
        }
        CLAY_TEXT(CLAY_STRING("Last active scene:"), CLAY_TEXT_CONFIG({
            .fontSize = 13,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        CLAY_TEXT(UIStr(editorLastScene[0] ? editorLastScene : "None"), CLAY_TEXT_CONFIG({
            .fontSize = 13,
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        CLAY(CLAY_ID("SettingsButtons"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
                .childGap = 10,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
        if (UIButtonFlags(CLAY_ID("SettingsClearLastScene"), CLAY_STRING("Clear Last Scene"), (Clay_Dimensions){ 140.0f, 30.0f }, false, UIButtonFlag_FitText))
            {
                editorLastScene[0] = '\0';
                EditorSettingsSave();
            }
        }

        UIDivider(CLAY_ID("SettingsPhysicsDivider"));
        CLAY_TEXT(CLAY_STRING("World Physics"), CLAY_TEXT_CONFIG({
            .fontSize = 18,
            .textColor = UIGetClayColor(UIColor_Text)
        }));

        // global (not per-scene); persisted to PhysicsSettings.txt and pushed live onto
        // the active world so edits take effect without reloading.
        PhysicsSettings_Load();
        PhysicsSettings* phys = &g_PhysicsSettings;
        bool physChanged = UIEditFloatN(CLAY_ID("SettingsGravity"), CLAY_STRING("Gravity"), phys->gravity, 3u, -1000.0f, 1000.0f, 3);

        f32 substeps = (f32)phys->substepCount;
        if (UIEditInt(CLAY_ID("SettingsSubsteps"), CLAY_STRING("Substep count"), &substeps, 1, 32))
        {
            phys->substepCount = (u32)substeps;
            physChanged = true;
        }

        physChanged |= UICheckbox(CLAY_ID("SettingsSleep"), CLAY_STRING("Enable sleeping"), &phys->enableSleep);
        physChanged |= UICheckbox(CLAY_ID("SettingsContinuous"), CLAY_STRING("Enable continuous collision"), &phys->enableContinuous);

        if (physChanged)
        {
            Scene_PhysicsApplyWorldSettings();
            PhysicsSettings_Save();
        }

        UIEndWindow();
    }
}

static u32 EditorOpenWindowMask(void)
{
    return ((u32)editorOpen << 0) | ((u32)sceneOpen << 1) | ((u32)texturesOpen << 2) | ((u32)settingsOpen << 3) |
           ((u32)assetsOpen << 4) | ((u32)consoleOpen << 5) | ((u32)testOpen << 6) | ((u32)sceneViewOpen << 7) |
           ((u32)terrainOpen << 8) | ((u32)importTestOpen << 9);
}

bool EditorSceneViewActive(void)
{
    return sceneViewVisible;
}

float2 EditorSceneViewOrigin(void)
{
    return sceneViewVisible ? sceneViewContentPos : (float2){ 0.0f, 0.0f };
}

float2 EditorSceneMouse(void)
{
    f32 mx, my;
    GetMousePos(&mx, &my);
    float2 origin = EditorSceneViewOrigin();
    return (float2){ mx - origin.x, my - origin.y };
}

bool EditorSceneViewPointVisible(float2 point)
{
    if (!sceneViewVisible) return true;
    if (point.x < sceneViewContentPos.x || point.y < sceneViewContentPos.y ||
        point.x > sceneViewContentPos.x + sceneViewContentSize.x ||
        point.y > sceneViewContentPos.y + sceneViewContentSize.y) return false;
    Clay_ElementId windowID = { .id = StringToHash(SCENE_VIEW_TITLE, 5381u) };
    return UIWindowPointVisible(windowID, point);
}

bool EditorSceneInteractAllowed(void)
{
    f32 mx, my;
    GetMousePos(&mx, &my);
    if (sceneViewVisible) return EditorSceneViewPointVisible((float2){ mx, my });
    if (my < 42.0f) return false; // editor tab bar
    return !UIAnyWindowHovered();
}

static void DrawSceneViewWindow(void)
{
    sceneViewVisible = false;
    Clay_ElementId windowID = { .id = StringToHash(SCENE_VIEW_TITLE, 5381u) };
    if (!UIBeginWindowId(windowID, SCENE_VIEW_TITLE, (float2){ 420.0f, 80.0f }, (float2){ 960.0f, 620.0f }, &sceneViewOpen, 0u)) return;

    sceneViewImage = (UIImageData){ .texture = RenderGetFinalTexture() };
    Clay_ElementId imageId = CLAY_ID("SceneViewImage");
    if (sceneViewImage.texture)
    {
        CLAY(imageId, {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
            .image = { .imageData = &sceneViewImage }
        }) {}
    }

    // last frame's content rect, next frame's scene renders at this size
    Clay_ElementData imageData = Clay_GetElementData(imageId);
    if (imageData.found && imageData.boundingBox.width >= 1.0f && imageData.boundingBox.height >= 1.0f)
    {
        sceneViewContentPos = (float2){ imageData.boundingBox.x, imageData.boundingBox.y };
        sceneViewContentSize = (float2){ imageData.boundingBox.width, imageData.boundingBox.height };
        sceneViewVisible = true;
    }
    UIEndWindow();
}

static bool EditorWindowIsMaximized(void)
{
    return (SDL_GetWindowFlags(g_SDLWindow) & SDL_WINDOW_MAXIMIZED) != 0u;
}

static void EditorToggleWindowMaximized(void)
{
    if (EditorWindowIsMaximized()) SDL_RestoreWindow(g_SDLWindow);
    else SDL_MaximizeWindow(g_SDLWindow);
}

static Clay_BoundingBox editorTabBarButtonBoxes[16];
static u32 editorTabBarButtonBoxCount;
static f32 editorTabBarHitTestHeight;

static bool EditorPointInBox(Clay_BoundingBox box, f32 x, f32 y)
{
    return x >= box.x && y >= box.y && x <= box.x + box.width && y <= box.y + box.height;
}

static void EditorCacheTabBarButtonBox(Clay_ElementId id)
{
    if (editorTabBarButtonBoxCount >= ARRAY_SIZE(editorTabBarButtonBoxes)) return;
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found) return;
    editorTabBarButtonBoxes[editorTabBarButtonBoxCount++] = data.boundingBox;
}

static Clay_Color EditorButtonColor(bool hovered, bool selected)
{
    if (hovered) return UIGetClayColor(UIColor_Hovered);
    if (selected) return UIGetClayColor(UIColor_SelectedBorder);
    return UIGetClayColor(UIColor_Quad);
}

static bool EditorTabBarIconButton(Clay_ElementId id, UIImageData* image, Clay_Dimensions size)
{
    bool clicked = false;
    f32 radius = UIFloatStackZero(UIFloat_CornerRadius) ? size.height * 0.5f : UIGetFloat(UIFloat_CornerRadius);
    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(size.width), CLAY_SIZING_FIXED(size.height) },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = EditorButtonColor(Clay_Hovered(), false),
        .cornerRadius = CLAY_CORNER_RADIUS(radius),
        .border = { .color = UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_ALL(UIGetFloat(UIFloat_BorderWidth)) }
    }) {
        if (UIClicked()) clicked = true;
        if (image && image->texture)
        {
            CLAY(CLAY_ID_LOCAL("Icon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(16.0f), CLAY_SIZING_FIXED(16.0f) } },
                .image = { .imageData = image }
            }) {}
        }
    }
    return clicked;
}

// a tab-bar dropdown that toggles a group of window-open flags. `names[i]` labels the row,
// `flags[i]` points at the bool the row toggles. clicking a row flips that window open/closed.
static void EditorWindowMenu(Clay_ElementId id, Clay_String label, const char* const* names, bool* const* flags, u32 count)
{
    UIMenuItem items[16];
    if (count > ARRAY_SIZE(items)) count = ARRAY_SIZE(items);
    for (u32 i = 0u; i < count; i++) { items[i].label = names[i]; items[i].checked = *flags[i]; }

    s32 clicked = UIMenuButton(id, label, (Clay_Dimensions){ UIGetFloat(UIFloat_ButtonSize), 25.0f }, items, count);
    if (clicked >= 0) *flags[clicked] ^= true;
}

static bool EditorPointInCachedTabBarButton(f32 x, f32 y)
{
    for (u32 i = 0; i < editorTabBarButtonBoxCount; i++)
        if (EditorPointInBox(editorTabBarButtonBoxes[i], x, y)) return true;
    return false;
}

static SDL_HitTestResult SDLCALL EditorWindowHitTest(SDL_Window* window, const SDL_Point* area, void* data)
{
    (void)data;
    if (!window || !area) return SDL_HITTEST_NORMAL;

    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0u) return SDL_HITTEST_NORMAL;

    int width, height;
    SDL_GetWindowSize(window, &width, &height);

    const int edge = 8;
    bool left   = area->x >= 0 && area->x < edge;
    bool right  = area->x >= width - edge && area->x < width;
    bool top    = area->y >= 0 && area->y < edge;
    bool bottom = area->y >= height - edge && area->y < height;

    if (top && left)     return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)    return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left)   return SDL_HITTEST_RESIZE_LEFT;
    if (right)  return SDL_HITTEST_RESIZE_RIGHT;
    if (top)    return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;

    if ((f32)area->y < editorTabBarHitTestHeight && !EditorPointInCachedTabBarButton((f32)area->x, (f32)area->y))
        return SDL_HITTEST_DRAGGABLE;

    return SDL_HITTEST_NORMAL;
}

void EditorInit(void)
{
    editorTabBarHitTestHeight = EDITOR_TAB_BAR_HEIGHT;
    if (!SDL_SetWindowHitTest(g_SDLWindow, EditorWindowHitTest, NULL))
        AX_WARN("failed to set editor window hit test: %s", SDL_GetError());

    editorLogoTexture = rImportTexture("Assets/Icons/CLogo.png", TexFlags_MipMap, "EditorLogo");
    editorLogoImage   = UIImageFromTexture(&editorLogoTexture);
	const u32 color = 0xFF115694;
    editorMinimizeTexture = Create64pxBitTexture(EditorMinimizeIconRows, editorMinimizePixels, color, "EditorMinimizeIcon");
    editorMaximizeTexture = Create64pxBitTexture(EditorMaximizeIconRows, editorMaximizePixels, color, "EditorMaximizeIcon");
    editorRestoreTexture  = Create64pxBitTexture(EditorRestoreIconRows , editorRestorePixels , color, "EditorRestoreIcon");
    editorCloseTexture    = Create64pxBitTexture(EditorCloseIconRows   , editorClosePixels   , color, "EditorCloseIcon");
    editorMinimizeImage = UIImageFromTexture(&editorMinimizeTexture);
    editorMaximizeImage = UIImageFromTexture(&editorMaximizeTexture);
    editorRestoreImage  = UIImageFromTexture(&editorRestoreTexture);
    editorCloseImage    = UIImageFromTexture(&editorCloseTexture);
}

static void GraphicsEditorUI(void)
{
    const f32 tabBarHeight = EDITOR_TAB_BAR_HEIGHT;
    Clay_BeginLayout();

    int screenWidth, screenHeight;
    SDL_GetWindowSize(g_SDLWindow, &screenWidth, &screenHeight);
    u16 borderWidth = (u16)UIGetFloat(UIFloat_BorderWidth);

    UIWindowSetTopInset(tabBarHeight);

    static bool uiLayoutLoadAttempted;
    if (!uiLayoutLoadAttempted)
    {
        uiLayoutLoadAttempted = true;
        EditorSettingsLoad();
        if (editorContinueLastUI)
        {
            const char* layoutPath = ConcatWithTempPath(EDITOR_UI_LAYOUT_PATH, sizeof(EDITOR_UI_LAYOUT_PATH));
            if (layoutPath)
            {
                UIWindowLoadLayout(layoutPath);
                ArenaPopGlobal(4096);
            }
        }
    }

    u32 openMaskBefore = EditorOpenWindowMask();
    UIPushFloat(UIFloat_BorderWidth, 0.0f);
    CLAY(CLAY_ID("TabBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(screenWidth), CLAY_SIZING_FIXED(tabBarHeight) },
            .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = UIPanelColor(),
    }) {

        CLAY(CLAY_ID("TabBarLeft"), {
            .layout = { 
                .sizing = { CLAY_SIZING_FIXED(screenWidth >> 1), CLAY_SIZING_GROW(0) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 15,
                .padding  = { 20, 0 },
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            UIPushFloatAdd(UIFloat_TextScale, -0.15f);
            UIPushFloat(UIFloat_CornerRadius, 1.0f);
            if (editorLogoImage.texture)
            {
                CLAY(CLAY_ID("TabBarLogo"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(26.0f), CLAY_SIZING_FIXED(26.0f) } },
                    .image = { .imageData = &editorLogoImage }
                }) {
                    if (UIClicked()) AX_LOG("C Engine logo clicked. The C is for chaos.");
                }
            }
            // tab-bar dropdowns, grouped by purpose. add another button by copying a block
            // with a fresh CLAY_ID/label and its own names/flags, then cache its box below.
            EditorWindowMenu(CLAY_ID("Scene"), CLAY_STRING("Scene"),
                (const char*[]){ "Scene", "Textures", "Terrain" },
                (bool*[]){ &sceneOpen, &texturesOpen, &terrainOpen }, 3u);

            EditorWindowMenu(CLAY_ID("Windows"), CLAY_STRING("Windows"),
                (const char*[]){ "Graphics", "Settings", "Assets", "Console", "Test", "View" },
                (bool*[]){ &editorOpen, &settingsOpen, &assetsOpen, &consoleOpen, &testOpen, &sceneViewOpen }, 6u);

            UIPopFloat(UIFloat_CornerRadius);
            UIPopFloat(UIFloat_TextScale);
        }

        CLAY(CLAY_ID("TabBarRight"), {
            .layout = { 
                .sizing = { CLAY_SIZING_FIXED((screenWidth >> 1) - 10), CLAY_SIZING_GROW(0) },
                .childAlignment = { .x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 15,
            }
        }) {
            UIPushFloat(UIFloat_CornerRadius, 9.0f);
            UIButtonPushColors(UCOLOR_GOLD, UCOLOR_GOLD, UCOLOR_ORANGE);
            
            if (EditorTabBarIconButton(CLAY_ID("TabBarMinimizeProgram"), &editorMinimizeImage, (Clay_Dimensions) { 23, 23 }))
            {
                SDL_MinimizeWindow(g_SDLWindow);
            }

            UIButtonPushColors(UCOLOR_SUCCESS, UCOLOR_DARK_GREEN, UCOLOR_GREEN);
            if (EditorTabBarIconButton(CLAY_ID("TabBarMaximizeProgram"), EditorWindowIsMaximized() ? &editorRestoreImage : &editorMaximizeImage, (Clay_Dimensions) { 23, 23 }))
            {
                EditorToggleWindowMaximized();
            }
 
            UIButtonPushColors(UCOLOR_ERROR, UCOLOR_DARK_RED, UCOLOR_RED);
            if (EditorTabBarIconButton(CLAY_ID("TabBarExitProgram"), &editorCloseImage, (Clay_Dimensions) { 23, 23 }))
            {
                extern void DestroyMain();
                DestroyMain();
            }
         
            UIPopFloat(UIFloat_CornerRadius);
            UIButtonPopColors();
            UIButtonPopColors();
            UIButtonPopColors();
        }
    }

    editorTabBarButtonBoxCount = 0u;
    EditorCacheTabBarButtonBox(CLAY_ID("Scene"));
    EditorCacheTabBarButtonBox(CLAY_ID("Windows"));
    EditorCacheTabBarButtonBox(CLAY_ID("TabBarMinimizeProgram"));
    EditorCacheTabBarButtonBox(CLAY_ID("TabBarMaximizeProgram"));
    EditorCacheTabBarButtonBox(CLAY_ID("TabBarExitProgram"));
    EditorCacheTabBarButtonBox(CLAY_ID("TabBarLogo"));

    UIPopFloat(UIFloat_BorderWidth);

    CLAY(CLAY_ID("GraphicsEditorRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
        }
    }) {

        if (showFps) ShowFps();
        bool sceneViewWasVisible = sceneViewVisible;
        if (!sceneViewWasVisible) DrawSceneLightGizmos(&g_Camera);
        DrawSceneViewWindow();
        WindowTestUI();
        DrawSettingsWindow();
        DrawSceneWindow(&sceneOpen);
        DrawTexturesWindow(&texturesOpen);
        DrawAssetsWindow(&assetsOpen);
        DrawTerrainWindow(&terrainOpen);
        DrawConsoleWindow(&consoleOpen);
        DrawGraphicsWindow();
    }

    if (EditorOpenWindowMask() != openMaskBefore) UIWindowMarkLayoutChanged();
    if (UIWindowConsumeLayoutChanged() && editorContinueLastUI)
    {
        const char* layoutPath = ConcatWithTempPath(EDITOR_UI_LAYOUT_PATH, sizeof(EDITOR_UI_LAYOUT_PATH));
        if (layoutPath)
        {
            UIWindowSaveLayout(layoutPath);
            ArenaPopGlobal(4096);
        }
    }

    // the renderer sizes the scene to the view content (0 0 = fullscreen like before)
    if (sceneViewVisible) SetSceneViewSize((u32)(sceneViewContentSize.x + 0.5f), (u32)(sceneViewContentSize.y + 0.5f));
    else SetSceneViewSize(0u, 0u);

    Clay_RenderCommandArray commands = UIEndLayout();
    UIRenderCommands(&commands);

    // with the scene in a window the light icons draw after the window quads so they
    // appear on top of the scene image, filtered to spots where the view is unobstructed
    if (sceneViewVisible) DrawSceneLightGizmos(&g_Camera);
}

void UIRenderCallback(void)
{
    // static char textArea[512] = "Text area 中文测试 日本語テスト\nArabic: العربية\nGreek: Ελληνικά";
    // UIText("SDF + Slug Immediate UI", (float2){ 56.0f, 56.0f });
    // UITextArea("Text Area", (float2){ 56.0f, 292.0f }, textArea, (u32)sizeof(textArea), (float2){ 520.0f, 160.0f });
      
    if (GetKeyPressed(SDLK_F7)) showFps = !showFps;
    GraphicsEditorUI();
}
