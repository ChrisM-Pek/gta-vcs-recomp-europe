# The shader compilers the macOS build runs (docs/MACOS.md): glslang (HLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> MSL),
# built from pinned sources into build/build-tools/shader-tools by tools/shader_tools.py - nothing to install. The pins
# follow the Vulkan headers' (src/rhi). tools/deps.py bundles these sources for offline builds (deps/).
cmake_minimum_required(VERSION 3.21)
project(vcs_shader_tools LANGUAGES C CXX)

include(FetchContent)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)            # no SPIRV-Tools optimiser (and no SPIRV-Tools download)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glslang
    GIT_REPOSITORY "https://github.com/KhronosGroup/glslang.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
)

set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spirv_cross
    GIT_REPOSITORY "https://github.com/KhronosGroup/SPIRV-Cross.git"
    GIT_TAG "vulkan-sdk-1.4.350.1"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glslang spirv_cross)

# both programs next to each other at the top of the build folder
set_target_properties(glslang-standalone spirv-cross PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_custom_target(vcs_shader_tools ALL DEPENDS glslang-standalone spirv-cross)
