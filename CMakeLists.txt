cmake_minimum_required(VERSION 3.20)
project(Main C)

# Force MSVC compiler flags
if (MSVC)
    add_compile_options(
        /O2
        /arch:AVX2
        /GR-   # no RTTI
        /GS-   # disable security checks
        /EHsc- # disable exceptions
    )
endif()

# Add your source
add_executable(MainMSVC Main.c)

# Output to build/ instead of default
set_target_properties(MainMSVC PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build"
)

# Link with Windows system libs
target_link_libraries(MainMSVC PRIVATE
    advapi32
    d3d11
    dxgi
    dxguid
    user32
    shell32
    gdi32
    winmm
)