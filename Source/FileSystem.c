
/********************************************************************************
*    Purpose: Reading from files and Writing to files                           *
*    Author : Anilcan Gulkaya 2023 anilcangulkaya7@gmail.com github @benanil    *
********************************************************************************/

#include "Include/Memory.h"
#include "Include/Platform.h"
#include "Include/FileSystem.h"
#include "Include/Algorithm.h" // ParseInt

#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <io.h>
    #include <direct.h> 
    #include <windows.h>
    #include "Include/dirent.h"
    #define F_OK 0
#else 
    #include <unistd.h>
    #include <sys/types.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <limits.h>
    #define _rmdir rmdir
    #define _mkdir mkdir
    #define _fileno fileno
    #define _filelengthi64 filelength
    #ifndef MAX_PATH
    #define MAX_PATH PATH_MAX
    #endif
#endif

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

extern "C" android_app* g_android_app;
#endif


#ifdef PLATFORM_WINDOWS
    #define ASTL_FILE_SEPERATOR ('\\')
    typedef void* HANDLE; // forward declare Windows handle
#else
    #define ASTL_FILE_SEPERATOR ('/')
    int GetCurrentDirectory(int size, char* outPath);
    #if !defined(__ANDROID__)
    #include <stdio.h>
    #endif
#endif

// path must be null terminated string
const char* GetFileExtension(const char* path, int size)
{
    while (path[size-1] != '.' && size > 0)
        size--;
    return path + size;
}

int ChangeExtension(char* path, int pathLen, const char* newExt)
{
    int lastDot = pathLen - 1, oldLen = 0;
    while (path[lastDot - 1] != '.' && lastDot >= 0)
        lastDot--, oldLen++;

    int i = lastDot, newLen = 0;
    for (; *newExt; i++)
        path[i] = *newExt++, newLen++;
    path[i++] = '\0';
    // clean the right with zeros
    while (i < pathLen)
        path[i++] = '\0';
    return pathLen + (oldLen - newLen);
}

bool FileHasExtension(const char* path, int size, const char* extension)
{
    int extLen = 0;
    // go to end of extension
    while (extension[1] != 0)
        extension++, extLen++;
    
    if (size <= extLen) return false;
    
    for (int i = 0; i <= extLen; i++)
    {
        if (*extension-- != path[--size])
            return false;
    }
    return true;
}

void GetBaseDir(const char* path, char* out)
{
    s32 len = (s32)StringLengthSafe(path, 2048);
    for (s32 i = len - 1; i >= 0; --i)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            SmallMemCpy(out, path, i + 1);
            out[i + 1] = 0;
            return;
        }
    }
    SmallMemCpy(out, "./", 3);
}

s32 GetFileNameNoExt(const char* path, char* out)
{
    s32 len = (s32)StringLengthSafe(path, 512);
    s32 start = 0;

    for (s32 i = len - 1; i >= 0; --i)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            start = i + 1; 
            break; 
        }
    }

    s32 end = len;
    for (s32 i = len - 1; i >= start; --i)
    {
        if (path[i] == '.') 
        { 
            end = i; 
            break; 
        }
    }

    s32 n = end - start;
    SmallMemCpy(out, path + start, n);
    out[n] = 0;
    return n;
}

// returns pointer to the end of the new path
char* PathGoBackwards(char* path, int end, bool skipSeparator)
{
    if (path == NULL || end <= 0) return path;

    if (path[end-1] == '/' || path[end-1] == '\\')
        path[--end] = '\0';
        
    while (end >= 0 && (path[end] != '/' && path[end] != '\\')) {
        path[end--] = '\0'; // Null-terminate the string.
    }
    
    if (skipSeparator && end >= 0) {
        path[end--] = '\0'; // Null-terminate again to remove the separator.
    }

    return path + end + 1; // Return the new starting point of the path.
}

// returns length of filename
int CopyFilename(char* out, uint64_t outLen, const char* path, int end)
{
    int numChars = 0;
    while (numChars < outLen && end >= 0 && (path[end] != '/' && path[end] != '\\')) {
        out[numChars++] = path[end--]; // Null-terminate the string.
    }
    
    // reverse
    for (int i = 0, j = numChars-1; i < j; i++, j--) {
        char t = out[i];
        out[i] = out[j];
        out[j] = t;
    }

    return numChars; // Return the new starting point of the path.
}

// these functions works fine with file and folders
bool FileExist(const char* file)
{
#ifdef __ANDROID__
    AAsset* asset = AAssetManager_open(g_android_app->activity->assetManager, file, 0);
    if (asset == NULL) {
        return false;
    } else {
        AAsset_close(asset);
        return true;
    }
#elif defined(_WIN32)
    DWORD attr = GetFileAttributesA(file);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    return access(file, F_OK) == 0;
#endif
}

uint64_t FileSize(const char* file)
{
    #ifdef __ANDROID__
    AAsset* asset = AAssetManager_open(g_android_app->activity->assetManager, file, 0);
    if (asset) {
        off64_t sz = AAsset_getLength64(asset);
        AAsset_close(asset);
        return (uint64_t)sz;
    }
    return 0;

    #elif defined(_WIN32)
    struct _stat64 sb;
    if (_stat64(file, &sb) != 0)
        return 0;
    return (uint64_t)sb.st_size;

    #else
    FILE* fp = fopen(file, "rb");
    if (!fp)
        return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    long sz = ftell(fp);
    fclose(fp);

    if (sz < 0)
        return 0;

    return (uint64_t)sz;
    #endif
}

bool RenameFile(const char* oldFile, const char* newFile)
{
    return rename(oldFile, newFile) != 0;
}

bool CreateFolder(const char* folderName) 
{
    return _mkdir(folderName
               #ifndef _WIN32
               , 0777
               #endif 
               ) == 0;
}

bool RemoveFile(const char* file) {
    return remove(file);
}

bool IsFolder(const char* path)
{
  struct stat file_info; 
  return stat(path, &file_info) == 0 && (S_ISDIR(file_info.st_mode));
}

bool IsSymLink(const char* path)
{
    struct stat file_info; 
    return stat(path, &file_info) == 0 && (S_ISLNK(file_info.st_mode));
}

#ifdef __ANDROID__
struct AFile {
    AAsset* asset;
};

AFile AFileOpen(const char* fileName, AOpenFlag flag) {
    return { AAssetManager_open(g_android_app->activity->assetManager, fileName, 0) };
}

void AFileRead(void* dst, uint64_t size, AFile file, int alignment = 0) {
    AAsset_read(file.asset, dst, size);
}

void AFileSeekBegin(AFile file) {
    AAsset_seek(file.asset, 0, SEEK_SET);
}

void AFileSeek(long offset, AFile file) {
    AAsset_seek(file.asset, offset, SEEK_CUR);
}

void AFileWrite(const void* src, uint64_t size, AFile file, int alignment = 0)
{ }

void AFileClose(AFile file) {
    AAsset_close(file.asset);
}

bool AFileExist(AFile file) {
    return file.asset != NULL;
}

uint64_t AFileSize(AFile file) {
    return AAsset_getLength(file.asset);
}

int AFileReadLine(char* dst, int maxLen, AFile file) {
    if (maxLen <= 0 || !dst || !file.asset) return 0;
    
    int i = 0;
    char c;
    
    while (i < maxLen - 1) {
        if (AAsset_read(file.asset, &c, 1) != 1)
            break;
        dst[i++] = c;
        if (c == '\n') break;
    }
    
    dst[i] = '\0';
    return i;
}

#else

AFile AFileOpen(const char* fileName, AOpenFlag flag)
{
    AFile afile = {0};

    #ifdef _MSC_VER
    DWORD access = GENERIC_READ, creation = OPEN_EXISTING;

    switch (flag) {
        case AOpenFlag_WriteBinary: 
        case AOpenFlag_WriteText:
            access = GENERIC_WRITE;
            creation = CREATE_ALWAYS;
        break;
    }
    afile.file = CreateFileA(fileName, access, FILE_SHARE_READ, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);

    if (afile.file == INVALID_HANDLE_VALUE)
        afile.file = NULL;

    #else
    const char* modes[4] = { "rb", "wb", "r", "w" };
    afile.file = fopen(fileName, modes[flag]);
    #endif

    unsigned char bom[] = {0xEF, 0xBB, 0xBF};

    #ifndef _MSC_VER
    if (flag == AOpenFlag_WriteText && afile.file) {
        fwrite(bom, 1, sizeof(bom), afile.file);
    }
    #else
    if (flag == AOpenFlag_WriteText && afile.file) {
        DWORD written;
        WriteFile(afile.file, bom, sizeof(bom), &written, NULL);
    }
    #endif

    return afile;
}

void AFileRead(void* dst, uint64_t size, AFile file, int alignment) {
    #ifndef _MSC_VER
    fread(dst, alignment, size, file.file);
    #else
    ReadFile(file.file, dst, (DWORD)(size * alignment), NULL, NULL);
    #endif
}

void AFileWrite(const void* src, uint64_t size, AFile file, int alignment) {
    #ifndef _MSC_VER
    fwrite(src, alignment, size, file.file);
    #else
    WriteFile(file.file, src, (DWORD)(size * alignment), NULL, NULL);
    #endif
}

void AFileSeekBegin(AFile file) {
    #ifndef _MSC_VER
    fseek(file.file, 0, SEEK_SET);
    #else
    LARGE_INTEGER pos;
    pos.QuadPart = 0;
    SetFilePointerEx(file.file, pos, NULL, FILE_BEGIN);
    #endif
}

void AFileSeek(long offset, AFile file) {
    #ifndef _MSC_VER
    fseek(file.file, offset, SEEK_CUR);
    #else
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    SetFilePointerEx(file.file, pos, NULL, FILE_CURRENT);
    #endif
}

void AFileClose(AFile file) {
    #ifndef _MSC_VER
    fclose(file.file);
    #else
    CloseHandle(file.file);
    #endif
}

bool AFileExist(AFile file) {
    #ifndef _MSC_VER
    return file.file != NULL;
    #else
    return file.file && file.file != INVALID_HANDLE_VALUE;
    #endif
}

// Returns the number of characters read (excluding null terminator), or 0 on EOF/error
// Line includes the newline character if present
int AFileReadLine(char* dst, int maxLen, AFile file) 
{
    if (maxLen <= 0 || !dst) return 0;
    int i = 0;
    char c;
    
    #ifdef _MSC_VER
    DWORD bytesRead;
    while (i < maxLen - 1) {
        if (!ReadFile(file.file, &c, 1, &bytesRead, NULL) || bytesRead == 0)
            break;
        dst[i++] = c;
        if (c == '\n') break;
    }
    #else
    int ch;
    while (i < maxLen - 1) {
        ch = fgetc(file.file);
        if (ch == EOF) break;
        dst[i++] = (char)ch;
        if (ch == '\n') break;
    }
    #endif
    
    dst[i] = '\0';
    return i;
}

int AFileReadI32(char* dst, int maxLen, AFile file)
{
    AFileReadLine(dst, maxLen, file);
    int result = 0; ParsePositiveNumber(dst, &result);
    return result;
}

uint64_t AFileSize(AFile file)
{
    #ifdef _WIN32
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.file, &size))
        return 0;
    return (uint64_t)size.QuadPart;
    #elif defined(__ANDROID__)
    if (file.asset == NULL) return 0;
    return AAsset_getLength(file.asset);
    #else
    if (!file.file)
        return 0;

    struct stat sb;
    if (fstat(fileno(file.file), &sb) != 0)
        return 0;
    return (uint64_t)sb.st_size;
    #endif  
}

#endif

char* ReadAllFile(const char* fileName, char* buffer, uint64_t bufferSize)
{
    AFile file = AFileOpen(fileName, AOpenFlag_ReadBinary);
    if (!AFileExist(file))
        return NULL;

    uint64_t fileSize = AFileSize(file);
    if (fileSize > bufferSize)
    {
        AX_WARN("buffer is not enough! %s, bufferSize: %llu, needed:%llu", fileName, bufferSize, fileSize);
        return NULL;
    }

    AFileRead(buffer, fileSize, file, 1);
    AFileClose(file);
    return buffer;
}

char* ReadAllFileAlloc(const char* fileName)
{
    uint64_t fileSize = FileSize(fileName);
    return ReadAllFile(fileName, AllocZeroTLSFGlobal(fileSize + 1, 1), fileSize); // +1 for null terminator
}

// don't forget to free using FreeAllText
// fileName      : path of the file that we want to load
// buffer        : is pre allocated memory if exist. otherwise null
// numCharacters : if not null returns length of the imported string
// startText     : if its not null will be added to start of the buffer
// note: if you define it you are responsible of deleting the buffer
char* ReadAllText(const char* fileName, char* buffer, uint64_t* numCharacters, const char* startText)
{
    if (buffer == NULL || fileName == NULL) {
        return NULL; 
    }

    int startTextLen = startText ? StringLength(startText) : 0;
    AFile file = AFileOpen(fileName, AOpenFlag_ReadBinary);
    
    if (!AFileExist(file)) {
        AX_WARN("Error opening the file %s", fileName);
        return NULL; // Return an error code
    }
    
    // Determine the file size
    uint64_t file_size = AFileSize(file);

    if (file_size >= 3)
    {
        AFileRead(buffer, 3, file, 1);
        bool isBOM = buffer[0] == '\xEF' && buffer[1] == '\xBB' && buffer[2] == '\xBF';
        if (!isBOM) {
            AFileSeekBegin(file);
        }
    }

    if (startText) 
        while (*startText) *buffer++ = *startText++;
    
    // Read the entire file into the buffer
    AFileRead(buffer, file_size, file, 1);
    buffer[file_size] = '\0'; // Null-terminate the buffer
    AFileClose(file);
    if (numCharacters) 
        *numCharacters = (uint64_t)file_size + 1;
    return buffer - startTextLen;
}

char* ReadAllTextAlloc(const char* fileName, uint64_t* numCharacters, const char* startText)
{
    int startTextLen = startText ? StringLength(startText) : 0;
    // Allocate memory to store the entire file
    char* buffer = (char*)AllocZeroTLSFGlobal(FileSize(fileName) + 40 + startTextLen, 1); // +1 for null terminator
    return ReadAllText(fileName, buffer, numCharacters, startText);
}


void FreeAllText(char* text)
{
    DeAllocateTLSFGlobal(text);
}

void WriteAllBytes(const char *filename, const char *bytes, unsigned long size) 
{
    // Open the file for writing in binary mode
    AFile file = AFileOpen(filename, AOpenFlag_WriteBinary);
    if (!AFileExist(file)) 
    {
        perror("Failed to open file for writing");
        return;
    }

    AFileWrite(bytes, size, file, 1);

    AFileClose(file);
}

// buffer is pre allocated memory if exist. otherwise null. 
// note: if you define it you are responsible of deleting the buffer
void ACopyFile(const char* source, const char* dst, char* buffer)
{
    uint64_t sourceSize = 0;
    bool bufferProvided = buffer != 0;
    char* sourceFile;
    if (buffer) sourceFile = ReadAllText(source, buffer, &sourceSize, NULL);
    else        sourceFile = ReadAllTextAlloc(source, &sourceSize, NULL);

    AFile dstFile = AFileOpen(dst, AOpenFlag_WriteBinary);
    AFileWrite(sourceFile, sourceSize, dstFile, 1);
    AFileClose(dstFile);

    if (!bufferProvided) DeAllocateTLSFGlobal(sourceFile);
}

#ifndef _WIN32
int GetCurrentDirectory(int size, char* outPath) {
    return getcwd(outPath, size) != NULL;
}
#endif

static bool IsPathSeparator(char c)
{
    return c == '/' || c == '\\';
}

static bool IsAbsolutePath(const char* path)
{
    if (!path || !path[0])
        return false;

    // Unix: /home/test
    if (IsPathSeparator(path[0]))
        return true;

    // Windows-style path, but not using Windows API: C:/Test
    if (path[0] && path[1] == ':' && IsPathSeparator(path[2]))
        return true;

    return false;
}

static void RemoveTrailingSeparator(char* buffer, int* length)
{
    while (*length > 0 && IsPathSeparator(buffer[*length - 1]))
        --(*length);

    buffer[*length] = '\0';
}

static void PathGoBackOneFolder(char* buffer, int* length)
{
    RemoveTrailingSeparator(buffer, length);

    while (*length > 0 && !IsPathSeparator(buffer[*length - 1]))
        --(*length);

    RemoveTrailingSeparator(buffer, length);
}

static bool PushChar(char* buffer, int bufferSize, int* length, char c)
{
    if (*length + 1 >= bufferSize)
        return false;

    buffer[(*length)++] = c;
    buffer[*length] = '\0';
    return true;
}

static bool PushSeparator(char* buffer, int bufferSize, int* length)
{
    if (*length <= 0)
        return PushChar(buffer, bufferSize, length, ASTL_FILE_SEPERATOR);

    if (!IsPathSeparator(buffer[*length - 1]))
        return PushChar(buffer, bufferSize, length, ASTL_FILE_SEPERATOR);

    buffer[*length - 1] = ASTL_FILE_SEPERATOR;
    return true;
}

void AbsolutePath(const char* path, char* outBuffer, int bufferSize)
{
    if (!path || !outBuffer || bufferSize <= 0)
        return;

    outBuffer[0] = '\0';

    int currLength = 0;

    if (IsAbsolutePath(path))
    {
        // copy root / drive first
        while (*path && currLength + 1 < bufferSize)
        {
            char c = *path;

            if (IsPathSeparator(c))
                c = ASTL_FILE_SEPERATOR;

            outBuffer[currLength++] = c;
            outBuffer[currLength] = '\0';

            ++path;

            // stop after root separator:
            // "/" or "C:/"
            if (currLength == 1 && outBuffer[0] == ASTL_FILE_SEPERATOR)
                break;

            if (currLength == 3 && outBuffer[1] == ':' && outBuffer[2] == ASTL_FILE_SEPERATOR)
                break;
        }
    }
    else
    {
        currLength = GetCurrentDirectory(bufferSize, outBuffer);
        if (currLength <= 0 || currLength >= bufferSize)
        {
            outBuffer[0] = '\0';
            return;
        }

        for (int i = 0; i < currLength; ++i)
        {
            if (IsPathSeparator(outBuffer[i]))
                outBuffer[i] = ASTL_FILE_SEPERATOR;
        }
    }

    while (*path)
    {
        while (IsPathSeparator(*path))
            ++path;

        if (!*path)
            break;

        if (path[0] == '.' && (path[1] == '\0' || IsPathSeparator(path[1])))
        {
            ++path;
            continue;
        }

        if (path[0] == '.' && path[1] == '.' && (path[2] == '\0' || IsPathSeparator(path[2])))
        {
            PathGoBackOneFolder(outBuffer, &currLength);
            path += 2;
            continue;
        }

        if (!PushSeparator(outBuffer, bufferSize, &currLength))
            return;

        while (*path && !IsPathSeparator(*path))
        {
            if (!PushChar(outBuffer, bufferSize, &currLength, *path))
                return;

            ++path;
        }
    }

    outBuffer[currLength] = '\0';
}

void NormalizePath(const char* path, char* out, u32 outSize)
{
    u32 i = 0u;
    for (; path[i] && i + 1u < outSize; i++)
        out[i] = path[i] == '\\' ? '/' : path[i];
    out[i] = '\0';
}

void ChangeExtensionAndCopy(const char* path, const char* extension, char* out, u32 outSize)
{
    MemsetZero(out, outSize);
    MemCopy(out, path, StringLength(path));
    ChangeExtension(out, outSize, extension);
}

bool CombinePaths(char* dst, uint64_t dstSize, const char* a, const char* b)
{
    int aLen = StringLength(a);
    int bLen = StringLength(b);
    if (aLen + bLen + 1 > dstSize)
    {
        AX_WARN("Combine path len is too long! capacity: %llu, requested:%d, %s %s", dstSize, aLen + bLen, a, b);
        return false;
    }
    SmallMemCpy(dst, a, aLen);
    dst[aLen] = ASTL_FILE_SEPERATOR;
    SmallMemCpy(dst + aLen + 1, b, bLen);
    dst[aLen + 1 + bLen] = 0;
    return true;
}

// this is an function that can traverse your all computer files! very fast
// returns 0: reading failed, 1: success, 2: not enough buffer
int VisitFolder(const char* root, FolderVisitFn visitFn, void* data, bool recurse)
{
    static AX_THREAD_LOCAL int16_t stack[1024 * SIMD_NUM_BYTES * 2]; // 128kb
    char* buffer = (char*)ArenaPushGlobal(0);
    char combined[MAX_PATH];
    int sp = 0;
    int bufPos = 0;

    MemSet(buffer, 0, Minu64(ArenaRemainingCurrent(), (u64)(MAX_PATH)));
    MemsetZero(combined, sizeof(combined));

    int rootLen = (int)StringLength(root);
    if (rootLen <= 0 || rootLen + 1 > ArenaRemainingCurrent())
        return 0;

    MemCopy(buffer + bufPos, root, rootLen);
    buffer[bufPos + rootLen] = '\0';

    stack[sp++] = bufPos;
    bufPos += rootLen + 1;

    while (sp > 0)
    {
        int offset = stack[--sp];
        const char* path = buffer + offset;

        DIR* dir = opendir(path);
        if (!dir)
        {
            AX_WARN("VisitFolder file couldn't open! %s\n", path);
            continue;
        }

        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL)
        {
            const char* name = ent->d_name;
            if (name[0] == '.')
                continue;

            if (!CombinePaths(combined, sizeof(combined), path, name))
            {
                continue;
            }

            visitFn(combined, data);

            if (recurse && IsFolder(combined))
            {
                int len = (int)StringLengthSafe(combined, sizeof(combined) - 1);
                if (len <= 0)
                {
                    AX_WARN("VisitFolder skipping folder! %s\n", path);
                    continue;
                }

                len += 1;

                if (sp < (int)ARRAY_SIZE(stack) && (bufPos + len) <= ArenaRemainingCurrent())
                {
                    MemCopy(buffer + bufPos, combined, len);
                    stack[sp++] = bufPos;
                    bufPos += len;
                }
                else
                {
                    AX_WARN("VisitFolder stack size is not enough! %s\n", path);
                    return 2;
                }
            }
        }

        closedir(dir);
    }

    return 1;
}


bool HasAnySubdir(const char* path)
{
    DIR* dir;
    struct dirent* ent;
    if ((dir = opendir(path)) != NULL) 
    {
        char combined[1024];
        int pathLen = StringLengthSafe(path, sizeof(combined));
        while (pathLen !=-1 && (ent = readdir(dir)) != NULL)  
        {
            MemsetZero(combined, sizeof(combined));
            ASSERT(pathLen + StringLengthSafe(ent->d_name, sizeof(combined)) < sizeof(combined));
            bool combineSuccess = CombinePaths(combined, sizeof(combined), path, (const char*)ent->d_name);
            if (combineSuccess && ent->d_name[0] != '.' && IsFolder((const char*)combined)) return true;
        }
        closedir(dir);
    }
    return false;
}

void EnsurePath(const char* path)
{
    char temp[1024];
    int lastSeparator = -1;
    for (int i = 0; path[i] != '\0' && i < 1023; ++i)
    {
        if (path[i] == '/' || path[i] == '\\') lastSeparator = i;
        temp[i] = path[i];
    }

    if (lastSeparator <= 0) return;

    for (int i = 0; i <= lastSeparator; ++i)
    {
        if (temp[i] == '/' || temp[i] == '\\')
        {
            char originalChar = temp[i];
            temp[i] = '\0'; // Temporarily terminate to check this specific folder
            
            if (!IsFolder(temp))
                CreateFolder(temp);
            
            temp[i] = originalChar; // Restore separator
        }
    }
}

void RemoveFolder(const char* path, void* unused) 
{
    (void)unused;
    VisitFolder(path, RemoveFolder, NULL, true); // recursive delete all subfolders and files
    if (IsFolder(path)) {
        _rmdir(path);
    }
    else {
        RemoveFile(path);
    }
}

void WriteFloat4(v128f v, AFile file0)
{
    char buff[128];
    int sizeLen = FloatToString(buff, VecGetX(v), 4);
    buff[sizeLen] = '\n';
    AFileWrite(buff, sizeLen+1, file0, 1);

    sizeLen = FloatToString(buff, VecGetY(v), 4);
    buff[sizeLen] = '\n';
    AFileWrite(buff, sizeLen+1, file0, 1);

    sizeLen = FloatToString(buff, VecGetZ(v), 4);
    buff[sizeLen] = '\n';
    AFileWrite(buff, sizeLen+1, file0, 1);

    sizeLen = FloatToString(buff, VecGetW(v), 4);
    buff[sizeLen] = '\n';
    buff[sizeLen+1] = '\n';
    AFileWrite(buff, sizeLen+2, file0, 1);
}
