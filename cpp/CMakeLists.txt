cmake_minimum_required(VERSION 3.24)

project(ForgeEnginePrototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ForgeEngine
    src/App.cpp
    src/Engine.cpp
    src/Importer.cpp
    src/Main.cpp
    src/Math.cpp
    src/Renderer.cpp
    src/Scene.cpp
    src/Win32Window.cpp
)

target_include_directories(ForgeEngine PRIVATE include)

if (MSVC)
    target_compile_options(ForgeEngine PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(ForgeEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(ForgeEngine PRIVATE OpenGL::GL)

if (WIN32)
    target_link_libraries(ForgeEngine PRIVATE shell32)
endif()

if (WIN32)
    target_compile_definitions(ForgeEngine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
