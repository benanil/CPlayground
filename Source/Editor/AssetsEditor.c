// assets browser window: folder tree, file grid, search and file operations
#include "EditorInternal.h"
#include "Include/Platform.h"
#include "Include/Random.h"
#include "Include/Algorithm.h"
#include "Include/FileSystem.h"
#include <SDL3/SDL_misc.h> // SDL_OpenURL

extern WindowState g_WindowState;

#define ASSET_ROOT          "Assets"
#define ASSET_MAX_ENTRIES   (1024*16)
#define ASSET_MAX_PATH      1024
#define ASSET_FOLDER_STATES 512
#define ASSET_BOX_WIDTH     92.0f
#define ASSET_BOX_HEIGHT    116.0f
#define ASSET_BOX_GAP       10.0f
#define ASSET_DOUBLE_CLICK_SECONDS 0.4f

typedef struct AssetEntry_
{
    u16  pathLen;
    u16  nameOffset; // file name start inside path
    s16  parent;     // entry index, -1 when directly under the assets root
    u8   isDir;
    char path[ASSET_MAX_PATH];
} AssetEntry;

// directory snapshot, rescanned periodically because nesting VisitFolder calls
// inside a visit callback would alias the arena buffer it iterates
static AssetEntry assetEntries[ASSET_MAX_ENTRIES];
static u32  assetNumEntries;
static bool assetDbDirty = true;
static f32  assetLastScan;

static u16  assetGridItems[ASSET_MAX_ENTRIES];
static u32  assetNumGridItems;

static char assetCurrentFolder[ASSET_MAX_PATH] = ASSET_ROOT;
static char assetSelectedPath[ASSET_MAX_PATH];
static char assetCopiedPath[ASSET_MAX_PATH];
static char assetSearchText[64];
static char assetNameInput[128];
static bool assetCreatePopupOpen;
static bool assetCreateIsFile;
static bool assetDeletePopupOpen;
static bool assetTreeRootOpen = true;
static f32  assetTreeWidth = 220.0f;
static bool assetTreeDragging;
static bool assetTreeCursorOwned;

// the platform double click flag is set on release, item clicks fire on press,
// so detect double clicks here: two quick presses on the same item
static f32  assetLastClickTime = -10.0f;
static u32  assetLastClickHash;

// folder tree open flags keyed by path hash, linear probe insert-only
static struct AssetFolderState_ { u32 hash; u8 open; } assetFolderStates[ASSET_FOLDER_STATES];

static u8* AssetFolderOpenSlot(u32 hash)
{
    u32 i = hash % ASSET_FOLDER_STATES;
    for (u32 probe = 0u; probe < ASSET_FOLDER_STATES; probe++, i = (i + 1u) % ASSET_FOLDER_STATES)
    {
        if (assetFolderStates[i].hash == hash) return &assetFolderStates[i].open;
        if (assetFolderStates[i].hash == 0u)
        {
            assetFolderStates[i].hash = hash;
            return &assetFolderStates[i].open;
        }
    }
    static u8 overflow;
    overflow = 0u;
    return &overflow;
}

static void AssetCollectFn(const char* path, void* data)
{
    (void)data;
    static int numTry = 0;
    if (assetNumEntries >= ASSET_MAX_ENTRIES && numTry++ < 16) {
        AX_WARN("asset folder ASSET_MAX_ENTRIES reached!");
        return;
    }
    u32 len = (u32)StringLength(path);
    if ((len == 0u || len >= ASSET_MAX_PATH) && numTry++ < 16) {
        AX_WARN("asset folder path is too long");
        return;
    }

    AssetEntry* e = &assetEntries[assetNumEntries++];
    MemCopy(e->path, path, len + 1u);
    e->pathLen = (u16)len;
    e->isDir = IsFolder(path) ? 1u : 0u;
    e->parent = -1;
    e->nameOffset = 0u;
    for (u32 i = 0u; i < len; i++)
        if (path[i] == '/' || path[i] == '\\') e->nameOffset = (u16)(i + 1u);
}

static void AssetRefresh(void)
{
    assetNumEntries = 0u;
    VisitFolder(ASSET_ROOT, AssetCollectFn, NULL, true);

    for (u32 i = 0u; i < assetNumEntries; i++)
    {
        AssetEntry* e = &assetEntries[i];
        u32 dirLen = e->nameOffset > 0u ? e->nameOffset - 1u : 0u;
        for (u32 j = 0u; j < assetNumEntries; j++)
        {
            const AssetEntry* dir = &assetEntries[j];
            if (j == i || !dir->isDir || dir->pathLen != dirLen) continue;
            bool match = true;
            for (u32 c = 0u; c < dirLen && match; c++) match = dir->path[c] == e->path[c];
            if (match) { e->parent = (s16)j; break; }
        }
    }
    assetDbDirty = false;
    assetLastScan = TimeSinceStartup();
}

static void AssetSetCurrentFolder(const char* path)
{
    u32 len = (u32)StringLength(path);
    if (len >= ASSET_MAX_PATH) {
        AX_WARN("AssetSetCurrentFolder path is too long");
        return;
    }
    MemsetZero(assetCurrentFolder, sizeof(assetCurrentFolder));
    MemCopy(assetCurrentFolder, path, len);
    assetSelectedPath[0] = '\0';
}

// out: entry index, -1 for the assets root, -2 when the path no longer exists
static s32 AssetFindEntry(const char* path)
{
    if (StringEqual(path, ASSET_ROOT, sizeof(ASSET_ROOT))) return -1;
    for (u32 i = 0u; i < assetNumEntries; i++)
        if (StringEqual(assetEntries[i].path, path, StringLength(path) + 1)) return (s32)i;
    return -2;
}

static void AssetOpenWithOS(const char* path)
{
    char absolute[768] = {0};
    char url[1024] = "file:///";
    AbsolutePath(path, absolute, sizeof(absolute));
    u32 len = (u32)(sizeof("file:///") - 1u);
    for (const char* c = absolute; *c && len + 1u < sizeof(url); c++)
        url[len++] = (*c == '\\') ? '/' : *c;
    url[len] = '\0';
    AX_LOG("open folder: %s", absolute);
    SDL_OpenURL(url);
}

static void AssetPaste(void)
{
    if (assetCopiedPath[0] == '\0' || !FileExist(assetCopiedPath)) return;
    if (IsFolder(assetCopiedPath))
    {
        AX_WARN("folder paste is not supported");
        return;
    }
    char destination[ASSET_MAX_PATH * 2];
    MemsetZero(destination, sizeof(destination));
    if (!CombinePaths(destination, sizeof(destination), assetCurrentFolder, GetFileName(assetCopiedPath)))
    {
        AX_WARN("combining paths when asset pasting failed");
        return;
    }
    if (StringEqual(destination, assetCopiedPath, StringLength(assetCopiedPath) + 1))
    {
        AX_WARN("trying to paste into same file");
        return;
    }
    ACopyFile(assetCopiedPath, destination, NULL);
    assetDbDirty = true;
}


//------------------------------------------------------------------------
// Icons

enum AssetIcon_
{
    AssetIcon_Folder = 0,
    AssetIcon_File,
    AssetIcon_CPP,
    AssetIcon_HPP,
    AssetIcon_HLSL,
    AssetIcon_GLSL,
    AssetIcon_Material,
    AssetIcon_Image,
    AssetIcon_Audio,
    AssetIcon_Mesh,
    AssetIcon_Search,
    AssetIcon_Count
};

static bool assetIconsLoaded;
static Texture assetIconTextures[AssetIcon_Count];
static UIImageData assetIconImages[AssetIcon_Count];

static void AssetLoadIcons(void)
{
    if (assetIconsLoaded) return;
    static const char* iconPaths[AssetIcon_Count] = {
        "Assets/Textures/Icons/folder.png",
        "Assets/Textures/Icons/file.png",
        "Assets/Textures/Icons/cpp_icon.png",
        "Assets/Textures/Icons/hpp_icon.png",
        "Assets/Textures/Icons/hlsl_file_icon.png",
        "Assets/Textures/Icons/glsl.png",
        "Assets/Textures/Icons/Material_Icon.png",
        "Assets/Textures/Icons/image_file.png",
        "Assets/Textures/Icons/audio_file.png",
        "Assets/Textures/Icons/mesh.png",
        "Assets/Textures/Icons/magnifying-glass.png"
    };
    for (u32 i = 0u; i < AssetIcon_Count; i++)
    {
        assetIconTextures[i] = rImportTexture(iconPaths[i], TexFlags_MipMap, GetFileName(iconPaths[i]));
        assetIconImages[i] = UIImageFromTexture(&assetIconTextures[i]);
    }
    assetIconsLoaded = true;
}

static UIImageData* AssetIconForEntry(const AssetEntry* e)
{
    if (e->isDir) return &assetIconImages[AssetIcon_Folder];
	const char* path = e->path;
	int pathLen = StringLength(path);
    if (FileHasExtension(path, pathLen, ".gltf") || FileHasExtension(path, pathLen, ".glb") || FileHasExtension(path, pathLen, ".fbx") || FileHasExtension(path, pathLen, ".obj") || FileHasExtension(path, pathLen, ".abm"))
        return &assetIconImages[AssetIcon_Mesh];
    if (FileHasExtension(path, pathLen, ".png") || FileHasExtension(path, pathLen, ".jpg") || FileHasExtension(path, pathLen, ".jpeg") || FileHasExtension(path, pathLen, ".dds") || FileHasExtension(path, pathLen, ".basis") || FileHasExtension(path, pathLen, ".ctex"))
        return &assetIconImages[AssetIcon_Image];
    if (FileHasExtension(path, pathLen, ".wav") || FileHasExtension(path, pathLen, ".ogg") || FileHasExtension(path, pathLen, ".mp3"))
        return &assetIconImages[AssetIcon_Audio];
    if (FileHasExtension(path, pathLen, ".hlsl"))
        return &assetIconImages[AssetIcon_HLSL];
    if (FileHasExtension(path, pathLen, ".glsl") || FileHasExtension(path, pathLen, ".vert") || FileHasExtension(path, pathLen, ".frag"))
        return &assetIconImages[AssetIcon_GLSL];
    if (FileHasExtension(path, pathLen, ".mat"))
        return &assetIconImages[AssetIcon_Material];
    if (FileHasExtension(path, pathLen, ".c") || FileHasExtension(path, pathLen, ".cpp") || FileHasExtension(path, pathLen, ".cs"))
        return &assetIconImages[AssetIcon_CPP];
    if (FileHasExtension(path, pathLen, ".h") || FileHasExtension(path, pathLen, ".hpp"))
        return &assetIconImages[AssetIcon_HPP];
    return &assetIconImages[AssetIcon_File];
}

//------------------------------------------------------------------------
// Folder tree

static bool AssetDirHasSubdir(s32 entryIdx)
{
    for (u32 i = 0u; i < assetNumEntries; i++)
        if (assetEntries[i].parent == entryIdx && assetEntries[i].isDir) return true;
    return false;
}

static void AssetFolderTree(s32 parentIdx, u32 depth)
{
    for (u32 i = 0u; i < assetNumEntries; i++)
    {
        AssetEntry* e = &assetEntries[i];
        if (e->parent != parentIdx || !e->isDir) continue;

        u32 hash = StringToHash(e->path, 5381u);
        u8* openSlot = AssetFolderOpenSlot(hash);
        bool open = *openSlot != 0u;

        u32 flags = 0u;
        if (!AssetDirHasSubdir((s32)i)) flags |= UITreeNodeFlags_Leaf;
        if (StringEqual(e->path, assetCurrentFolder, StringLength(assetCurrentFolder) + 1)) flags |= UITreeNodeFlags_Selected;

        bool selectClicked = false;
        Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("AssetTreeNode"), hash);
        if (UITreeNode(id, UIStr(e->path + e->nameOffset), depth, flags, open, &selectClicked))
        {
            *openSlot ^= 1u;
            open = !open;
        }
        if (selectClicked) AssetSetCurrentFolder(e->path);
        if (open) AssetFolderTree((s32)i, depth + 1u);
    }
}

//------------------------------------------------------------------------
// Grid

static void AssetBuildGridList(void)
{
    assetNumGridItems = 0u;
    bool searching = assetSearchText[0] != '\0';

    s32 current = AssetFindEntry(assetCurrentFolder);
    if (current == -2)
    {
        AssetSetCurrentFolder(ASSET_ROOT);
        current = -1;
    }

    for (u32 pass = 0u; pass < 2u; pass++) // folders first, then files
    {
        for (u32 i = 0u; i < assetNumEntries && assetNumGridItems < ASSET_MAX_ENTRIES; i++)
        {
            const AssetEntry* e = &assetEntries[i];
            if ((e->isDir != 0u) != (pass == 0u)) continue;
            if (searching)
            {
                if (!StringContains(e->path + e->nameOffset, assetSearchText)) continue;
            }
            else if (e->parent != current) continue;
            assetGridItems[assetNumGridItems++] = (u16)i;
        }
    }
}

static Clay_String AssetItemLabel(const char* name)
{
    u32 len = (u32)StringLength(name);
    if (len <= 13u) return UIStr(name);

    char* text = UIFrameStringAlloc(32u);
    if (!text) return UIStr(name);
    u32 out = 0u;
    for (u32 i = 0u; i < len && out < 28u; i++)
    {
        text[out++] = name[i];
        if (i == 12u) text[out++] = '\n'; // long names rarely have spaces, force the wrap point
    }
    if (len > 26u) { text[out - 2u] = '.'; text[out - 1u] = '.'; }
    text[out] = '\0';
    return (Clay_String) { .isStaticallyAllocated = false, .length = (s32)out, .chars = text };
}

static void AssetSelect(const AssetEntry* e)
{
    MemsetZero(assetSelectedPath, sizeof(assetSelectedPath));
    MemCopy(assetSelectedPath, e->path, e->pathLen);
}

static void AssetDrawGridItem(u32 entryIdx, u32 itemIdx)
{
    AssetEntry* e = &assetEntries[entryIdx];
    const char* name = e->path + e->nameOffset;
    Clay_ElementId id = Clay_GetElementIdWithIndex(CLAY_STRING("AssetItem"), itemIdx);
    bool isSelected = StringEqual(e->path, assetSelectedPath, StringLength(assetSelectedPath) + 1);

    CLAY(id, {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(ASSET_BOX_WIDTH), CLAY_SIZING_FIXED(ASSET_BOX_HEIGHT) },
            .padding = { 6, 6, 6, 4 },
            .childGap = 4,
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER }
        },
        .backgroundColor = Clay_Hovered() ? UIGetClayColor(UIColor_Hovered) : (Clay_Color){ 30, 30, 30, 160 },
        .cornerRadius = CLAY_CORNER_RADIUS(UIGetFloat(UIFloat_CornerRadius)),
        .border = { .color = isSelected ? UIGetClayColor(UIColor_SelectedBorder) : UIGetClayColor(UIColor_Border), .width = CLAY_BORDER_OUTSIDE(1) }
    }) {
        if (UIClicked())
        {
            u32 clickHash = StringToHash(e->path, 5381u);
            f32 now = TimeSinceStartup();
            bool doubleClicked = clickHash == assetLastClickHash && now - assetLastClickTime < ASSET_DOUBLE_CLICK_SECONDS;
            assetLastClickHash = clickHash;
            assetLastClickTime = doubleClicked ? -10.0f : now; // eat it so a third click does not refire

            AssetSelect(e);
            if (doubleClicked)
            {
				int pathLen = StringLength(e->path);
                if (e->isDir) AssetSetCurrentFolder(e->path);
                else if (FileHasExtension(e->path, pathLen, ".scene")) EditorOpenScene(e->path);
                else AssetOpenWithOS(e->path);
            }
        }
        // right click targets the item for the context menu ops
        if (Clay_Hovered() && GetMousePressed(MouseButton_Right)) AssetSelect(e);

        CLAY(CLAY_ID_LOCAL("Icon"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(56.0f), CLAY_SIZING_FIXED(56.0f) } },
            .image = { .imageData = AssetIconForEntry(e) }
        }) {}

        CLAY_TEXT(AssetItemLabel(name), CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = UIGetClayColor(UIColor_Text),
            .textAlignment = CLAY_TEXT_ALIGN_CENTER
        }));
    }
}

static void AssetDrawGrid(void)
{
    Clay_ElementData gridData = Clay_GetElementData(CLAY_ID("AssetsGrid"));
    f32 gridWidth = gridData.found ? gridData.boundingBox.width : 600.0f;
    u32 columns = Maxu32((u32)((gridWidth - 24.0f) / (ASSET_BOX_WIDTH + ASSET_BOX_GAP)), 1u);

    if (assetNumGridItems == 0u)
    {
        CLAY_TEXT(assetSearchText[0] ? CLAY_STRING("No matches.") : CLAY_STRING("Empty folder."), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = UIGetClayColor(UIColor_SubText)
        }));
        return;
    }

    for (u32 start = 0u; start < assetNumGridItems; start += columns)
    {
        Clay_ElementId rowId = Clay_GetElementIdWithIndex(CLAY_STRING("AssetGridRow"), start);
        CLAY(rowId, {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(ASSET_BOX_HEIGHT) },
                .childGap = (u16)ASSET_BOX_GAP,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            u32 end = Minu32(start + columns, assetNumGridItems);
            for (u32 i = start; i < end; i++) AssetDrawGridItem(assetGridItems[i], i);
        }
    }
}

//------------------------------------------------------------------------
// Nav bar and right click events

static void AssetNavBar(void)
{
    CLAY(CLAY_ID("AssetsNavRow"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28.0f) },
            .childGap = 8,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        }
    }) {
        UIPushFloatAdd(UIFloat_TextScale, -0.2f);
        bool back = UIButton(CLAY_ID("AssetsBack"), CLAY_STRING("<"), (Clay_Dimensions){ 30.0f, 24.0f }, false);
        UIPopFloat(UIFloat_TextScale);
        if ((back || GetMouseReleased(MouseButton_Backward)) && StringLength(assetCurrentFolder) > (int)sizeof(ASSET_ROOT) - 1)
        {
            PathGoBackwards(assetCurrentFolder, StringLength(assetCurrentFolder), true);
            assetSelectedPath[0] = '\0';
        }

        CLAY_TEXT(UIStr(assetCurrentFolder), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = UIGetClayColor(UIColor_Text),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        }));

        CLAY(CLAY_ID_LOCAL("Spacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1.0f) } } }) {}

        CLAY(CLAY_ID_LOCAL("SearchIcon"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(18.0f), CLAY_SIZING_FIXED(18.0f) } },
            .image = { .imageData = &assetIconImages[AssetIcon_Search] }
        }) {}
        static UITextAreaCustomData searchData;
        searchData.type = UICustomType_TextArea;
        searchData.buffer = assetSearchText;
        searchData.capacity = sizeof(assetSearchText);
        searchData.flags = UITextAreaFlags_CenterY;
        CLAY(CLAY_ID("AssetsSearchBox"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(180.0f), CLAY_SIZING_FIXED(24.0f) } },
            .custom = { .customData = &searchData }
        }) {}
    }
}

static void AssetEventOpenFolder(void* unused) {
    (void)unused;
    AssetOpenWithOS(assetCurrentFolder);
}

static void AssetEventCopy(void* unused) {
    (void)unused;
    if (assetSelectedPath[0]) MemCopy(assetCopiedPath, assetSelectedPath, sizeof(assetCopiedPath));
}

static void AssetEventPaste(void* unused) {
    (void)unused;
    AssetPaste();
}

static void AssetEventDelete(void* unused) {
    (void)unused;
    if (assetSelectedPath[0]) assetDeletePopupOpen = true;
}

static void AssetEventCreateFolder(void* unused) {
    (void)unused;
    assetCreatePopupOpen = true;
    assetCreateIsFile = false;
    assetNameInput[0] = '\0';
}

static void AssetEventCreateFile(void* unused) {
    (void)unused;
    assetCreatePopupOpen = true;
    assetCreateIsFile = true;
    assetNameInput[0] = '\0';
}

static void AssetEventImportToScene(void* unused) {
    (void)unused;
    if (assetSelectedPath[0]) EditorImportMeshToScene(assetSelectedPath);
}

static void AssetEventImportWithDetail(void* unused) {
    (void)unused;
    if (assetSelectedPath[0]) EditorOpenImportDetail(assetSelectedPath);
}

static void AssetEventOpenScene(void* unused) {
    (void)unused;
    if (assetSelectedPath[0]) EditorOpenScene(assetSelectedPath);
}

//------------------------------------------------------------------------
// Split divider and popups

// resize the tree pane by dragging the split divider, uses last frame's layout
static void AssetTreeDividerDrag(void)
{
    Clay_ElementData divider = Clay_GetElementData(CLAY_ID("AssetsSplitDivider"));
    if (!divider.found) return;

    Clay_PointerData pointer = Clay_GetPointerState();
    f32 mx = pointer.position.x;
    f32 my = pointer.position.y;
    f32 centerX = divider.boundingBox.x + divider.boundingBox.width * 0.5f;
    f32 testDistance = assetTreeDragging ? 60.0f : 6.0f;
    bool near = Absf32(mx - centerX) < testDistance &&
                my >= divider.boundingBox.y && my <= divider.boundingBox.y + divider.boundingBox.height;

    if (near || assetTreeDragging)
    {
        wSetCursor(wCursor_ResizeEW);
        assetTreeCursorOwned = true;
    }
    else if (assetTreeCursorOwned)
    {
        wSetCursor(wCursor_Default);
        assetTreeCursorOwned = false;
    }

    if (near && GetMousePressed(MouseButton_Left)) assetTreeDragging = true;
    if (!GetMouseDown(MouseButton_Left)) assetTreeDragging = false;
    if (assetTreeDragging)
    {
        Clay_ElementData row = Clay_GetElementData(CLAY_ID("AssetsSplitRow"));
        if (row.found) assetTreeWidth = Clampf32(mx - row.boundingBox.x, 80.0f, 600.0f);
    }
}

static void AssetCreatePopup(void)
{
    if (!assetCreatePopupOpen) return;
    float2 center = { g_WindowState.prev_width * 0.5f - 190.0f, g_WindowState.prev_height * 0.5f - 90.0f };
    const char* title = assetCreateIsFile ? "Create File" : "Create Folder";
    if (!UIBeginWindow(title, center, (float2){ 380.0f, 190.0f }, &assetCreatePopupOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Name:"), CLAY_TEXT_CONFIG({
        .fontSize = 14,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    static UITextAreaCustomData nameData;
    nameData.type = UICustomType_TextArea;
    nameData.buffer = assetNameInput;
    nameData.capacity = sizeof(assetNameInput);
    nameData.flags = UITextAreaFlags_CenterY;
    CLAY(CLAY_ID("AssetCreateNameBox"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26.0f) } },
        .custom = { .customData = &nameData }
    }) {}

    CLAY(CLAY_ID("AssetCreateButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("AssetCreateOk"), CLAY_STRING("Create"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            if (assetNameInput[0])
            {
                char combined[ASSET_MAX_PATH * 2];
                MemsetZero(combined, sizeof(combined));
                if (CombinePaths(combined, sizeof(combined), assetCurrentFolder, assetNameInput))
                {
                    if (assetCreateIsFile)
                    {
                        AFile file = AFileOpen(combined, AOpenFlag_WriteText);
                        AFileClose(file);
                    }
                    else CreateFolder(combined);
                    assetDbDirty = true;
                }
            }
            assetCreatePopupOpen = false;
        }
        if (UIButton(CLAY_ID("AssetCreateCancel"), CLAY_STRING("Cancel"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            assetCreatePopupOpen = false;
        }
    }
    UIEndWindow();
}

static void AssetDeletePopup(void)
{
    if (!assetDeletePopupOpen) return;
    if (assetSelectedPath[0] == '\0')
    {
        assetDeletePopupOpen = false;
        return;
    }

    float2 center = { g_WindowState.prev_width * 0.5f - 210.0f, g_WindowState.prev_height * 0.5f - 85.0f };
    if (!UIBeginWindow("Delete?", center, (float2){ 420.0f, 180.0f }, &assetDeletePopupOpen, UIWindowFlags_NoResize)) return;

    CLAY_TEXT(CLAY_STRING("Delete this resource?"), CLAY_TEXT_CONFIG({
        .fontSize = 15,
        .textColor = UIGetClayColor(UIColor_Text)
    }));
    CLAY_TEXT(UIStr(assetSelectedPath), CLAY_TEXT_CONFIG({
        .fontSize = 13,
        .textColor = UIGetClayColor(UIColor_SubText)
    }));

    CLAY(CLAY_ID("AssetDeleteButtons"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(34.0f) },
            .childGap = 10,
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        if (UIButton(CLAY_ID("AssetDeleteYes"), CLAY_STRING("Yes"), (Clay_Dimensions){ 96.0f, 30.0f }, false) || GetKeyPressed('\r'))
        {
            if (IsFolder(assetSelectedPath)) RemoveFolder(assetSelectedPath, NULL);
            else RemoveFile(assetSelectedPath);
            assetSelectedPath[0] = '\0';
            assetDbDirty = true;
            assetDeletePopupOpen = false;
        }
        if (UIButton(CLAY_ID("AssetDeleteNo"), CLAY_STRING("No"), (Clay_Dimensions){ 96.0f, 30.0f }, false))
        {
            assetDeletePopupOpen = false;
        }
    }
    UIEndWindow();
}

void DrawAssetsWindow(bool* open)
{
    Clay_ElementId windowID = (Clay_ElementId) { .id = StringToHash("AssetsWindow", 5381u) };
    if (UIBeginWindowId(windowID, "Assets", (float2) { 366.0f, 640.0f }, (float2) { 1054.0f, 360.0f }, open, UIWindowFlags_RightClickable))
    {
        AssetLoadIcons();
        if (assetDbDirty || TimeSinceStartup() - assetLastScan > 2.0f) AssetRefresh();

        UIRightClickAddEvent("Open Folder", AssetEventOpenFolder, NULL);
        if (assetSelectedPath[0])
        {
			int pathLen = StringLength(assetSelectedPath);
            if (IsMeshPath(assetSelectedPath))
            {
                UIRightClickAddEvent("Import to Scene", AssetEventImportToScene, NULL);
                UIRightClickAddEvent("Import with Detail", AssetEventImportWithDetail, NULL);
            }
            if (FileHasExtension(assetSelectedPath, pathLen, ".scene"))
				UIRightClickAddEvent("Open Scene", AssetEventOpenScene, NULL);
            UIRightClickAddEvent("Copy", AssetEventCopy, NULL);
            UIRightClickAddEvent("Delete", AssetEventDelete, NULL);
        }
        if (assetCopiedPath[0]) UIRightClickAddEvent("Paste", AssetEventPaste, NULL);
        UIRightClickAddEvent("Create Folder", AssetEventCreateFolder, NULL);
        UIRightClickAddEvent("Create File", AssetEventCreateFile, NULL);

        AssetNavBar();

        f32 splitHeight = UIWindowRemainingHeight(windowID, CLAY_ID("AssetsSplitRow"), 0.0f);
        CLAY(CLAY_ID("AssetsSplitRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(splitHeight) },
                .childGap = 6,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            CLAY(CLAY_ID("AssetsTreePane"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(assetTreeWidth), CLAY_SIZING_GROW(0) } }
            }) {
                CLAY(CLAY_ID("AssetsTreeScroll"), UIScrollPanelDeclaration(0.0f, 2u)) {
                    bool rootClicked = false;
                    u32 rootFlags = StringEqual(assetCurrentFolder, ASSET_ROOT, sizeof(ASSET_ROOT)) ? UITreeNodeFlags_Selected : 0u;
                    assetTreeRootOpen ^= UITreeNode(CLAY_ID("AssetTreeRoot"), CLAY_STRING(ASSET_ROOT), 0u, rootFlags, assetTreeRootOpen, &rootClicked);
                    if (rootClicked) AssetSetCurrentFolder(ASSET_ROOT);
                    if (assetTreeRootOpen) AssetFolderTree(-1, 1u);
                }
            }
            CLAY(CLAY_ID("AssetsSplitDivider"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(4.0f), CLAY_SIZING_GROW(0) } },
                .backgroundColor = { 55, 55, 55, 160 },
                .cornerRadius = CLAY_CORNER_RADIUS(2.0f)
            }) {}
            CLAY(CLAY_ID("AssetsGrid"), UIScrollPanelDeclaration(0.0f, 8u)) {
                AssetBuildGridList();
                AssetDrawGrid();
            }
        }
        AssetTreeDividerDrag();
        UIEndWindow();
    }

    // popup windows are separate floating windows, never nested in the assets window
    AssetCreatePopup();
    AssetDeletePopup();
}
