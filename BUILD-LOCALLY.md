# NightLight - Build locally

Double-click **compile.bat** on Windows.

Requirements: Windows 10/11 x64, Visual Studio 2022 or later (or Visual Studio Build Tools), the **Desktop development with C++** workload including MSVC x64 tools and Windows SDK, and **CMake 3.23+**. Ninja is optional. No GitHub Actions are needed.

The batch file detects the MSVC toolchain using `vswhere.exe`, selects Ninja if installed (or NMake otherwise), configures a Release build, compiles the application and regression tests, runs `ctest`, and puts the portable executable at **dist\\NightLight.exe**.

If the batch file reports missing tools, install them through Visual Studio Installer and run it again. It does not modify your Windows startup tasks or your NightLight configuration.

**Source snapshot:** main commit `1c6e64e165c526fab3209f271fe7fc8ccb6a580f` (gamma-only cursor fix).
