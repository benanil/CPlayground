cmake_minimum_required(VERSION 3.16)

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

# prevent installing to system directories. 
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE INTERNAL "")

set(CMAKE_MSVC_PARALLEL_COMPILE OFF)

# Declare the project
project(CEngine)

if ((APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "Darwin") OR EMSCRIPTEN)
    set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")    # Disable shared builds on platforms where it does not make sense to use them
    set(SDL_SHARED OFF)
else()
    set(SDL_SHARED ON)
endif()

# if(MSVC)
#     if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
#         add_definitions(/MP)				# parallelize each target, unless Ninja is the generator
#     endif()
# endif()

# Set the name of the executable
set(EXECUTABLE_NAME ${PROJECT_NAME})

include_directories(${CMAKE_SOURCE_DIR})

add_executable(${EXECUTABLE_NAME})

find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

set(BASIS_ENCODER_SOURCES
    Extern/basis_universal/encoder/basisu_enc.cpp
    Extern/basis_universal/encoder/basisu_comp.cpp
    Extern/basis_universal/encoder/basisu_frontend.cpp
    Extern/basis_universal/encoder/basisu_backend.cpp
    Extern/basis_universal/encoder/basisu_basis_file.cpp
    Extern/basis_universal/encoder/basisu_etc.cpp
    Extern/basis_universal/encoder/basisu_gpu_texture.cpp
    Extern/basis_universal/encoder/basisu_pvrtc1_4.cpp
    Extern/basis_universal/encoder/basisu_resampler.cpp
    Extern/basis_universal/encoder/basisu_resample_filters.cpp
    Extern/basis_universal/encoder/basisu_ssim.cpp
    Extern/basis_universal/encoder/basisu_uastc_enc.cpp
    Extern/basis_universal/encoder/basisu_astc_ldr_common.cpp
    Extern/basis_universal/encoder/basisu_astc_ldr_encode.cpp
    Extern/basis_universal/encoder/basisu_astc_hdr_common.cpp
    Extern/basis_universal/encoder/basisu_astc_hdr_6x6_enc.cpp
    Extern/basis_universal/encoder/basisu_uastc_hdr_4x4_enc.cpp
    Extern/basis_universal/encoder/basisu_bc7enc.cpp
    Extern/basis_universal/encoder/basisu_kernels_sse.cpp
    Extern/basis_universal/encoder/basisu_tinyexr.cpp
    Extern/basis_universal/encoder/jpgd.cpp
    Extern/basis_universal/encoder/pvpngreader.cpp
    Extern/basis_universal/encoder/basisu_opencl.cpp
    Source/BasisCompressWrapper.cpp
    Extern/basis_universal/transcoder/basisu_transcoder.cpp
    Extern/basis_universal/encoder/3rdparty/android_astc_decomp.cpp
    Extern/ufbx.c
    # Extern/basis_universal/encoder/3rdparty/exr_reader.hh
    # Extern/basis_universal/encoder/3rdparty/qoi.h
    # Extern/basis_universal/encoder/3rdparty/streamreader.hh
    # Extern/basis_universal/encoder/3rdparty/tinydds.h
    # Extern/basis_universal/encoder/3rdparty/tinyexr.h
)

foreach(src ${BASIS_ENCODER_SOURCES})
    set_source_files_properties(${src} PROPERTIES
        COMPILE_DEFINITIONS "NOMINMAX;BASISU_SUPPORT_ZSTD=0;BASISD_SUPPORT_KTX2_ZSTD=0"
    )
endforeach()

if(MSVC)
    foreach(src ${BASIS_ENCODER_SOURCES})
        set_source_files_properties(${src} PROPERTIES
            COMPILE_OPTIONS "/wd4244;/wd4267;/wd4018;/wd4005"
        )
    endforeach()
endif()

set_source_files_properties(
    Extern/basis_universal/transcoder/basisu_transcoder.cpp
    PROPERTIES COMPILE_DEFINITIONS
    "NOMINMAX;BASISD_SUPPORT_PVRTC2=0;BASISD_SUPPORT_FXT1=0;BASISD_SUPPORT_ATC=0;BASISD_SUPPORT_KTX2_ZSTD=0;BASISU_SUPPORT_ZSTD=0"
)

target_sources(${EXECUTABLE_NAME} PRIVATE
    Source/Main.c
    Source/DataStructures/Array.c
    Extern/tlsf.c
    Extern/ExternAll.cpp

    Source/Animation.c
    Source/Algorithm.c
    Source/Async.c
    Source/JobSystem.c
    Source/ParallelFor.c
    Source/Memory.c
    Source/FileSystem.c
    Source/Platform.c
    Source/Editor/Editor.c
    Source/AnimationBake.c
    Source/BVH.c
    Source/Scene.c
    Source/SceneBundleCache.c
    Source/SceneSerializer.c
    Source/DemoScene.c
    Source/Physics.c

    Source/Terrain/Terrain.c
    Source/Terrain/TerrainMaterial.c
    Source/Terrain/TerrainFoliage.c
    Source/Terrain/TerrainDensity.c
    Source/Terrain/TerrainEdit.c
    Source/Terrain/TransvoxelMesh.c
    Source/Terrain/TransvoxelMesher.c
    Source/Terrain/Transvoxel.c

    Source/Editor/AssetsEditor.c
    Source/Editor/SceneEditor.c
    Source/Editor/TerrainEditor.c
    Source/Editor/ConsoleEditor.c
    Source/Editor/EditorGizmo.c

    Source/Rendering/Graphics.c
    Source/Rendering/Rendering.c
    Source/Rendering/RenderSet.c
    Source/Rendering/RenderingPipelines.c
    Source/Rendering/RenderingCompute.c
    Source/Rendering/RenderingDraw.c
    Source/Rendering/Shadow.c

    Source/UI/KBTextShape.c
    Source/UI/Slug.c
    Source/UI/UIRenderer.c
    Source/UI/UIText.c
    Source/UI/UIWindow.c

    Source/AssetManagement/GLTFParser.c
    Source/AssetManagement/SceneNormalize.c
    Source/AssetManagement/MeshBake.c
    Source/AssetManagement/AssetManager.c
    Source/AssetManagement/BasisBinding.cpp
    Source/AssetManagement/TextureSystem.c
    
    ${BASIS_ENCODER_SOURCES}
)

if(WIN32)
    target_sources(${EXECUTABLE_NAME} PRIVATE Source/Windows/info.rc)
endif()

set_property(TARGET ${EXECUTABLE_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${EXECUTABLE_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

# if(WIN32)
#     target_compile_definitions(${EXECUTABLE_NAME} PRIVATE SDL_GPU_D3D12=1)
#     
#     set(SDL_GPU_DXGI ON CACHE BOOL "" FORCE)
#     set(SDL_GPU_D3D12 ON CACHE BOOL "" FORCE)
#     
#     target_link_libraries(${EXECUTABLE_NAME} PUBLIC d3d12 dxgi dxguid)
# endif()

# Configure SDL by calling its CMake file.
# we use EXCLUDE_FROM_ALL so that its install targets and configs don't
# pollute upwards into our configuration.
add_subdirectory(Extern/SDL3 EXCLUDE_FROM_ALL)

target_include_directories(${EXECUTABLE_NAME} PRIVATE
    Extern/basis_universal
    Extern/basis_universal/encoder
    Extern/basis_universal/transcoder
)

target_compile_options(${EXECUTABLE_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:/arch:AVX2>
    $<$<C_COMPILER_ID:Clang>:/arch:AVX2>
    #$<$<CONFIG:Release>:/fsanitize=address>
)

#target_link_options(${EXECUTABLE_NAME} PRIVATE $<$<CONFIG:Release>:/fsanitize=address>)

target_compile_definitions(${EXECUTABLE_NAME} PRIVATE
    BASISU_NO_ITERATOR_DEBUG_LEVEL
)

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")
    list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_LINK_LIBRARIES stdc++)
    # Prevent CMake from implicitly adding standard library link arguments
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES 
        LINK_DEPENDS_NO_SHARED TRUE
    )
endif()

# Link SDL to our executable. This also makes its include directory available to us. 
if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME} PUBLIC Dbghelp) # crash handler stack walk
    target_link_options(${EXECUTABLE_NAME} PRIVATE /MAP)     # rva -> symbol mapping for release crashes
endif()

target_link_libraries(${EXECUTABLE_NAME} PUBLIC SDL3::SDL3) # If using satelite libraries, SDL must be the last item in the list.

add_subdirectory(Extern/box3d)

target_link_libraries(${EXECUTABLE_NAME} PRIVATE box3d::box3d)

set_property(TARGET ${EXECUTABLE_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
# on Visual Studio, set our app as the default project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "${EXECUTABLE_NAME}")
