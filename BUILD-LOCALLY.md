# NightLight - Build locally

Double-click **compile.bat** on Windows.

Requirements: Windows 10/11 x64, Visual Studio 2022 or later (or Visual Studio Build Tools), the **Desktop development with C++** workload including MSVC x64 tools and Windows SDK, and **CMake 3.23+**. Ninja is optional. No GitHub Actions are needed.

The batch file detects the MSVC toolchain using `vswhere.exe`, selects Ninja if installed (or NMake otherwise), configures a Release build, compiles the application and regression tests, runs `ctest`, and puts the portable executable at **dist\\NightLight.exe**.

If the batch file reports missing tools, install them through Visual Studio Installer and run it again. It does not modify your Windows startup tasks or your NightLight configuration.

**Source base:** main commit `1c6e64e165c526fab3209f271fe7fc8ccb6a580f`, with an additional cursor-rendering compatibility repair on this local-build branch.

## Hardware cursor and colour correction

The app uses per-monitor gamma LUTs and does **not** use a fullscreen Magnification colour matrix.

An earlier NightLight mouse-cursor workaround set `HKCU\\Control Panel\\Mouse\\MouseTrails` to `-1` to force software cursor rendering. **Software cursors can become warm** under gamma correction. On startup, NightLight now detects **only that `-1` value** and disables mouse trails using the documented Windows `SystemParametersInfoW(SPI_SETMOUSETRAILS, 0, ...)` API. Normal user mouse-trail values are left intact.

This **permits** Windows to use a hardware cursor but cannot require it: the Windows graphics stack and driver choose the actual cursor plane. Microsoft documents that a hardware cursor *typically* bypasses display gamma, but not always. A physically white cursor cannot be guaranteed on every GPU/display setup.

If you previously ran `Apply.cmd` from `NightLight-Cursor-Fix-Reversible.zip`, also run its `Restore.cmd` (or sign out and back in after letting NightLight clear the setting).

No Windows mouse setting is modified except when the previous NightLight `MouseTrails=-1` workaround is detected.

