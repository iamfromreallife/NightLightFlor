@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0" || exit /b 1
set "ROOT=%CD%"
set "CMAKE=cmake.exe"
set "CTEST=ctest.exe"
if not exist "%ROOT%\CMakeLists.txt" goto missing_source
if not exist "%ROOT%\main.cpp" goto missing_source
where cmake.exe >nul 2>&1
if not errorlevel 1 goto tools_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_tools
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto missing_tools
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
:tools_ready
"%CMAKE%" --version >nul 2>&1
if errorlevel 1 goto missing_tools
"%CTEST%" --version >nul 2>&1
if errorlevel 1 goto missing_tools

rem Stage only build inputs outside the potentially deep extraction folder.
rem A unique short directory also guarantees a clean CMake configuration.
:allocate
set "STAGE=%TEMP%\NL%RANDOM%%RANDOM%"
if exist "%STAGE%" goto allocate
mkdir "%STAGE%" || goto failed
mkdir "%STAGE%\s" || goto failed
set "SRC=%STAGE%\s"
set "BUILD=%STAGE%\b"
echo Building in: "%STAGE%"
for %%F in (CMakeLists.txt main.cpp startup_task.cpp startup_task.hpp inactive_media.cpp inactive_media.hpp inactive_dialog.hpp core.cpp core.hpp display_identity.cpp display_identity.hpp ddc.cpp ddc.hpp wmi_brightness.cpp wmi_brightness.hpp json.hpp menu_icons.hpp motion.hpp paint_surface.hpp tests.cpp icon.ico icon-dark.ico icon-light.ico app.manifest resources.rc.in) do (
    if not exist "%ROOT%\%%F" (
        echo ERROR: Missing source file %%F
        goto failed
    )
    copy /b /y "%ROOT%\%%F" "%SRC%\%%F" >nul
    if errorlevel 1 goto failed
)
set "CMAKE_GENERATOR="
set "CMAKE_GENERATOR_PLATFORM="
set "CMAKE_GENERATOR_TOOLSET="
echo [1/4] Configuring x64...
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -A x64 -DBUILD_TESTING=ON
if errorlevel 1 goto failed
echo [2/4] Compiling Release...
"%CMAKE%" --build "%BUILD%" --config Release --parallel
if errorlevel 1 goto failed
echo [3/4] Running regression tests...
"%CTEST%" --test-dir "%BUILD%" -C Release --output-on-failure
if errorlevel 1 goto failed
echo [4/4] Copying executable...
if not exist "%BUILD%\Release\NightLight.exe" goto failed
copy /b /y "%BUILD%\Release\NightLight.exe" "%ROOT%\NightLight.exe" >nul
if errorlevel 1 goto failed
rmdir /s /q "%STAGE%"
echo.
echo SUCCESS: NightLight v1.0.11 - "%ROOT%\NightLight.exe"
popd
pause
exit /b 0

:missing_source
echo ERROR: Place compile.bat beside CMakeLists.txt and main.cpp.
goto failed
:missing_tools
echo ERROR: Visual Studio C++ Build Tools, Windows SDK and CMake with CTest are required.
:failed
echo.
echo BUILD FAILED. Review the first error above.
echo Close any running NightLight before compiling.
if defined STAGE echo Temporary build retained for diagnosis: "%STAGE%"
popd
pause
exit /b 1
