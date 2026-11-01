# The launcher (docs/LAUNCHER.md): finds and checks the disc, installs what the build needs, runs tools/build.py and
# starts the game. A project of its own - no generated code, no disc, no third-party code - so CI builds it in seconds
# for a release:
#   python tools/build.py --launcher        (build/launcher, VCSLauncher.exe in the repository root)
# Its own window, renderer and UI (uppies' design): Win32, Direct3D 11, DirectWrite. Layers, each using only those
# below it: screens/ -> ui/ -> render/ -> platform/ -> core/; model/ is the state and the tools it runs.
cmake_minimum_required(VERSION 3.21)
project(vcs_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# the shader, compiled at build time into headers (no runtime compiler, shader errors are build errors)
file(GLOB VCS_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT VCS_FXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
find_program(VCS_FXC fxc HINTS ${VCS_FXC_CANDIDATES})
if(NOT VCS_FXC AND VCS_FXC_CANDIDATES)
    list(GET VCS_FXC_CANDIDATES 0 VCS_FXC)
endif()
if(NOT VCS_FXC)
    message(FATAL_ERROR "fxc.exe (Windows SDK) not found: install the Windows 10/11 SDK")
endif()
set(VCS_LAUNCHER_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VCS_LAUNCHER_SHADERS}")
set(VCS_UI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/render/d3d11/ui.hlsl")
foreach(stage vs ps)
    add_custom_command(
        OUTPUT "${VCS_LAUNCHER_SHADERS}/ui_${stage}.h"
        COMMAND "${VCS_FXC}" /nologo /O3 /T ${stage}_4_0 /E ${stage}_main /Vn g_ui_${stage} /Fh "${VCS_LAUNCHER_SHADERS}/ui_${stage}.h" "${VCS_UI_HLSL}"
        DEPENDS "${VCS_UI_HLSL}"
        COMMENT "fxc ui.hlsl ${stage}_main"
        VERBATIM)
endforeach()

add_executable(VCSLauncher WIN32
    main.cpp
    core/png.cpp
    platform/platform_win32.cpp
    render/draw.cpp
    render/font.cpp
    render/font_dwrite.cpp
    render/d3d11/renderer_d3d11.cpp
    ui/ui.cpp
    ui/widgets.cpp
    screens/screens.cpp
    screens/backdrop.cpp
    screens/titlebar.cpp
    screens/setup.cpp
    screens/home.cpp
    screens/task.cpp
    screens/sheets.cpp
    screens/overlays.cpp
    screens/gallery.cpp
    model/tasks.cpp
    model/process.cpp
    model/disc_check.cpp
    model/system.cpp
    model/json.cpp
    "${VCS_ROOT}/src/disc/iso_image.cpp"
    "${VCS_LAUNCHER_SHADERS}/ui_vs.h"
    "${VCS_LAUNCHER_SHADERS}/ui_ps.h"
)
# the version (VERSION in the repository root, the one number of a release) in the exe's version information
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS_ROOT}/VERSION")
file(STRINGS "${VCS_ROOT}/VERSION" VCS_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _vcs_version_numbers "${VCS_VERSION}")
if(NOT _vcs_version_numbers)
    message(FATAL_ERROR "VERSION must start with major.minor.patch (it holds '${VCS_VERSION}')")
endif()
target_compile_definitions(VCSLauncher PRIVATE VCS_VERSION_MAJOR=${CMAKE_MATCH_1} VCS_VERSION_MINOR=${CMAKE_MATCH_2}
                           VCS_VERSION_PATCH=${CMAKE_MATCH_3} "VCS_VERSION_TEXT=\"${VCS_VERSION}\"")

target_sources(VCSLauncher PRIVATE launcher.rc)
# the resource compiler doesn't report the files it embeds: a redrawn icon would otherwise not rebuild
set_source_files_properties(launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/launcher.ico")
target_link_libraries(VCSLauncher PRIVATE d3d11 dxgi dwrite dwmapi ole32 shell32 uuid winmm advapi32)
target_compile_definitions(VCSLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(VCSLauncher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${VCS_ROOT}/src" "${VCS_LAUNCHER_SHADERS}")
# next to tools/ and the source, where a release has it (the launcher finds the repository from its own folder)
set_target_properties(VCSLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_ROOT}")
if(MSVC)
    # one self-contained exe: the static C++ runtime, nothing to install
    set_property(TARGET VCSLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(VCSLauncher PRIVATE /W3 /utf-8)
endif()
