@echo off
rem Builds Hollow Engine with the Visual Studio 2022 C++ toolchain.
rem
rem   build.bat                 RelWithDebInfo build (optimized, with symbols)
rem   build.bat Debug           unoptimized debug build
rem   build.bat Release test    build, then run the engine tests
rem
rem Output: build\<Config>\bin\ (HollowEngine.exe, HollowRuntime.dll, Game.dll, HollowTests.exe)
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"

rem Locate Visual Studio (any edition) through vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Could not find vswhere.exe. Install Visual Studio 2022 with "Desktop development with C++".
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo No Visual Studio installation with the C++ x64 tools was found.
    exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
if errorlevel 1 exit /b %errorlevel%

cmake -S engine -B "build\%CONFIG%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 exit /b %errorlevel%

cmake --build "build\%CONFIG%"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built build\%CONFIG%\bin\HollowEngine.exe (with HollowRuntime.dll and Game.dll)

if /i not "%~2"=="test" exit /b 0
pushd "build\%CONFIG%"
ctest --output-on-failure
set "TEST_RESULT=%errorlevel%"
popd
exit /b %TEST_RESULT%
