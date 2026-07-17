#include "Include/RenderSet.h"
#include "Include/FileSystem.h"
#include "Include/Platform.h"
#include "Include/Graphics.h"
#include "Include/JobSystem.h"
#include "Include/TextureSystem.h"
#include "Include/Memory.h"
#include "Include/Scene.h"
#include <SDL3/SDL_atomic.h>

#define T_MAX_FOLIAGE_SCENE 64
#define T_FOLIAGE_MAX_PATH 512

typedef struct tFoliageState_
{
	Scene scene;
	u32   numPaths;
	char  paths[T_MAX_FOLIAGE_SCENE][T_FOLIAGE_MAX_PATH];
} tFoliageState;

tFoliageState gFoliage;

static void LoadFoliage(void* data)
{

}

static char* StringDuplcate(const char* str)
{
	int len = StringLength(str);
	char* res = (char*)AllocateTLSFGlobal(len + 1);
	MemCopy(res, str, len);
	return res;
}

static void VisitFile(const char* path, void* data)
{
	int pathLen = StringLength(path);
	u8 isMesh = FileHasExtension(path, pathLen, ".fbx") || FileHasExtension(path, pathLen, ".gltf") ||  
		        FileHasExtension(path, pathLen, ".obj") || FileHasExtension(path, pathLen, ".glb");
	if (isMesh)
	{
		if (gFoliage.numPaths >= T_MAX_FOLIAGE_SCENE || pathLen <= 0 || pathLen >= T_FOLIAGE_MAX_PATH) {
			AX_WARN("foliage path invalid");
			return;
		}

		NormalizePath(path, gFoliage.paths[gFoliage.numPaths], T_FOLIAGE_MAX_PATH);
		gFoliage.numPaths++;
	}
}

void tFoliage_Init()
{
	MemSet(&gFoliage, 0, sizeof(tFoliageState));
	Scene_Init(&gFoliage.scene);
	if (VisitFolder("Assets/Foliage", VisitFile, NULL, true) == 0) {
		AX_WARN("foliage file traverse failed!");
		return;
	}

	for (u32 i = 0; i < gFoliage.numPaths; i++)
	{
		AX_LOG("path: %s", gFoliage.paths[i]);
		Scene_AddBundle(&gFoliage.scene, gFoliage.paths[i], false);
	}
}
