cmake_minimum_required(VERSION 3.24)

project(HollowEngine VERSION 0.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT WIN32)
    message(FATAL_ERROR "Hollow Engine currently targets Windows only (Win32 + WGL + WIC).")
endif()

# Every executable and DLL lands in one folder (build/<config>/bin) so the
# editor finds HollowRuntime.dll and Game.dll next to itself.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
foreach (config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# Shared CRT everywhere: objects allocated in one module are freed in another.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(HOLLOW_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

function(hollow_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /MP)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# --- Third party ---------------------------------------------------------------
# glad: OpenGL 4.5 core + WGL loader.
set(GLAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad)
add_library(glad STATIC ${GLAD_DIR}/src/gl.c ${GLAD_DIR}/src/wgl.c)
target_include_directories(glad PUBLIC ${GLAD_DIR}/include)

# Dear ImGui: Win32 + OpenGL3 backends.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp)
target_compile_definitions(imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Jolt Physics (vendored v5.3.0). Only the runtime links it; no Jolt header
# appears in the engine's public headers. Assert/debug configuration follows
# NDEBUG, the same as the code that includes it.
set(JOLT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/JoltPhysics)
file(GLOB_RECURSE JOLT_SOURCES CONFIGURE_DEPENDS ${JOLT_DIR}/Jolt/*.cpp)
add_library(Jolt STATIC ${JOLT_SOURCES})
target_include_directories(Jolt PUBLIC ${JOLT_DIR})
if (MSVC)
    target_compile_options(Jolt PRIVATE /MP /W0)
endif()

# --- HollowRuntime (engine runtime DLL) --------------------------------------------
# Everything a game needs at runtime: core, assets, scene, physics,
# scripting (and the plain-data render settings / camera saved in scenes).
# Shared so the editor, game modules and tests all use one copy of the
# engine's state (log, script registry, Jolt). Every function is exported.
set(HOLLOW_RUNTIME_DIRS Core Assets Scene Physics Scripting)
set(HOLLOW_RUNTIME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Render/Camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Render/RenderSettings.cpp)
foreach (dir ${HOLLOW_RUNTIME_DIRS})
    file(GLOB_RECURSE dir_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${dir}/*.cpp)
    list(APPEND HOLLOW_RUNTIME_SOURCES ${dir_sources})
endforeach()
file(GLOB_RECURSE HOLLOW_PUBLIC_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp)

add_library(HollowRuntime SHARED ${HOLLOW_RUNTIME_SOURCES} ${HOLLOW_PUBLIC_HEADERS})
set_target_properties(HollowRuntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON PREFIX "")
target_include_directories(HollowRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(HollowRuntime PUBLIC ${HOLLOW_DEFINES})
target_link_libraries(HollowRuntime PRIVATE Jolt windowscodecs ole32 shell32)
hollow_warnings(HollowRuntime)

# --- HollowEngine (editor executable) ---------------------------------------------
file(GLOB_RECURSE HOLLOW_EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Render/*.cpp)
list(REMOVE_ITEM HOLLOW_EDITOR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Render/Camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Render/RenderSettings.cpp)
file(GLOB_RECURSE HOLLOW_PRIVATE_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
# Application icon (assets/branding/hollow.ico) via a Windows resource script.
set(HOLLOW_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/HollowEngine.rc)

add_executable(HollowEngine ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp ${HOLLOW_EDITOR_SOURCES} ${HOLLOW_PRIVATE_HEADERS} ${HOLLOW_RESOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${HOLLOW_RUNTIME_SOURCES} ${HOLLOW_EDITOR_SOURCES} ${HOLLOW_PUBLIC_HEADERS} ${HOLLOW_PRIVATE_HEADERS})
target_include_directories(HollowEngine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Shaders load from the source tree first (editing a shader hot-reloads it
# while the engine runs), then from shaders/ next to the executable.
target_compile_definitions(HollowEngine PRIVATE HOLLOW_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
find_package(OpenGL REQUIRED)
target_link_libraries(HollowEngine PRIVATE HollowRuntime OpenGL::GL glad imgui shell32 windowscodecs ole32 comdlg32 dwmapi imm32)
hollow_warnings(HollowEngine)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HollowEngine)

# Runtime assets and shaders next to the executable, so they resolve from a
# stable path however the engine is launched (executable_directory()).
add_custom_command(TARGET HollowEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:HollowEngine>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:HollowEngine>/shaders
)

# --- HollowGame (Game.dll: gameplay scripts, hot-reloaded by the editor) ------------
# Rebuild this target while the editor runs to reload your scripts.
file(GLOB_RECURSE HOLLOW_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/game/*.hpp)
add_library(HollowGame SHARED ${HOLLOW_GAME_SOURCES})
set_target_properties(HollowGame PROPERTIES OUTPUT_NAME Game PREFIX "")
target_link_libraries(HollowGame PRIVATE HollowRuntime)
hollow_warnings(HollowGame)
if (MSVC)
    # A fresh PDB name per link, so a debugger holding the old PDB never
    # blocks a rebuild during hot reload.
    target_link_options(HollowGame PRIVATE "/PDBALTPATH:Game.pdb")
endif()
add_dependencies(HollowEngine HollowGame)

# --- Tests ---------------------------------------------------------------------------
# Logic tests for the runtime (no window/GPU): math, JSON, hierarchy,
# serialization, migration, physics, scripting, animation, import. `ctest`.
option(HOLLOW_BUILD_TESTS "Build the HollowTests executable" ON)
if (HOLLOW_BUILD_TESTS)
    enable_testing()
    add_executable(HollowTests tests/EngineTests.cpp)
    target_link_libraries(HollowTests PRIVATE HollowRuntime)
    target_compile_definitions(HollowTests PRIVATE HOLLOW_TEST_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets/showcase")
    hollow_warnings(HollowTests)
    add_test(NAME HollowTests COMMAND HollowTests WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
