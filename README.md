# NightLight

NightLight is a lightweight, portable Windows utility for adjusting display color temperature, brightness and contrast. It is written in native C++/Win32, runs in the system tray, and does not require a browser engine or an installer.

## Features

- Independent color temperature (1000–6500 K) and enable/disable controls for each monitor.
- Three selectable color correction engines: GPU gamma, monitor hardware (DDC/CI), and NVIDIA Target Gamma.
- Hardware brightness and contrast controls via DDC/CI, with WMI brightness support for compatible internal displays.
- Custom monitor names, reorderable monitor tabs, and saved per-monitor settings.
- Automatic monitor detection and restoration of saved adjustments after display sleep or system resume.
- Starts in the system tray; single-click the tray icon to open or hide the window.
- Optional close-by-clicking-outside behavior and a button to turn off the displays without suspending the PC.
- Light, dark and system themes; English and European Portuguese.
- Low idle overhead through event-driven workers, deferred hardware discovery, and drawing-buffer cleanup when hidden.

## NVIDIA context-menu fix

On some NVIDIA systems, Windows Night Light, f.lux or conventional GPU gamma correction can cause dark flashes or abnormal fade-in effects in Chromium-based browser context menus when Multiplane Overlay (MPO) is active.

NightLight's **NVIDIA Target Gamma** engine uses the NVIDIA driver's target gamma interface (`NvAPI_DISP_SetTargetGammaCorrection`) and an independent 1,024-entry RGB lookup table per channel. This workaround was successfully tested on an NVIDIA GeForce RTX 3050 with Chrome and Brave.

It does not disable MPO, DirectComposition or browser hardware acceleration, change browser settings, or use an on-screen color overlay.

### Enable the fix

1. Turn off Windows Night Light and exit other color-temperature utilities.
2. Restore the default desktop color settings in NVIDIA Control Panel.
3. Right-click the NightLight tray icon and select **Colour correction engine → your monitor → NVIDIA target gamma**.
4. Open NightLight and adjust the color temperature.

The status bar shows **NV target · Accepted** when the driver accepts the requested adjustment. This is an experimental, undocumented NVIDIA interface; acceptance is not independent readback verification. SDR with Advanced Color disabled is required. Compatibility with other GPUs and driver versions is not guaranteed. When disabled or on exit, the engine attempts to restore a neutral LUT rather than a previous custom NVIDIA calibration. It does not silently fall back to legacy GPU gamma.

## Build and run

1. Download or clone this repository.
2. Install Visual Studio C++ Build Tools with the Windows SDK and CMake (3.23 or newer).
3. Run `compile.bat` to build the x64 Release executable and run the regression tests.
4. Run the resulting `NightLight.exe` and open it from the system tray.

The build script uses a short temporary directory to avoid MSBuild path-length errors. Keep the executable in a writable folder: settings and recovery files are stored alongside it.

## Performance

With successful NVIDIA Target Gamma correction on all monitors, the periodic color watchdog is not needed. Hardware polling is suspended when the controls are hidden, and inactive workers sleep until needed.

To measure real CPU usage, memory and resource counts on your own system, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\measure-performance.ps1 -Seconds 60
```

## Compatibility

Designed for modern Windows 10 and Windows 11 systems. DDC/CI features depend on monitor firmware and connection type; enable DDC/CI in the monitor's own settings if required. NVIDIA Target Gamma requires a compatible NVIDIA driver and display connection.

The NVIDIA target gamma interface is based on independently documented reverse-engineered ABI information from [nvBrightness](https://github.com/pbatard/nvBrightness) and [NvProfileSwitcher](https://github.com/mgcarnevali/NvProfileSwitcher).


<img width="709" height="721" alt="image" src="https://github.com/user-attachments/assets/27c4af10-1ee6-450f-a667-87a7a706ede3" />


The tray icon automatically switches between light- and dark-taskbar variants using the Windows system theme, independently of NightLight's app theme.

The Fluent-inspired sun icon has dedicated light/dark tray variants rasterized at native small-icon sizes, and a separate full-size executable icon up to 256×256 pixels.
