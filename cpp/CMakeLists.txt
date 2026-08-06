cmake_minimum_required(VERSION 3.24)

project(ForgeEnginePrototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/cpp
)

if (WIN32)
    target_compile_definitions(imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_executable(ForgeEngine
    src/App.cpp
    src/EditorUi.cpp
    src/Engine.cpp
    src/Importer.cpp
    src/Json.cpp
    src/Main.cpp
    src/Math.cpp
    src/Renderer.cpp
    src/Scene.cpp
    src/TextureLoader.cpp
    src/Win32Window.cpp
)

target_include_directories(ForgeEngine PRIVATE include)

if (MSVC)
    target_compile_options(ForgeEngine PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(ForgeEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(ForgeEngine PRIVATE OpenGL::GL imgui)

if (WIN32)
    target_link_libraries(ForgeEngine PRIVATE shell32 windowscodecs ole32)
endif()

if (WIN32)
    target_compile_definitions(ForgeEngine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
