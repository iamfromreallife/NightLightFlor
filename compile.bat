@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0" || exit /b 1
set "ROOT=%CD%"
set "BUILD=%ROOT%\build\msvc-x64"
set "CMAKE=cmake.exe"
set "CTEST=ctest.exe"
if not exist CMakeLists.txt goto failed
where cmake.exe >nul 2>&1
if not errorlevel 1 goto configure
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto failed
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto failed
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
:configure
set "CMAKE_GENERATOR="
set "CMAKE_GENERATOR_PLATFORM="
set "CMAKE_GENERATOR_TOOLSET="
"%CMAKE%" -S "%ROOT%" -B "%BUILD%" -A x64 -DBUILD_TESTING=ON
if errorlevel 1 goto failed
"%CMAKE%" --build "%BUILD%" --config Release --parallel
if errorlevel 1 goto failed
"%CTEST%" --test-dir "%BUILD%" -C Release --output-on-failure
if errorlevel 1 goto failed
copy /Y "%BUILD%\Release\NightLight.exe" "%ROOT%\NightLight.exe" >nul
if errorlevel 1 goto failed
echo SUCCESS: "%ROOT%\NightLight.exe"
popd
pause
exit /b 0
:failed
echo BUILD FAILED. Install Visual Studio C++ Build Tools, Windows SDK and CMake.
echo Close any running NightLight before compiling. Review the output above.
popd
pause
exit /b 1
