@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
if errorlevel 1 goto :folder_error

echo.
echo ============================================================
echo  NightLight - Local Windows x64 Release build
echo ============================================================
echo.
echo Source directory: "%CD%"
echo.

rem Use the installed Visual Studio C++ toolchain, even when launched
rem by double-clicking this BAT rather than from a Developer Prompt.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_ROOT="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%V"
)
if defined VS_ROOT (
    if exist "%VS_ROOT%\Common7\Tools\VsDevCmd.bat" (
        echo Setting up Visual Studio x64 tools...
        call "%VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
        if errorlevel 1 goto :tools_error
    )
)

where cl.exe >nul 2>&1
if errorlevel 1 goto :compiler_missing
where rc.exe >nul 2>&1
if errorlevel 1 goto :sdk_missing
where cmake.exe >nul 2>&1
if errorlevel 1 goto :cmake_missing

rem Ninja is optional. NMake is available with the Visual C++ tools.
set "GENERATOR=NMake Makefiles"
set "BUILD_DIR=build-local-nmake"
where ninja.exe >nul 2>&1
if not errorlevel 1 (
    set "GENERATOR=Ninja"
    set "BUILD_DIR=build-local-ninja"
)

echo.
echo Generator: %GENERATOR%
echo Build directory: %BUILD_DIR%
echo.
echo [1/4] Configure (Release, native tests enabled)...
cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
if errorlevel 1 goto :build_failed

echo.
echo [2/4] Compile NightLight.exe and native tests...
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto :build_failed

echo.
echo [3/4] Run regression tests...
ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 goto :tests_failed

echo.
echo [4/4] Copy the portable EXE to dist...
if not exist "dist" mkdir "dist"
if errorlevel 1 goto :build_failed
if not exist "%BUILD_DIR%\NightLight.exe" goto :build_failed
copy /Y "%BUILD_DIR%\NightLight.exe" "dist\NightLight.exe" >nul
if errorlevel 1 goto :build_failed

echo.
echo ============================================================
echo  SUCCESS
echo  Portable executable: "%CD%\dist\NightLight.exe"
echo  Nothing has been built or uploaded on GitHub.
echo ============================================================
echo.
pause
exit /b 0

:folder_error
echo ERROR: Unable to access the source directory.
goto :failed

:tools_error
echo ERROR: Visual Studio developer environment failed to initialize.
goto :failed

:compiler_missing
echo ERROR: Microsoft C++ compiler cl.exe not found.
echo Install Visual Studio 2022/2026 or Build Tools with:
echo   - Desktop development with C++
echo   - MSVC x64/x86 build tools
echo   - Windows 10/11 SDK
echo Or run this BAT from an x64 Native Tools Command Prompt.
goto :failed

:sdk_missing
echo ERROR: Windows SDK resource compiler rc.exe not found.
echo Install the Windows 10/11 SDK from Visual Studio Installer.
goto :failed

:cmake_missing
echo ERROR: CMake 3.23 or newer not found.
echo Install CMake and add it to PATH, or install its Visual Studio component.
goto :failed

:tests_failed
echo ERROR: Native regression tests failed. No EXE was copied to dist.
goto :failed

:build_failed
echo ERROR: Configuration, compilation or packaging failed.
goto :failed

:failed
echo.
echo The build did not complete. See the messages above.
echo.
pause
exit /b 1
