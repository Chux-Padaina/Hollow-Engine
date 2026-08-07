@echo off
setlocal

set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VS_DEV_CMD%" (
    echo Visual Studio developer command script was not found:
    echo %VS_DEV_CMD%
    exit /b 1
)

call "%VS_DEV_CMD%" -arch=x64
if errorlevel 1 exit /b %errorlevel%

cmake -S cpp -B cpp\build-msvc -G "NMake Makefiles"
if errorlevel 1 exit /b %errorlevel%

cmake --build cpp\build-msvc --config Debug
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built cpp\build-msvc\HollowEngine.exe
