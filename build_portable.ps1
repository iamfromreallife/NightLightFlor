param([string]$Toolchain, [string]$OutputPath)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
if (-not $OutputPath) { $OutputPath = Join-Path $projectRoot 'NightLight.exe' }
if (-not $Toolchain) {
    $localTools = Join-Path $projectRoot 'build/toolchain'
    if (-not (Test-Path -LiteralPath $localTools)) {
        $localTools = Join-Path $env:LOCALAPPDATA 'NightLightBuildTools'
    }
    $Toolchain = (Get-ChildItem -LiteralPath $localTools -Directory -Filter 'llvm-mingw-*' | Select-Object -First 1).FullName
}
$compiler = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-clang++.exe'
$windres = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-windres.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Pass -Toolchain with the path to an extracted x64 LLVM-MinGW UCRT toolchain.' }
$output = Join-Path $projectRoot 'build/native-llvm'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$icon = (Join-Path $PSScriptRoot 'icon.ico').Replace('\','/')
$iconDark = (Join-Path $PSScriptRoot 'icon-dark.ico').Replace('\','/')
$iconLight = (Join-Path $PSScriptRoot 'icon-light.ico').Replace('\','/')
if (-not (Test-Path -LiteralPath $icon) -or -not (Test-Path -LiteralPath $iconDark) -or -not (Test-Path -LiteralPath $iconLight)) { throw 'Missing application or tray icon resource.' }
$manifest = (Join-Path $PSScriptRoot 'app.manifest').Replace('\','/')
$resource = Join-Path $output 'app.rc'
@("101 ICON `"$icon`"", "102 ICON `"$iconDark`"", "103 ICON `"$iconLight`"", "1 24 `"$manifest`"") | Set-Content -LiteralPath $resource
& $windres -i $resource -o "$output/app.res"
if ($LASTEXITCODE) { throw 'Resource compilation failed' }
$common = @('-std=c++17','-O2','-static','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0A00','-DNTDDI_VERSION=0x0A00000C')
$libraries = @('-luser32','-lgdi32','-lshell32','-lole32','-loleaut32','-lwbemuuid','-ladvapi32','-luuid','-ldxva2')
& $compiler @common "$PSScriptRoot/core.cpp" "$PSScriptRoot/display_identity.cpp" "$PSScriptRoot/ddc.cpp" "$PSScriptRoot/wmi_brightness.cpp" "$PSScriptRoot/tests.cpp" -o "$output/nightlight-native-tests.exe" @libraries
if ($LASTEXITCODE) { throw 'Test compilation failed' }
& "$output/nightlight-native-tests.exe"
if ($LASTEXITCODE) { throw 'Native regressions failed' }
& $compiler @common -s -municode -mwindows "$PSScriptRoot/core.cpp" "$PSScriptRoot/display_identity.cpp" "$PSScriptRoot/ddc.cpp" "$PSScriptRoot/wmi_brightness.cpp" "$PSScriptRoot/main.cpp" "$output/app.res" -o $OutputPath @libraries -lcomctl32 -ldwmapi -luxtheme -lgdiplus
if ($LASTEXITCODE) { throw 'Portable compilation failed' }
Get-Item -LiteralPath $OutputPath | Select-Object FullName,Length
Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256
