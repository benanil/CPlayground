<p align="center">
  <img src="Assets/Icons/CLogo.png" alt="CEngine logo" width="160">
</p>

## Overview
`CEngine` is a data-oriented GPU driven C99 engine with a small amount of C++ used for basis texture compression. It builds an SDL3 GPU-based executable and now contains much more than a playground: a scene system, forward+ renderer, asset pipeline, editor UI, animation, and streamed voxel terrain with lod.

## Features
- Scene and asset pipeline with glTF/glb/fbx/obj import
- GPU driven rendering
- BVH data structure over the world, (ray casting + picking support)
- Box3d physics
- Terrain + Foliage system
- Texture system with 4k texture2d atlass array's instead of bindless textures
- All objects in a world can be rendered with single draw call
- Clay layout with SDF UI shapes such as rounded rectangle circle
- Slug text rendering high quality with all alphabets/languages supported without texture atlasses. Thanks Eric Lengyel
- Editor tooling with dockable UI, scene editing, asset browsing.
- Streamed voxel terrain with procedural generation, sculpt/paint editing, and persisted terrain edits.
- Full compute + raster animation pipeline.
![Engine preview](Assets/Images/Untitled.png)
## Graphics Features
- Forward+ lighting.
- GPU compute draw call recording with HI-Z oclussion culling + frustum culling + lod selection
- Directional cascaded + point and spot light shadows.
- Animation compute and animated-vertex generation.
- HBAO, MLAA, MSAA, tonemapping, god rays, procedural sky, height fog, Screen Space Shadows.
- Precompiled shader outputs for SPIR-V and Metal (`spv/`, `msl/`).

## Project Layout
- `Source/Rendering/`: renderer, pipelines, compute passes, shadows, draw submission.
- `Source/AssetManagement/`: glTF/FBX import, mesh baking, texture processing, asset caching.
- `Source/Editor/`: editor windows, scene tools, terrain tools, asset browser, console.
- `Source/Terrain/`: streamed voxel terrain, marching cubes meshing, edit persistence.
- `Source/UI/`: custom UI renderer/windowing/text integration.
- `Include/`: public engine headers.
- `Math/`: math types, matrices, vectors, colors, quaternions, SIMD helpers.
- `Extern/`: bundled third-party dependencies.

## External Libraries

| Library | Purpose |
| --- | --- |
| SDL3  | Platform + GPU backend |
| Box3D | Physics | 
| basis_universal | Texture compression |
| ufbx | FBX import |
| meshoptimizer  | Mesh optimization |
| clay  | Editor UI layout  |
| kb_text_shape | Text shaping |
| smol-atlas  | ![Atlas packing](https://github.com/aras-p/smol-atlas)|
| stb_image | Image loading | tools/scripts |
| stb_image_resize2 | Image resizing | tools/scripts |
| stb_truetype  | Font parsing |
| stb_image_write | Image writing | tools/scripts |
| tlsf | Allocator |
| dynarray  | Dynamic arrays |
| sj | glTF JSON parsing  |
| sdefl / sinfl  | Compression  |

## Prerequisites
Install a C/C++ toolchain and CMake 3.16+.

### Windows
- Visual Studio (MSVC) or LLVM/Clang
- Optional: Ninja for faster builds

### macOS
- Xcode Command Line Tools
- CMake

### Linux
- GCC or Clang
- CMake

## Build Notes
- SDL3 is bundled in `Extern/SDL3`, so no separate SDL install is normally required.
- OpenMP is detected optionally by CMake and enabled when found.
- The project builds a single executable target: `CEngine`.
- Shader helper edits may require a forced rebuild, especially shared shader helper headers.

## How to Compile
python 3.9 and above
```bash
python Build/Compile.py
```

### Basic build
If you do this methods below you might get shader error because shaders are not compiled so use this command below to compile shaders
```bash
python Shaders/CompileShaders.py
```
```bash
cmake -S . -B build
cmake --build build
```

### Windows (Ninja)
```bash
winget install Ninja-build.Ninja
cmake -S . -B Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build Debug
cmake -S . -B Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Release
```
