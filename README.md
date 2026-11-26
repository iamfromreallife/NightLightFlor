# NightLight

NightLight is a lightweight, portable Windows utility for adjusting display color temperature, brightness and contrast. It is written in native C++/Win32, runs in the system tray, and does not require a browser engine or an installer.

## Features

- Independent color temperature (1000–6500 K) and enable/disable controls for each monitor.
- Three selectable color correction engines: GPU gamma, monitor hardware (DDC/CI), and NVIDIA Target Gamma.
- Hardware brightness and contrast controls via DDC/CI, with WMI brightness support for compatible internal displays.
- Custom monitor names, reorderable monitor tabs, and saved per-monitor settings.
- Automatic monitor detection and restoration of saved adjustments after display sleep or system resume.
- Starts in the system tray; single-click the tray icon to open or hide the window.
- Optional **Start with Windows** in the tray menu, directly below Theme. Creates a per-user scheduled logon task; disabling deletes that task. No administrator elevation or password is requested. If you move the portable executable, disable and re-enable the option to update its path.
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

## Compatibility

Designed for modern Windows 10 and Windows 11 systems. DDC/CI features depend on monitor firmware and connection type; enable DDC/CI in the monitor's own settings if required. NVIDIA Target Gamma requires a compatible NVIDIA driver and display connection.



<img width="772" height="756" alt="Captura de ecrã 2026-11-26 013315" src="https://github.com/user-attachments/assets/515c6330-c201-415f-966d-6ca7134f2df2" />

<img width="742" height="791" alt="Captura de ecrã 2026-11-26 013230" src="https://github.com/user-attachments/assets/f3ba794c-781c-4cb8-972f-cfc0eaa5872a" />

<img width="289" height="261" alt="Captura de ecrã 2026-11-26 013253" src="https://github.com/user-attachments/assets/fb54d17b-adbd-4fb6-b224-a3ba423b6126" />




Display wake recovery reasserts all connected monitors' saved color temperatures and known brightness/contrast values across multiple delayed passes, even when the driver initially accepts a write. It preserves physical monitor identity and protects saved targets from transient readbacks. The recovery timer stops after the bounded sequence once the topology is stable.

On every launch, including scheduled logon startup, the application runs the same stable-topology recovery sequence as display wake. Saved per-monitor brightness and contrast are actively written even if the UI remains closed. A settled hardware readback detects mismatches and triggers bounded retries; unknown values are discovered rather than written as defaults.

Interactive tray opening and closing use native Win32/DWM transitions instead of a custom fade/slide. Every physical tray press toggles immediately, including rapid double clicks. Initial launch remains silent; internal control animations are unchanged. Windows determines whether a particular transition is animated according to system settings and window state.

Repeated tray toggles now use genuine Win32 minimize/restore state transitions. The hidden owner remains in place, so the app does not create a taskbar button. Iconified windows stop UI rendering and hardware UI polling. If Windows exposes a minimized desktop icon, only that iconic representation is hidden. Native animation appearance remains controlled by DWM and Windows settings.

Each reopening now creates a fresh native top-level frame while retaining the existing child controls and a persistent hidden tray-service owner. The tray icon, power notifications, monitor workers, configuration and timers are not restarted. This requests the stock first-show DWM transition on every opening without creating a taskbar button or waiting for the compositor. The previous minimize/restore approach is replaced; Windows still controls the actual appearance of native animations.

Performance audit (1.0.10): bounded per-font text measurements and ink-centred glyph paths are reused across rendering and frame replacement; stock GDI brushes replace temporary solid brushes; common DDC layout measurements are computed once; duplicate tray menu images share a bitmap within each popup; neutral gamma ramps are reused. Caches are invalidated on DPI/font changes and GDI+ paths are released before shutdown. Hardware I/O schedules, monitor identity checks, verification, persistence and animation timings are intentionally unchanged. The optional `measure-performance.ps1` also reports GDI and USER handle counts to help detect frame recreation leaks.

The NVIDIA path now checks physical identity and Advanced Color status against the same fresh CCD snapshot, eliminating two duplicate topology enumerations per actual target-gamma write while retaining preflight and final validation. Control clipping geometry is cached only after successful native assignment, avoiding redundant region allocation and window-position messages during animation; cache entries are dropped on control destruction, DPI/font changes and frame replacement.

## Dim when inactive (optional)
The tray menu has **Dim when inactive** immediately below Close by clicking outside. Enable it, select all or individual physically identified displays, choose black overlay opacity (1–99%, default 90% for new configurations), and choose 2, 5, 10, 15, 30 or 60 minutes or a custom interval (1–1440 minutes; default 5). Existing saved dim amounts are preserved. Checked entries indicate the selected options. Each selected screen is independently active while the mouse is over it, keyboard activity is directed at its foreground window, or a playing media session belongs to its foreground application. Keyboard raw-input notifications are used only as activity signals; no key payload is read or logged. The MSVC build checks Windows system media playback and active Core Audio sessions; the LLVM-MinGW build uses Core Audio only. App-level media detection cannot reliably distinguish separate windows/tabs of the same application or silent media that does not expose a Windows playback session.

Dimming now uses one native black layered window per inactive display. It is click-through, non-activating, topmost, absent from the taskbar and Alt+Tab, and removed on activity, disabling, sleep, topology changes or exit. No DDC/CI, WMI or gamma writes are used for temporary dimming. Normal brightness, contrast and color correction remain unchanged. A 99% setting leaves a very faint image. The overlay is static and allocated only when needed; no extra per-frame rendering loop. Exclusive fullscreen, protected content and HDR may behave differently, and composition can affect MPO eligibility while an overlay is visible. The previously saved normal hardware targets are restored by existing startup recovery if upgrading from an interrupted hardware-dimming session.
