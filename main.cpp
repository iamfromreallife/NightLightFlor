#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <cwchar>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "core.hpp"
#include "ddc.hpp"
#include "motion.hpp"
#include "menu_icons.hpp"
#include "paint_surface.hpp"
#include "startup_task.hpp"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using nightlight::Model;
using nightlight::Monitor;
using nightlight::Preferences;

namespace {
constexpr int ID_DDC = 1011, ID_DDC_SLIDER_BASE = 5000, ID_DDC_MONITOR_BASE = 6000;
constexpr int ID_RENAME_EDIT = 1006;
constexpr int ID_GAMMA = 1010, ID_REFRESH = 1012, ID_SCREEN_OFF = 1013;
constexpr UINT TIMER_GAMMA = 11;
constexpr UINT TIMER_SAVE_DDC = 13;
constexpr UINT TIMER_ANIMATION = 16;
constexpr UINT TIMER_SAVE_WINDOW = 17;
constexpr UINT TIMER_VERIFY_COLOR = 14, TIMER_READ_DDC = 15;
// The interactive-session notification also covers display sleep without a
// topology change (WM_DISPLAYCHANGE alone does not).
constexpr GUID displayStateGuid{0x2b84c20e,0xad23,0x4ddf,{0x93,0xdb,0x05,0xff,0xbd,0x7e,0xfc,0xa5}};
constexpr int ID_MONITOR_BASE = 2000;
constexpr int ID_TRAY_DISABLE_BASE = 3000, ID_TRAY_EXIT = 3999;
constexpr int ID_TRAY_CLOSE_OUTSIDE = 4103, ID_TRAY_STARTUP = 4104;
constexpr int ID_TRAY_LANGUAGE_BASE = 4200, ID_TRAY_THEME_BASE = 4300;
constexpr int ID_TRAY_COLOR_GPU_BASE=7000, ID_TRAY_COLOR_HW_BASE=8000, ID_TRAY_COLOR_TARGET_BASE=10000;
constexpr UINT WM_TRAY_ICON = WM_APP + 37;
constexpr UINT WM_GAMMA_COMPLETE = WM_APP + 41;
constexpr UINT WM_MINIMIZE_TO_TRAY = WM_APP + 43;
constexpr UINT TIMER_DISPLAYS = 10;
constexpr UINT TIMER_TRAY_RETRY = 21;
constexpr wchar_t CLASS_NAME[] = L"NightLight.Native.Win32";
constexpr int ICON_RESOURCE = 101; // full-size Explorer and application icon
constexpr int ICON_RESOURCE_DARK_TASKBAR = 102; // white circle, black sun
constexpr int ICON_RESOURCE_LIGHT_TASKBAR = 103; // black circle, white sun
constexpr UINT TRAY_ID = 100;

// Standalone adaptation of the supplied Windhawk mod's input protection.
// There is NO BlockInput: physical low-level mouse/key-down events are
// intercepted for three seconds, while asynchronous display-state notices
// and intercepted input can trigger bounded, event-driven sleep corrections.
// Hardware/driver-level wakeups remain outside user-mode hook guarantees.
constexpr UINT WM_SCREEN_GUARD_CORRECTION=WM_APP+44;
constexpr UINT TIMER_SCREEN_OFF_ACTION=19;
constexpr GUID consoleDisplayStateGuid{
    0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};

class ScreenOffInputGuard {
public:
    ScreenOffInputGuard():stopEvent_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {}
    ~ScreenOffInputGuard() {
        stop();
        if (stopEvent_) CloseHandle(stopEvent_);
    }
    bool start() {
        if (!stopEvent_ || active_.load(std::memory_order_acquire)) return false;
        if (worker_.joinable()) worker_.join();
        ResetEvent(stopEvent_);
        offRequested_.store(false,std::memory_order_release);
        seenDisplayOff_.store(false,std::memory_order_release);
        correctionPending_.store(false,std::memory_order_release);
        lastCorrection_.store(0,std::memory_order_release);
        correctionCount_.store(0,std::memory_order_release);
        generation_.fetch_add(1);
        auto started=std::make_shared<std::promise<bool>>();
        auto ready=started->get_future();
        try {
            worker_=std::thread([this,started] {
                run(started);
            });
        } catch (...) { return false; }
        if (ready.wait_for(std::chrono::milliseconds(1500))!=
                std::future_status::ready) {
            stop();
            return false;
        }
        return ready.get();
    }
    void markOffRequested() {
        if (guarding()) offRequested_.store(true,std::memory_order_release);
    }
    bool guarding() const {
        const ULONGLONG deadline=deadline_.load(std::memory_order_acquire);
        return active_.load(std::memory_order_acquire) &&
            deadline!=0 && GetTickCount64()<deadline;
    }
    void stop() {
        if (stopEvent_) SetEvent(stopEvent_);
        deadline_.store(0,std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        active_.store(false,std::memory_order_release);
    }
private:
    static std::atomic<ScreenOffInputGuard*> current_;
    static constexpr const wchar_t* kPowerClass=
        L"NightLight.ScreenOff.PowerNotifications";
    static LRESULT CALLBACK mouseHook(int code,WPARAM wp,LPARAM lp) {
        auto* self=current_.load(std::memory_order_acquire);
        if (self && code==HC_ACTION && lp && self->guarding()) {
            const auto* event=reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (!(event->flags&LLMHF_INJECTED)) {
                self->queueCorrection(1);
                return 1; // physical input; injected events are untouched
            }
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    static LRESULT CALLBACK keyboardHook(int code,WPARAM wp,LPARAM lp) {
        auto* self=current_.load(std::memory_order_acquire);
        if (self && code==HC_ACTION && lp && self->guarding()) {
            const auto* event=reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            if (!(event->flags&LLKHF_INJECTED) &&
                wp!=WM_KEYUP && wp!=WM_SYSKEYUP) {
                self->queueCorrection(1);
                return 1; // leave key-up alone to avoid stuck keys
            }
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    static LRESULT CALLBACK powerWindowProc(HWND hwnd,UINT msg,
                                            WPARAM wp,LPARAM lp) {
        if (msg==WM_NCCREATE) {
            const auto* created=reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(created->lpCreateParams));
            return TRUE;
        }
        auto* self=reinterpret_cast<ScreenOffInputGuard*>(
            GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (self && msg==WM_POWERBROADCAST &&
            wp==PBT_POWERSETTINGCHANGE && lp) {
            const auto* power=reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (IsEqualGUID(power->PowerSetting,consoleDisplayStateGuid) &&
                power->DataLength>=sizeof(DWORD)) {
                DWORD state=0;
                CopyMemory(&state,power->Data,sizeof(state));
                if (self->guarding() &&
                    self->offRequested_.load(std::memory_order_acquire)) {
                    if (state==0) {
                        self->seenDisplayOff_.store(true,std::memory_order_release);
                    } else if (state==1 &&
                               self->seenDisplayOff_.load(std::memory_order_acquire)) {
                        self->queueCorrection(2);
                    }
                }
            }
            return TRUE;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    void queueCorrection(WPARAM reason) {
        if (!guarding() ||
            !offRequested_.load(std::memory_order_acquire) ||
            correctionPending_.exchange(true,std::memory_order_acq_rel)) return;
        const DWORD tid=threadId_.load(std::memory_order_acquire);
        if (!tid || !PostThreadMessageW(tid,WM_SCREEN_GUARD_CORRECTION,
                            reason,generation_.load(std::memory_order_acquire)))
            correctionPending_.store(false,std::memory_order_release);
    }
    void correctUnexpectedWake(DWORD generation) {
        correctionPending_.store(false,std::memory_order_release);
        if (generation!=generation_.load(std::memory_order_acquire) ||
            !guarding() || !offRequested_.load(std::memory_order_acquire)) return;
        const ULONGLONG now=GetTickCount64();
        const ULONGLONG previous=lastCorrection_.load(std::memory_order_acquire);
        const unsigned count=correctionCount_.load(std::memory_order_acquire);
        if (!nightlight::shouldCorrectScreenWake(now,previous,count)) return;
        lastCorrection_.store(now,std::memory_order_release);
        correctionCount_.store(count+1,std::memory_order_release);
        // Only a real physical event or a power-state transition schedules
        // this extra request. No polling, retry timer or synchronous broadcast.
        PostMessageW(HWND_BROADCAST,WM_SYSCOMMAND,SC_MONITORPOWER,2);
    }
    void run(const std::shared_ptr<std::promise<bool>>& started) {
        MSG message{};
        PeekMessageW(&message,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
        threadId_.store(GetCurrentThreadId(),std::memory_order_release);
        current_.store(this,std::memory_order_release);
        HINSTANCE instance=GetModuleHandleW(nullptr);
        HHOOK mouse=SetWindowsHookExW(WH_MOUSE_LL,mouseHook,instance,0);
        HHOOK keyboard=mouse?
            SetWindowsHookExW(WH_KEYBOARD_LL,keyboardHook,instance,0):nullptr;
        WNDCLASSW cls{};
        cls.hInstance=instance;
        cls.lpfnWndProc=powerWindowProc;
        cls.lpszClassName=kPowerClass;
        const bool registered=RegisterClassW(&cls)!=0;
        HWND powerWindow=registered?
            CreateWindowExW(0,kPowerClass,L"",0,0,0,0,0,
                            HWND_MESSAGE,nullptr,instance,this):nullptr;
        HPOWERNOTIFY powerNotify=powerWindow?
            RegisterPowerSettingNotification(powerWindow,&consoleDisplayStateGuid,
                                              DEVICE_NOTIFY_WINDOW_HANDLE):nullptr;
        const bool ready=mouse && keyboard &&
            WaitForSingleObject(stopEvent_,0)!=WAIT_OBJECT_0;
        if (ready) {
            deadline_.store(GetTickCount64()+nightlight::kScreenOffInputGuardMs,
                            std::memory_order_release);
            active_.store(true,std::memory_order_release);
        }
        started->set_value(ready);
        if (ready) {
            bool quit=false;
            while (WaitForSingleObject(stopEvent_,0)!=WAIT_OBJECT_0) {
                const ULONGLONG now=GetTickCount64();
                const ULONGLONG deadline=deadline_.load(std::memory_order_acquire);
                if (!deadline || now>=deadline) break;
                const DWORD remaining=static_cast<DWORD>(
                    std::min<ULONGLONG>(deadline-now,MAXDWORD));
                const DWORD wait=MsgWaitForMultipleObjectsEx(
                    1,&stopEvent_,remaining,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if (wait==WAIT_OBJECT_0) break;
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if (message.message==WM_QUIT) { quit=true; break; }
                    if (message.message==WM_SCREEN_GUARD_CORRECTION) {
                        correctUnexpectedWake(static_cast<DWORD>(message.lParam));
                        continue;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (quit) break;
            }
        }
        // Expiry is enforced by the worker even if UI message timers stall.
        deadline_.store(0,std::memory_order_release);
        active_.store(false,std::memory_order_release);
        offRequested_.store(false,std::memory_order_release);
        current_.store(nullptr,std::memory_order_release);
        if (powerNotify) UnregisterPowerSettingNotification(powerNotify);
        if (powerWindow) DestroyWindow(powerWindow);
        if (registered) UnregisterClassW(kPowerClass,instance);
        if (keyboard) UnhookWindowsHookEx(keyboard);
        if (mouse) UnhookWindowsHookEx(mouse);
        threadId_.store(0,std::memory_order_release);
        correctionPending_.store(false,std::memory_order_release);
    }
    HANDLE stopEvent_=nullptr;
    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> offRequested_{false};
    std::atomic<bool> seenDisplayOff_{false};
    std::atomic<bool> correctionPending_{false};
    std::atomic<unsigned> correctionCount_{0};
    std::atomic<ULONGLONG> lastCorrection_{0};
    std::atomic<ULONGLONG> deadline_{0};
    std::atomic<DWORD> threadId_{0};
    std::atomic<DWORD> generation_{0};
};
std::atomic<ScreenOffInputGuard*> ScreenOffInputGuard::current_{nullptr};

struct Palette {
    COLORREF page, header, card, border, text, subtle, muted;
    COLORREF chip, selected, selectedText, accent, button, outline;
};
COLORREF mix(COLORREF a, COLORREF b, double f);
Palette colors(bool dark) {
    Palette p=dark ? Palette{
        RGB(32,32,32),RGB(32,32,32),RGB(43,43,43),RGB(59,59,59),
        RGB(245,245,245),RGB(208,208,208),RGB(172,172,172),
        RGB(51,51,51),0,0,0,0,0} : Palette{
        RGB(243,243,243),RGB(243,243,243),RGB(255,255,255),RGB(227,227,227),
        RGB(32,32,32),RGB(88,88,88),RGB(104,104,104),
        RGB(250,250,250),0,0,0,0,0};
    p.accent=RGB(238,75,35); // #EE4B23
    p.selected=mix(p.card,p.accent,dark?0.18:0.09);
    p.selectedText=p.text;
    p.button=p.chip;
    p.outline=p.border;
    return p;
}
COLORREF mix(COLORREF a, COLORREF b, double f) {
    const auto c = [f](int x, int y) { return static_cast<int>(std::round(x + (y - x) * f)); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)),
               c(GetBValue(a), GetBValue(b)));
}
void fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
Gdiplus::Color drawingColor(COLORREF c) {
    return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c));
}
void rounded(HDC dc, RECT rect, int diameter, COLORREF background, COLORREF stroke) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float x=static_cast<float>(rect.left)+0.5f, y=static_cast<float>(rect.top)+0.5f;
    const float w=static_cast<float>(rect.right-rect.left)-1, h=static_cast<float>(rect.bottom-rect.top)-1;
    if (w<=0 || h<=0) return;
    const float d=std::min(static_cast<float>(diameter),std::min(w,h));
    Gdiplus::GraphicsPath path;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(drawingColor(background));
    Gdiplus::Pen pen(drawingColor(stroke),1.0f);
    graphics.FillPath(&brush,&path); graphics.DrawPath(&pen,&path);
}
void smoothCircle(HDC dc, float x, float y, float radius, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(drawingColor(color));
    graphics.FillEllipse(&brush,x-radius,y-radius,radius*2,radius*2);
}
ULONGLONG uiTime() {
    static const LONGLONG frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    return static_cast<ULONGLONG>((counter.QuadPart/frequency)*1000+(counter.QuadPart%frequency)*1000/frequency);
}
std::wstring formatNumber(int number) {
    wchar_t str[32]{};
    swprintf_s(str, L"%d", number);
    return str;
}
bool isEnterOrSpace(WPARAM key) { return key == VK_RETURN || key == VK_SPACE; }

class NativeApp {
public:
    NativeApp(HINSTANCE instance)
        : instance_(instance), model_() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&drawingToken_,&input,nullptr);
        BufferedPaintInit();
    }
    ~NativeApp() {
        removeTray();
        if (taskbarOwner_) DestroyWindow(taskbarOwner_);
        for (HFONT font : fonts_) if (font) DeleteObject(font);
        if (drawingToken_) Gdiplus::GdiplusShutdown(drawingToken_);
        BufferedPaintUnInit();
    }

    int run(int show) {
        gammaUnlocked_ = nightlight::gammaRangeUnlocked();
        // Do not write to the first, potentially incomplete boot topology.
        // The normal recovery timer will enumerate twice before restoring.
        model_.load();
        if (!model_.isReady()) {
            MessageBoxW(nullptr, model_.lastError().c_str(), L"NightLight",
                        MB_ICONERROR | MB_OK);
            return 2;
        }
        // Previous NightLight cursor-test scripts set MouseTrails=-1,
        // deliberately forcing software pointer composition. Undo ONLY that
        // particular workaround; regular pointer preferences remain intact.
        // The Windows compositor/GPU ultimately chooses hardware vs software.
        nightlight::clearLegacySoftwareCursorOverride();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&common);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = NativeApp::staticWindowProc;
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(ICON_RESOURCE));
        wc.hIconSm = static_cast<HICON>(
            LoadImageW(instance_, MAKEINTRESOURCEW(ICON_RESOURCE),
                       IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR|LR_SHARED));
        wc.hbrBackground = nullptr;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) return 3;

        int windowWidth=560;
        // Fixed first-run height. The user controls the window size from then on,
        // and scrollbars handle content that exceeds the available space.
        int windowHeight=620;
        if (const auto savedSize=model_.windowSize()) {
            windowWidth=static_cast<int>(savedSize->cx);
            windowHeight=static_cast<int>(savedSize->cy);
        }
        const auto savedPosition=model_.windowPosition();
        int x=savedPosition?static_cast<int>(savedPosition->x):
            (GetSystemMetrics(SM_CXSCREEN)-windowWidth)/2;
        int y=savedPosition?static_cast<int>(savedPosition->y):
            (GetSystemMetrics(SM_CYSCREEN)-windowHeight)/2;
        // Keep both saved dimensions and saved coordinates inside a current
        // monitor's work area; left-hand monitors may use negative coordinates.
        RECT proposed{x,y,x+windowWidth,y+windowHeight};
        MONITORINFO screen{sizeof(screen)};
        if (GetMonitorInfoW(MonitorFromRect(&proposed,MONITOR_DEFAULTTONEAREST),&screen)) {
            const RECT area=screen.rcWork;
            windowWidth=std::clamp(windowWidth,px(200),
                std::max(px(200),static_cast<int>(area.right-area.left)));
            windowHeight=std::clamp(windowHeight,px(260),
                std::max(px(260),static_cast<int>(area.bottom-area.top)));
            x=std::clamp(x,static_cast<int>(area.left),
                std::max(static_cast<int>(area.left),static_cast<int>(area.right)-windowWidth));
            y=std::clamp(y,static_cast<int>(area.top),
                std::max(static_cast<int>(area.top),static_cast<int>(area.bottom)-windowHeight));
        }
        // A hidden owner keeps the visible window off the taskbar without
        // changing its regular title bar, resizing, or caption buttons.
        taskbarOwner_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"NightLight Owner",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
        if (!taskbarOwner_) return 4;
        window_ = CreateWindowExW(0, CLASS_NAME, L"NightLight",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, windowWidth, windowHeight,
            taskbarOwner_, nullptr, instance_, this);
        if (!window_) return 4;

        // Enforce the fully expanded contents before any visible paint.
        // Otherwise a restored narrow window briefly flashes scrollable.
        initializingWindow_=false;
        ensureWindowFitsContents();
        // Never show the main window during startup. When a Scheduled Task
        // runs early at sign-in, Explorer's notification area may not yet be
        // available and the initial NIM_ADD can fail. Retrying tray registration
        // asynchronously (below) is safer than briefly showing the window.
        SetWindowPos(window_,nullptr,0,0,0,0,nightlight::kHideToTrayFlags);
        UpdateWindow(window_);
        noteWindowPosition();
        MSG msg{};
        BOOL messageResult=0;
        while ((messageResult=GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            const bool inlineKey=msg.hwnd==editName_ && msg.message==WM_KEYDOWN &&
                (msg.wParam==VK_RETURN || msg.wParam==VK_ESCAPE);
            if (inlineKey || !IsDialogMessageW(window_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return messageResult<0?1:static_cast<int>(msg.wParam);
    }

private:
    ULONG_PTR drawingToken_=0;
    HINSTANCE instance_;
    Model model_;
    nightlight::DdcController ddc_;
    HWND window_ = nullptr;
    HWND hardwareDrag_=nullptr;
    int hardwareDragLeft_=0,hardwareDragRight_=1;
    bool animationTimerRunning_=false;
    bool colorWatchdogRunning_=false,hardwarePollRunning_=false,hardwareDiscovered_=false;
    nightlight::PaintSurface pageSurface_;
    HWND taskbarOwner_ = nullptr;
    struct HardwareRow {
        std::wstring device;
        std::wstring physicalKey;
        HWND nameButton = nullptr, brightness = nullptr, contrast = nullptr;
        int top = 0;
        nightlight::Motion topMotion;
    };
    std::vector<HardwareRow> hardwareRows_;
    bool ddcVisible_ = true;
    struct ReorderPosition { nightlight::Motion x,y; };
    std::map<HWND,ReorderPosition> reorderPositions_;
    bool reorderInProgress_=false,screenDragArmed_=false,screenDragActive_=false;
    std::wstring screenDragDevice_;
    POINT screenDragStart_{};
    size_t screenDragSlot_=0;
    std::vector<RECT> screenDragSlots_;
    HPOWERNOTIFY displayPowerNotification_=nullptr;
    bool displayAwake_=true;
    bool displayRecoveryActive_=false;
    bool restoreDdcAfterWake_=false;
    unsigned recoveryPass_=0;
    unsigned recoverySettleChecks_=0;
    bool recoveryVerificationIssued_=false;
    int hardwareTop_ = 0;
    int hardwareHeight_ = 186;
    HWND btnRefresh_=nullptr, btnScreenOff_=nullptr, editName_=nullptr;
    ScreenOffInputGuard screenOffGuard_;
    HWND armedTabSwitch_=nullptr;
    bool editingName_=false;
    std::wstring editingDevice_;
    std::map<std::wstring,int> tabDesiredWidths_;
    std::vector<HWND> tabs_;
    HWND btnGamma_ = nullptr;
    bool gammaUnlocked_ = false;
    bool trayMenuOpen_=false;
    int temperatureHeight_ = 180;
    HWND gammaTooltip_ = nullptr;
    std::wstring gammaHelp_, screenOffHelp_;
    HANDLE gammaHelper_ = nullptr;

    HFONT font_ = nullptr, boldFont_ = nullptr, headlineFont_ = nullptr, smallFont_ = nullptr;
    // All three section headings share exactly the same HFONT handle.
    HFONT sectionTitleFont_ = nullptr;
    std::vector<HFONT> fonts_;
    size_t selected_ = 0;
    UINT dpi_ = 96;
    int width_ = 870, height_ = 440;
    RECT slider_ = {};
    int contentHeight_=0;
    bool layingOut_=false;
    bool initializingWindow_=true;
    unsigned layoutDepth_=0;
    ULONGLONG layoutTime_=0;
    int panelTop_ = 84, panelHeight_ = 142, secondPanelTop_ = 238;
    bool dragging_ = false;
    bool trayReady_ = false, shuttingDown_ = false;
    bool trayVersion4_ = false;
    nightlight::TrayClickFilter trayClickFilter_;
    UINT taskbarCreated_ = 0;
    NOTIFYICONDATAW notify_ = {};
    HICON trayIcon_=nullptr;
    int trayIconResource_=0, trayIconWidth_=0, trayIconHeight_=0;

    struct Interaction {
        nightlight::Motion hover{0},press{0},position;
        bool repainting=false;
        std::unique_ptr<nightlight::PaintSurface> surface;
    };
    std::map<HWND,Interaction> interactions_;
    nightlight::Motion temperaturePosition_,temperatureEngagement_{0},entrance_{0};
    nightlight::Motion hardwareReveal_{1},refreshTurn_{0};
    int hardwareVisibleHeight_=0;
    bool animations_=true,highContrast_=false,systemDark_=false,
        opening_=false,temperatureHover_=false;
    bool entranceAddedLayer_=false,entranceMove_=true,parentAnimating_=false,reflowAnimating_=false,composedTransition_=false;
    RECT entranceBounds_{};

    bool pt() const { return model_.preferences().locale == L"pt-PT"; }
    std::wstring tr(const wchar_t* english, const wchar_t* portuguese) const {
        return pt() ? portuguese : english;
    }
    Palette palette() const {
        auto p=colors(nightlight::effectiveDarkTheme(
            model_.preferences().theme,systemDark_));
        if (highContrast_) {
            p.page=p.header=p.card=GetSysColor(COLOR_WINDOW);
            p.text=p.subtle=p.muted=p.selectedText=GetSysColor(COLOR_WINDOWTEXT);
            p.border=p.outline=GetSysColor(COLOR_WINDOWTEXT);
            p.accent=GetSysColor(COLOR_HIGHLIGHT);
            p.chip=p.button=p.selected=p.card;
        }
        return p;
    }
    void updateMotionPreference() {
        BOOL enabled=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        highContrast_=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        animations_=enabled && !highContrast_ && !GetSystemMetrics(SM_REMOTESESSION);
        if (!animations_) {
            refreshTurn_.finish();
            finishEntrance();finishReordering();
            temperaturePosition_.finish(); temperatureEngagement_.finish();
            hardwareReveal_.finish();
            for (auto& item:interactions_) {
                item.second.hover.finish(); item.second.press.finish(); item.second.position.finish();
                InvalidateRect(item.first,nullptr,FALSE);
            }
            stopAnimationTimer();
            if (window_) layout();
        }
    }
    void startAnimationTimer() {
        if (!animationTimerRunning_ && IsWindowVisible(window_))
            animationTimerRunning_=SetTimer(window_,TIMER_ANIMATION,10,nullptr)!=0;
    }
    void stopAnimationTimer() { KillTimer(window_,TIMER_ANIMATION); animationTimerRunning_=false; }
    void finishReordering() {
        reorderInProgress_=false;
        for(auto& item:reorderPositions_){item.second.x.finish();item.second.y.finish();}
        for(auto& row:hardwareRows_)row.topMotion.finish();
    }
    bool reorderMotionActive(ULONGLONG now) const {
        for(const auto& item:reorderPositions_)if(item.second.x.active(now)||item.second.y.active(now))return true;
        for(const auto& row:hardwareRows_)if(row.topMotion.active(now))return true;
        return false;
    }
    RECT screenTabSwitchRect(HWND tab) const {
        RECT bounds{}; GetClientRect(tab,&bounds);
        // A generous 48px click target includes the visible 32px toggle.
        return RECT{bounds.right-px(48),bounds.top,bounds.right,bounds.bottom};
    }
    void toggleMonitor(size_t index) {
        if (index>=model_.monitors().size()) return;
        model_.updateEnabled(index,!model_.monitors()[index].enabled);
        updateTrayTooltip();
        if (index<tabs_.size()) InvalidateRect(tabs_[index],nullptr,FALSE);
        repaint(); // Kelvin display/disabled slider may have changed.
    }
    void beginScreenDrag(size_t index,POINT point) {
        if(index>=tabs_.size())return;
        commitDrag();endHardwareDrag();
        screenDragArmed_=true;screenDragActive_=false;
        screenDragDevice_=model_.monitors()[index].device;screenDragStart_=point;screenDragSlot_=index;
        screenDragSlots_.clear();
        for(size_t i=0;i<tabs_.size();++i) {
            RECT slot{};
            GetWindowRect(tabs_[i],&slot);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&slot),2);
            screenDragSlots_.push_back(slot);
        }
        SetCapture(window_);SetCursor(LoadCursorW(nullptr,IDC_HAND));
    }
    void moveScreenDrag(POINT point) {
        if(!screenDragArmed_)return;
        if(!screenDragActive_) {
            if(std::abs(point.x-screenDragStart_.x)<GetSystemMetrics(SM_CXDRAG) && std::abs(point.y-screenDragStart_.y)<GetSystemMetrics(SM_CYDRAG))return;
            screenDragActive_=true;
        }
        size_t target=screenDragSlot_;
        for(size_t i=0;i<screenDragSlots_.size();++i)if(PtInRect(&screenDragSlots_[i],point)){target=i;break;}
        if(target==screenDragSlot_)return;
        auto& monitors=model_.monitors();
        const auto current=std::find_if(monitors.begin(),monitors.end(),[&](const Monitor& m){return m.device==screenDragDevice_;});
        if(current==monitors.end())return;
        const size_t from=static_cast<size_t>(current-monitors.begin());
        const std::wstring selected=hasMonitor()?selectedMonitor()->device:L"";
        if(!model_.moveMonitor(from,target))return;
        auto move=[&](auto& values) {auto value=std::move(values[from]);values.erase(values.begin()+from);values.insert(values.begin()+target,std::move(value));};
        move(tabs_);move(hardwareRows_);
        for(size_t i=0;i<tabs_.size();++i) {
            SetWindowLongPtrW(tabs_[i],GWLP_ID,ID_MONITOR_BASE+i);
            SetWindowLongPtrW(hardwareRows_[i].nameButton,GWLP_ID,ID_DDC_MONITOR_BASE+i);
            SetWindowLongPtrW(hardwareRows_[i].brightness,GWLP_ID,ID_DDC_SLIDER_BASE+i*2);
            SetWindowLongPtrW(hardwareRows_[i].contrast,GWLP_ID,ID_DDC_SLIDER_BASE+i*2+1);
            if(monitors[i].device==selected)selected_=i;
        }
        screenDragSlot_=target;reorderInProgress_=true;layout();startAnimationTimer();
    }
    void finishScreenDrag(bool activate=false) {
        if(!screenDragArmed_)return;
        const bool moved=screenDragActive_;const auto device=screenDragDevice_;
        screenDragArmed_=screenDragActive_=false;screenDragSlots_.clear();
        if(GetCapture()==window_)ReleaseCapture();
        if(moved) {model_.save();updateTrayTooltip();}
        else if(activate) {
            const auto& monitors=model_.monitors();
            for(size_t i=0;i<monitors.size();++i)if(monitors[i].device==device){select(i);break;}
        }
    }
    void measureHardwareDrag(HWND control) {
        const auto position=SendMessageW(control,TBM_GETPOS,0,0);
        RECT thumb{};
        SendMessageW(control,TBM_SETPOS,TRUE,0); SendMessageW(control,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        hardwareDragLeft_=(thumb.left+thumb.right)/2;
        SendMessageW(control,TBM_SETPOS,TRUE,10000); SendMessageW(control,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        hardwareDragRight_=(thumb.left+thumb.right)/2;
        SendMessageW(control,TBM_SETPOS,TRUE,position);
    }
    void moveHardwareDrag(HWND control,int x,bool force=false) {
        const int position=nightlight::sliderPositionFromX(x,hardwareDragLeft_,hardwareDragRight_,0,10000);
        if (!force && SendMessageW(control,TBM_GETPOS,0,0)==position) return;
        SendMessageW(control,TBM_SETPOS,TRUE,position);
        SendMessageW(window_,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(control));
        InvalidateRect(control,nullptr,FALSE);
    }
    void endHardwareDrag(bool notify=true) {
        HWND control=hardwareDrag_; hardwareDrag_=nullptr;
        if (!control) return;
        if (GetCapture()==control) ReleaseCapture();
        const auto found=interactions_.find(control);
        if (found!=interactions_.end()) animate(found->second.press,0,160);
        if (notify && !shuttingDown_) SendMessageW(window_,WM_HSCROLL,TB_ENDTRACK,reinterpret_cast<LPARAM>(control));
    }
    void animate(nightlight::Motion& motion,double target,unsigned duration=140) {
        const auto now=uiTime();
        if (!animations_ || !IsWindowVisible(window_)) motion.snap(target);
        else motion.to(target,now,duration,true);
        if (motion.active(now)) startAnimationTimer();
    }
    static LRESULT CALLBACK interactionProc(HWND control,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto* app=reinterpret_cast<NativeApp*>(data);
        auto& state=app->interactions_[control];
        const auto tab=std::find(app->tabs_.begin(),app->tabs_.end(),control);
        if(tab!=app->tabs_.end()) {
            const size_t index=static_cast<size_t>(tab-app->tabs_.begin());
            if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
            if ((msg==WM_CAPTURECHANGED || msg==WM_CANCELMODE ||
                 msg==WM_NCDESTROY) && app->armedTabSwitch_==control)
                app->armedTabSwitch_=nullptr;
            const RECT switchArea=app->screenTabSwitchRect(control);
            const POINT cursor{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if ((msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) &&
                PtInRect(&switchArea,cursor)) {
                // The switch is interactive without selecting or dragging
                // its monitor and without triggering double-click rename.
                app->finishScreenDrag();
                app->armedTabSwitch_=control;
                SetFocus(control);
                SetCapture(control);
                app->animate(state.press,1,90);
                return 0;
            }
            if (msg==WM_LBUTTONUP && app->armedTabSwitch_==control) {
                const POINT cursor{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                RECT target=app->screenTabSwitchRect(control);
                const bool inside=PtInRect(&target,cursor)!=FALSE;
                app->armedTabSwitch_=nullptr;
                if (GetCapture()==control) ReleaseCapture();
                app->animate(state.press,0,160);
                if (inside) app->toggleMonitor(index);
                return 0;
            }
            if (msg==WM_LBUTTONDBLCLK) {
                app->finishScreenDrag();
                app->startInlineRename(index);return 0;
            }
            if(msg==WM_LBUTTONDOWN) {
                POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                MapWindowPoints(control,app->window_,&point,1);
                app->beginScreenDrag(index,point);return 0;
            }
        }
        if (app->hardwareRow(control)) {
            if ((msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) && IsWindowEnabled(control)) {
                app->commitDrag(); app->hardwareDrag_=control;
                SetFocus(control); SetCapture(control); app->measureHardwareDrag(control);
                app->animate(state.press,1,90); app->moveHardwareDrag(control,GET_X_LPARAM(lp),true);
                return 0;
            }
            if (msg==WM_MOUSEMOVE && app->hardwareDrag_==control && GetCapture()==control) {
                app->moveHardwareDrag(control,GET_X_LPARAM(lp)); return 0;
            }
            if (msg==WM_LBUTTONUP && app->hardwareDrag_==control) {
                app->moveHardwareDrag(control,GET_X_LPARAM(lp)); app->endHardwareDrag(); return 0;
            }
            if ((msg==WM_CAPTURECHANGED || msg==WM_CANCELMODE || msg==WM_NCDESTROY || (msg==WM_ENABLE && !wp)) && app->hardwareDrag_==control)
                app->endHardwareDrag(msg!=WM_NCDESTROY);
            if (msg==WM_SIZE && app->hardwareDrag_==control) {
                const auto result=DefSubclassProc(control,msg,wp,lp); app->measureHardwareDrag(control); return result;
            }
        }
        // Owner-drawn buttons and trackbars paint their entire background in
        // one buffered pass. Native background erasure would expose a blank
        // frame before WM_DRAWITEM / NM_CUSTOMDRAW supplies the content.
        if (msg==WM_ERASEBKGND) return 1;
        if (msg==WM_PAINT && app->composedTransition_) {
            PAINTSTRUCT paint{}; BeginPaint(control,&paint); EndPaint(control,&paint); return 0;
        }
        if (msg==WM_PAINT && app->hardwareRow(control)) {
            // Always paint the entire slider ourselves. DefSubclassProc
            // allows the native TRACKBAR_CLASS to draw its own intermediate
            // thumb/rail during startup and EnableWindow transitions,
            // producing single-frame flashes in either Windows theme.
            PAINTSTRUCT paint{};
            HDC dc=BeginPaint(control,&paint);
            if (dc) {
                NMCUSTOMDRAW draw{};
                draw.hdr.hwndFrom=control;
                draw.dwDrawStage=CDDS_PREPAINT;
                draw.hdc=dc;
                app->drawHardwareSlider(draw);
            }
            EndPaint(control,&paint);
            return 0;
        }
        if (msg==WM_PRINTCLIENT && app->hardwareRow(control)) {
            NMCUSTOMDRAW draw{};
            draw.hdr.hwndFrom=control;
            draw.dwDrawStage=CDDS_PREPAINT;
            draw.hdc=reinterpret_cast<HDC>(wp);
            if (draw.hdc) app->drawHardwareSlider(draw);
            return 0;
        }
        if (msg==WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,control,0}; TrackMouseEvent(&tracking);
            app->animate(state.hover,1);
        } else if (msg==WM_MOUSELEAVE) app->animate(state.hover,0);
        else if (msg==WM_LBUTTONDOWN || (msg==WM_KEYDOWN && isEnterOrSpace(wp))) app->animate(state.press,1,90);
        else if (msg==WM_LBUTTONUP || msg==WM_CAPTURECHANGED || msg==WM_KILLFOCUS ||
            (msg==WM_KEYUP && isEnterOrSpace(wp))) app->animate(state.press,0,160);
        if (msg==WM_NCDESTROY) {
            app->reorderPositions_.erase(control);
            app->interactions_.erase(control); RemoveWindowSubclass(control,interactionProc,id);
        }
        return DefSubclassProc(control,msg,wp,lp);
    }
    static LRESULT CALLBACK editFieldProc(HWND control,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto* app=reinterpret_cast<NativeApp*>(data);
        if(msg==WM_KEYDOWN && (wp==VK_RETURN || wp==VK_ESCAPE)) {
            app->finishInlineRename(wp==VK_RETURN);return 0;
        }
        if(msg==WM_CHAR && (wp==L'\r' || wp==L'\n'))return 0;
        if(msg==WM_PASTE && OpenClipboard(control)) {
            HANDLE contents=GetClipboardData(CF_UNICODETEXT);
            if(contents) {
                const wchar_t* pasted=static_cast<const wchar_t*>(GlobalLock(contents));
                if(pasted) {
                    // Single-line paste behaviour despite ES_MULTILINE.
                    std::wstring line=pasted;
                    const auto end=line.find_first_of(L"\r\n");
                    if(end!=std::wstring::npos)line.resize(end);
                    GlobalUnlock(contents);
                    CloseClipboard();
                    SendMessageW(control,EM_REPLACESEL,TRUE,
                        reinterpret_cast<LPARAM>(line.c_str()));
                    return 0;
                }
            }
            CloseClipboard();
        }
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(control,editFieldProc,id);
        return DefSubclassProc(control,msg,wp,lp);
    }
    void watchInteraction(HWND control) {
        if (control) { interactions_.try_emplace(control); SetWindowSubclass(control,interactionProc,1,reinterpret_cast<DWORD_PTR>(this)); }
    }
    void finishEntrance() {
        if (!opening_) return;
        opening_=false; entrance_.snap(1);
        if (entranceMove_) SetWindowPos(window_,nullptr,entranceBounds_.left,entranceBounds_.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetLayeredWindowAttributes(window_,0,255,LWA_ALPHA);
        if (entranceAddedLayer_) SetWindowLongPtrW(window_,GWL_EXSTYLE,GetWindowLongPtrW(window_,GWL_EXSTYLE)&~WS_EX_LAYERED);
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
    }
    void tickAnimations() {
        // Start work just after a compositor present, leaving a full frame
        // budget to resize, lay out and paint a coherent replacement surface.
        // Avoid blocking the short opening motion on DWM presentation.
        // Position and opacity are sampled together every animation tick.
        if (IsWindowVisible(window_) && animations_ && !opening_) DwmFlush();
        const auto now=uiTime();
        bool active=false;
        if (opening_) {
            const double progress=entrance_.value(now);
            SetLayeredWindowAttributes(window_,0,static_cast<BYTE>(std::round(255*progress)),LWA_ALPHA);
            if (entranceMove_) SetWindowPos(window_,nullptr,entranceBounds_.left,
                entranceBounds_.top+nightlight::windowOpeningSlideOffset(
                    static_cast<int>(dpi_),progress),
                0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            if (!entrance_.active(now)) finishEntrance();
            else active=true;
        }
        const bool reflow=hardwareReveal_.active(now) || reorderInProgress_;
        if (reflow || reflowAnimating_) layout();
        reflowAnimating_=reflow;
        if(reorderInProgress_ && !reorderMotionActive(now))reorderInProgress_=false;
        const bool parentMoving=temperaturePosition_.active(now) || temperatureEngagement_.active(now) || refreshTurn_.active(now) || reflow;
        if (parentMoving || parentAnimating_) repaint();
        parentAnimating_=parentMoving;
        if(refreshTurn_.active(now) && btnRefresh_)InvalidateRect(btnRefresh_,nullptr,FALSE);
        active=active || parentMoving;
        for (auto& item:interactions_) {
            auto& state=item.second;
            const bool moving=state.hover.active(now) || state.press.active(now) || state.position.active(now);
            if (moving || state.repainting) InvalidateRect(item.first,nullptr,FALSE);
            state.repainting=moving;
            active=active || moving;
        }
        if (IsWindowVisible(window_)) {
            // Present child controls and the resized frame together. Letting
            // WM_PAINT wait until after a resize exposes the previous content
            // bitmap at the new frame size for one compositor frame.
            RedrawWindow(window_,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_NOERASE);
            GdiFlush();
        }
        if (!active || !IsWindowVisible(window_)) stopAnimationTimer();
    }
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    bool hasMonitor() const { return selected_ < model_.monitors().size(); }
    const Monitor* selectedMonitor() const {
        return hasMonitor() ? &model_.monitors()[selected_] : nullptr;
    }
    void repaint() {
        if (window_ && IsWindowVisible(window_)) InvalidateRect(window_, nullptr, FALSE);
    }
    void updateFonts() {
        for (HFONT font : fonts_) if (font) DeleteObject(font);
        fonts_.clear();
        auto create = [&](int size, int weight) {
            HFONT hfont = CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, weight>=FW_SEMIBOLD?L"Segoe UI Semibold":L"Segoe UI");
            fonts_.push_back(hfont);
            return hfont;
        };
        font_ = create(14, FW_NORMAL);
        smallFont_ = create(12, FW_NORMAL);
        boldFont_ = create(14, FW_SEMIBOLD);
        // Segoe UI Semibold, 14pt-equivalent logical size, same DPI and weight
        // across Screens, Color temperature and DDC/CI Controls.
        sectionTitleFont_ = boldFont_;
        headlineFont_ = create(20, FW_NORMAL);
        tabDesiredWidths_.clear();
        const HWND controls[] = {btnRefresh_,btnScreenOff_,btnGamma_,editName_};
        for (HWND control : controls) {
            if (control) SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(font_), TRUE);
        }
        for (auto& row:hardwareRows_) {
            SendMessageW(row.nameButton,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
            for (HWND slider:{row.brightness,row.contrast})
                SendMessageW(slider,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        }
        for (HWND tab : tabs_) SendMessageW(tab, WM_SETFONT,
            reinterpret_cast<WPARAM>(font_), TRUE);
    }
    HWND createButton(int id, const wchar_t* text) {
        HWND control=CreateWindowExW(0,L"BUTTON",text,
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|BS_OWNERDRAW,0,0,1,1,window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        watchInteraction(control);
        return control;
    }
    void initControls() {
        dpi_ = GetDpiForWindow(window_);
        updateMotionPreference();
        ddc_.setWindow(window_);
        btnRefresh_=createButton(ID_REFRESH,L"");
        btnScreenOff_=createButton(ID_SCREEN_OFF,L"");
        btnGamma_ = createButton(ID_GAMMA, L"");
        ShowWindow(btnGamma_, gammaUnlocked_ ? SW_HIDE : SW_SHOW);
        gammaTooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
        TOOLINFOW gammaTool{sizeof(gammaTool)};
        gammaTool.uFlags=TTF_IDISHWND | TTF_SUBCLASS;
        gammaTool.hwnd=window_; gammaTool.uId=reinterpret_cast<UINT_PTR>(btnGamma_);
        gammaTool.lpszText=const_cast<wchar_t*>(L"Extended gamma range");
        SendMessageW(gammaTooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&gammaTool));
        TOOLINFOW screenTool{sizeof(screenTool)};
        screenTool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
        screenTool.hwnd=window_;
        screenTool.uId=reinterpret_cast<UINT_PTR>(btnScreenOff_);
        screenTool.lpszText=const_cast<wchar_t*>(L"Turn off screens");
        SendMessageW(gammaTooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&screenTool));
        SendMessageW(gammaTooltip_,TTM_SETMAXTIPWIDTH,0,px(430));
        editName_ = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | ES_LEFT | ES_MULTILINE | ES_AUTOHSCROLL,
            0,0,1,1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RENAME_EDIT)),
            instance_, nullptr);
        SendMessageW(editName_, EM_LIMITTEXT, 64, 0);
        SendMessageW(editName_,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(0,0));
        SetWindowSubclass(editName_,editFieldProc,1,reinterpret_cast<DWORD_PTR>(this));
        refreshTabs();
        updateFonts();
        updateLanguage();
        updateTheme();
        layout();
        // Saved per-monitor targets are authoritative at launch. Hardware
        // discovery and writes will run after stable physical enumeration.
        ensureHardwareRows();
        displayPowerNotification_=RegisterPowerSettingNotification(window_,&displayStateGuid,DEVICE_NOTIFY_WINDOW_HANDLE);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        trayReady_ = addTray();
        // Explorer can still be starting when a logon task launches this app.
        // TaskbarCreated is not sufficient if NIM_ADD failed the first time.
        if (!trayReady_) SetTimer(window_,TIMER_TRAY_RETRY,1000,nullptr);
        model_.setStatusWindow(window_);
        model_.collectGammaResults();
        // Run on every launch, not just after a Windows power notification.
        // This is essential when the logon task starts before DDC/CI is ready.
        restoreDdcAfterWake_=true;
        scheduleDisplayRecovery();
        updateTrayTooltip();
        // Native window controls render immediately; no WebView runtime.
    }
    void updateTheme() {
        // Refresh only on startup, explicit menu changes or Windows theme
        // notifications, not on each slider/painting frame.
        systemDark_=nightlight::systemAppsUseDarkTheme();
        refreshTrayIcon();
        const BOOL dark=nightlight::effectiveDarkTheme(
            model_.preferences().theme,systemDark_)?TRUE:FALSE;
        const Palette p = palette();
        DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark)); // immersive dark titlebar
        COLORREF caption = p.header;
        DwmSetWindowAttribute(window_, 35, &caption, sizeof(caption));
        COLORREF captionText = p.text;
        DwmSetWindowAttribute(window_, 36, &captionText, sizeof(captionText));
        const DWORD corners=2; // DWMWCP_ROUND; ignored gracefully on Windows 10.
        DwmSetWindowAttribute(window_,33,&corners,sizeof(corners));
        const COLORREF frameBorder=highContrast_?p.text:(dark?RGB(112,112,112):RGB(160,160,160));
        DwmSetWindowAttribute(window_,34,&frameBorder,sizeof(frameBorder));
        // Repaint the native non-client caption as well as app contents;
        // the standard title bar must retain the matching text colour.
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
        repaint();
        for (HWND h : tabs_) InvalidateRect(h, nullptr,FALSE);
        const HWND buttons[] = {btnRefresh_,btnScreenOff_,btnGamma_};
        for (HWND b : buttons) if (b) InvalidateRect(b, nullptr,FALSE);
        for (const auto& row:hardwareRows_) { InvalidateRect(row.nameButton,nullptr,FALSE); InvalidateRect(row.brightness,nullptr,FALSE); InvalidateRect(row.contrast,nullptr,FALSE); }
    }
    void updateLanguage() {
        for (size_t i=0;i<hardwareRows_.size();++i) {
            const auto& row=hardwareRows_[i];
            const auto suffix=L" · "+model_.monitors()[i].displayName();
            SetWindowTextW(row.brightness,(tr(L"Brightness",L"Brilho")+suffix).c_str());
            SetWindowTextW(row.contrast,(tr(L"Contrast",L"Contraste")+suffix).c_str());
        }
        SetWindowTextW(btnRefresh_,tr(L"Refresh displays",L"Atualizar ecrãs").c_str());
        screenOffHelp_=tr(
            L"Turn off screens (mouse and keyboard ignored for 3 seconds)",
            L"Desligar ecrãs (rato e teclado ignorados durante 3 segundos)");
        SetWindowTextW(btnScreenOff_,screenOffHelp_.c_str());
        TOOLINFOW screenTool{sizeof(screenTool)};
        screenTool.hwnd=window_;
        screenTool.uId=reinterpret_cast<UINT_PTR>(btnScreenOff_);
        screenTool.lpszText=screenOffHelp_.data();
        SendMessageW(gammaTooltip_,TTM_UPDATETIPTEXTW,0,
            reinterpret_cast<LPARAM>(&screenTool));
        gammaHelp_ = tr(
            L"Requests administrator permission to set HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM: GdiICMGammaRange = 256. Applies system-wide; restart Windows afterwards. Enables stronger independent monitor warmth without overlays.",
            L"Pede autorização de administrador para definir HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM: GdiICMGammaRange = 256. Aplica-se ao sistema; reinicie o Windows. Permite cores mais quentes por ecrã sem sobreposições.");
        TOOLINFOW gammaTool{sizeof(gammaTool)};
        gammaTool.hwnd=window_; gammaTool.uId=reinterpret_cast<UINT_PTR>(btnGamma_);
        gammaTool.lpszText=gammaHelp_.data();
        SendMessageW(gammaTooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&gammaTool));
        SetWindowTextW(btnGamma_, tr(L"Unlock warmer colors…", L"Desbloquear cores quentes…").c_str());
        updateTrayTooltip();
        // Localized labels can need more room, especially the Portuguese tray mode.
        layout();
        repaint();
    }
    void refreshTabs() {
        finishInlineRename(true);finishScreenDrag();
        for (HWND tab : tabs_) DestroyWindow(tab);
        tabs_.clear();
        armedTabSwitch_=nullptr;
        tabDesiredWidths_.clear(); // Refresh measured text if Windows renames a display.
        if (selected_ >= model_.monitors().size()) selected_ = 0;
        for (size_t index = 0; index < model_.monitors().size(); ++index) {
            HWND tab = createButton(ID_MONITOR_BASE + static_cast<int>(index), L"");
            tabs_.push_back(tab);
            if (font_) SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        ensureHardwareRows();
        layout();
    }
    void startInlineRename(size_t index) {
        if(index>=model_.monitors().size())return;
        finishInlineRename(true);
        finishReordering();
        selected_=index;editingDevice_=model_.monitors()[index].device;editingName_=true;
        SetWindowTextW(editName_,model_.monitors()[index].displayName().c_str());
        layout();
        SetWindowPos(editName_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        RedrawWindow(editName_,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
        SetFocus(editName_);
        SendMessageW(editName_,EM_SETSEL,0,-1);
        InvalidateRect(tabs_[index],nullptr,FALSE);
    }
    void finishInlineRename(bool save) {
        if(!editingName_)return;
        const auto device=editingDevice_;editingName_=false;editingDevice_.clear();
        wchar_t value[70]{};GetWindowTextW(editName_,value,70);
        ShowWindow(editName_,SW_HIDE);
        if(save)for(size_t i=0;i<model_.monitors().size();++i)if(model_.monitors()[i].device==device) {
            const std::wstring alias=value==model_.monitors()[i].windowsName?L"":value;
            model_.renameMonitor(i,alias);
            tabDesiredWidths_.erase(device); // Re-measure the label after editing.
            if(i<hardwareRows_.size()) {
                const auto suffix=L" · "+model_.monitors()[i].displayName();
                SetWindowTextW(hardwareRows_[i].brightness,(tr(L"Brightness",L"Brilho")+suffix).c_str());
                SetWindowTextW(hardwareRows_[i].contrast,(tr(L"Contrast",L"Contraste")+suffix).c_str());
            }
            break;
        }
        layout();repaint();
        for(HWND tab:tabs_)InvalidateRect(tab,nullptr,FALSE);
    }
    void select(size_t index) {
        if (index >= model_.monitors().size()) return;
        commitDrag();
        selected_ = index;
        finishInlineRename(true);
        repaint();
        for (HWND tab : tabs_) InvalidateRect(tab, nullptr,FALSE);
        for (const auto& row:hardwareRows_) InvalidateRect(row.nameButton,nullptr,FALSE);
    }
    void syncGammaButton() {
        const bool unlocked = nightlight::gammaRangeUnlocked();
        if (unlocked == gammaUnlocked_) return;
        gammaUnlocked_ = unlocked;
        ShowWindow(btnGamma_, unlocked ? SW_HIDE : SW_SHOW);
        layout();
    }
    int textWidth(const std::wstring& text, HFONT font=nullptr) const {
        HDC dc=GetDC(window_);
        HGDIOBJ old=SelectObject(dc,font?font:(font_?font_:GetStockObject(DEFAULT_GUI_FONT)));
        SIZE size{}; GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&size);
        SelectObject(dc,old); ReleaseDC(window_,dc);
        return size.cx;
    }
    int selectorWidth(HWND control) const {
        wchar_t label[256]{}; GetWindowTextW(control,label,256);
        return textWidth(label)+px(46);
    }
    nightlight::WindowContentMinimum minimumContentForWidth(int clientWidth) const {
        std::vector<int> tabWidths;
        tabWidths.reserve(model_.monitors().size());
        for (const auto& monitor:model_.monitors())
            tabWidths.push_back(std::max(px(108),
                textWidth(monitor.displayName(),boldFont_)+px(88)));
        // The exit animation still draws hardware while it is collapsing.
        const bool hardwareShown=ddcVisible_ ||
            (hardwareReveal_.active(uiTime()) && hardwareReveal_.value(uiTime())>0);
        return nightlight::calculateWindowContentMinimum(
            static_cast<int>(dpi_),clientWidth,tabWidths,
            hardwareRows_.size(),gammaUnlocked_,hardwareShown);
    }
    SIZE frameThickness() const {
        RECT frame{};
        const DWORD style=window_?static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)):
            (WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN);
        const DWORD exstyle=window_?static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_EXSTYLE)):0;
        AdjustWindowRectExForDpi(&frame,style,FALSE,exstyle,dpi_);
        return SIZE{frame.right-frame.left,frame.bottom-frame.top};
    }
    SIZE requiredWindowSizeForWidth(int outerWidth) const {
        const SIZE frame=frameThickness();
        const auto needed=minimumContentForWidth(std::max(1,outerWidth-static_cast<int>(frame.cx)));
        return SIZE{needed.clientWidth+frame.cx,needed.clientHeight+frame.cy+px(30)};
    }
    bool ensureWindowFitsContents() {
        if (!window_ || IsIconic(window_) || IsZoomed(window_)) return false;
        RECT rect{};
        if (!GetWindowRect(window_,&rect)) return false;
        const int originalWidth=rect.right-rect.left;
        const int originalHeight=rect.bottom-rect.top;
        const SIZE needed=requiredWindowSizeForWidth(originalWidth);
        const int newWidth=std::max(originalWidth,static_cast<int>(needed.cx));
        // The minimum height depends on the eventual width because screen
        // tabs can wrap to additional rows.
        const SIZE finalNeeded=requiredWindowSizeForWidth(newWidth);
        const int newHeight=std::max(originalHeight,static_cast<int>(finalNeeded.cy));
        if (newWidth==originalWidth && newHeight==originalHeight) return false;
        int x=rect.left,y=rect.top;
        MONITORINFO screen{sizeof(screen)};
        if (GetMonitorInfoW(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST),&screen)) {
            const RECT& work=screen.rcWork;
            x=std::clamp(x,static_cast<int>(work.left),
                std::max(static_cast<int>(work.left),static_cast<int>(work.right)-newWidth));
            y=std::clamp(y,static_cast<int>(work.top),
                std::max(static_cast<int>(work.top),static_cast<int>(work.bottom)-newHeight));
        }
        SetWindowPos(window_,nullptr,x,y,newWidth,newHeight,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
        return true;
    }
    void layout() {
        if (!window_ || layingOut_) return;
        if (!layoutDepth_) layoutTime_=uiTime();
        struct LayoutScope { unsigned& depth; ~LayoutScope() { --depth; } } scope{layoutDepth_};
        ++layoutDepth_;
        layingOut_=true;
        RECT client{}; GetClientRect(window_,&client);
        width_=std::max(1L,client.right); height_=std::max(1L,client.bottom);
        struct Placement { HWND control; int x,y,w,h; };
        std::vector<Placement> placements;
        auto place=[&](HWND control,int x,int y,int w,int h,BOOL) {
            if(!control)return;
            if(!hardwareRow(control) && !hardwareNameButton(control)) {
                auto& motion=reorderPositions_[control];
                motion.x.to(x,layoutTime_,180,reorderInProgress_ && animations_ && IsWindowVisible(window_));
                motion.y.to(y,layoutTime_,180,reorderInProgress_ && animations_ && IsWindowVisible(window_));
                x=static_cast<int>(std::round(motion.x.value(layoutTime_)));
                y=static_cast<int>(std::round(motion.y.value(layoutTime_)));
            }
            placements.push_back({control,x,y,std::max(1,w),std::max(1,h)});
        };
        const double hardwareProgress=hardwareReveal_.value(layoutTime_);
        composedTransition_=IsWindowVisible(window_) && (hardwareReveal_.active(layoutTime_) || reorderInProgress_);
        const int margin=px(nightlight::kCardOuterMarginDip);
        const int inset=px(nightlight::kCardInnerInsetDip);
        panelTop_=px(nightlight::kCardsTopMarginDip);
        const int innerLeft=margin+inset,innerRight=width_-margin-inset;
        const int tabH=px(32),tabGap=px(6);
        const int tabsRight=innerRight;
        // Exactly 4px from the top and right edges of the Screens card.
        place(btnRefresh_,width_-margin-px(4)-px(32),panelTop_+px(4),px(32),tabH,TRUE);
        const int tabStart=innerLeft;
        int tabX=tabStart,tabY=panelTop_+px(40),rows=1;
        for (size_t i=0;i<tabs_.size();++i) {
            const auto& m=model_.monitors()[i];
            auto stored=tabDesiredWidths_.find(m.device);
            if(stored==tabDesiredWidths_.end())stored=tabDesiredWidths_.emplace(
                m.device,std::max(px(108),textWidth(m.displayName(),boldFont_)+px(88))).first;
            // On very narrow windows, ellipsize the name rather than
            // enlarging the entire window beyond the user's chosen width.
            const int tabW=std::min(stored->second,std::max(px(1),tabsRight-tabStart));
            if (tabX>tabStart && tabX+tabW>tabsRight) { tabX=tabStart; tabY+=px(38); ++rows; }
            place(tabs_[i],tabX,tabY,tabW,tabH,TRUE);
            if(editingName_ && m.device==editingDevice_) {
                // Rename occupies only the text zone: monitor icon and
                // individual toggle stay visible and usable.
                place(editName_,tabX+px(32),tabY+px(3),
                    std::max(px(24),tabW-px(86)),tabH-px(6),TRUE);
            }
            tabX+=tabW+tabGap;
        }
        // Screen-off button belongs to the same row as the monitor tabs,
        // immediately after the last one. On narrow windows it wraps with
        // the tabs instead of overlapping a name or its on/off toggle.
        const int offSize=px(nightlight::kScreenOffButtonSizeDip);
        if (tabX>tabStart && tabX+offSize>tabsRight) {
            tabX=tabStart;
            tabY+=px(38);
            ++rows;
        }
        // Keep the smaller square button vertically centred on the tabs.
        place(btnScreenOff_,tabX,tabY+(tabH-offSize)/2,
            offSize,offSize,TRUE);
        EnableWindow(btnScreenOff_,!tabs_.empty());
        panelHeight_=px(46+rows*38);
        secondPanelTop_=panelTop_+panelHeight_+px(nightlight::kCardsGapDip);
        const bool narrowTemperature=width_<px(350);
        const int temperatureShift=narrowTemperature?
            px(nightlight::kTemperatureNarrowExtraDip):0;
        temperatureHeight_=px(gammaUnlocked_?
            nightlight::kTemperatureUnlockedHeightDip:
            nightlight::kTemperatureLockedHeightDip)+temperatureShift;
        const int gammaWidth=std::min(px(220),std::max(px(1),innerRight-innerLeft));
        place(btnGamma_,innerRight-gammaWidth,
            secondPanelTop_+px(nightlight::kTemperatureGammaTopDip)+temperatureShift,
            gammaWidth,px(29),TRUE);
        // No fixed maximum: the temperature gradient extends or contracts
        // with the window, keeping the same 40-DIP side inset.
        const int sliderW=nightlight::temperatureSliderWidth(static_cast<int>(dpi_),width_);
        const int sliderLeft=(width_-sliderW)/2;
        slider_={sliderLeft,
            secondPanelTop_+px(nightlight::kTemperatureSliderTopDip)+temperatureShift,
            sliderLeft+sliderW,
            secondPanelTop_+px(nightlight::kTemperatureSliderTopDip+4)+temperatureShift};
        hardwareTop_=secondPanelTop_+temperatureHeight_+px(nightlight::kCardsGapDip);
        hardwareHeight_=px(hardwareRows_.empty()?72:
            nightlight::kDdcSectionBaseDip+
                nightlight::kDdcRowStepDip*static_cast<int>(hardwareRows_.size()));
        for (size_t i=0;i<hardwareRows_.size();++i) {
            auto& row=hardwareRows_[i];
            row.topMotion.to(hardwareTop_+px(nightlight::kDdcFirstRowTopDip+
                nightlight::kDdcRowStepDip*static_cast<int>(i)),
                layoutTime_,180,
                reorderInProgress_ && animations_ && IsWindowVisible(window_));
            row.top=static_cast<int>(std::round(row.topMotion.value(layoutTime_)));
            const auto& name=model_.monitors()[i].displayName();
            const int nameWidth=std::max(px(44),std::min({
                // Icon begins 10px from the left; leave the same 10px
                // after the measured name on the right (32px icon/text
                // offset + 10px trailing margin = 42px total).
                textWidth(name,boldFont_)+px(42),px(279),innerRight-innerLeft}));
            place(row.nameButton,innerLeft,row.top,nameWidth,tabH,TRUE);
            // One full-width pair of sliders per monitor, beneath its name.
            const int sliderLeft=innerLeft+px(nightlight::kDdcSliderLeftInsetDip);
            // The native trackbar reserves its own thumb inset. Extend the
            // HWND close enough to the percentage to remove the large gap,
            // while leaving room for a full 100 % reading.
            const int percentTextWidth=std::max(px(1),
                textWidth(L"100 %",smallFont_));
            const int sliderWidth=nightlight::ddcSliderWidth(
                static_cast<int>(dpi_),width_,percentTextWidth);
            place(row.brightness,sliderLeft,row.top+px(36),sliderWidth,px(28),TRUE);
            place(row.contrast,sliderLeft,row.top+px(66),sliderWidth,px(28),TRUE);
        }
        hardwareVisibleHeight_=static_cast<int>(std::round(hardwareHeight_*hardwareProgress));
        contentHeight_=secondPanelTop_+temperatureHeight_+margin+
            static_cast<int>(std::round(
                (hardwareHeight_+px(nightlight::kCardsGapDip))*hardwareProgress));
        // Never scroll the window: grow only when a content change increases
        // its required minimum size. Do not automatically shrink it again.
        if (!initializingWindow_ && ensureWindowFitsContents()) {
            layingOut_=false; layout(); return;
        }
        // Do not let MoveWindow copy old pixels while its siblings still occupy
        // their previous coordinates. All children move before any can repaint.
        const UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW;
        auto visibility=[&](HWND control) -> UINT {
            if (control==btnGamma_) return gammaUnlocked_?SWP_HIDEWINDOW:SWP_SHOWWINDOW;
            if(control==editName_)return editingName_?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
            if (hardwareRow(control) || hardwareNameButton(control)) return hardwareProgress>0?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
            return 0;
        };
        HDWP batch=BeginDeferWindowPos(static_cast<int>(placements.size()));
        for (const auto& placement:placements) {
            if (!batch) break;
            batch=DeferWindowPos(batch,placement.control,nullptr,placement.x,placement.y,placement.w,placement.h,flags|visibility(placement.control));
        }
        const bool placed=batch && EndDeferWindowPos(batch);
        if (!placed) for (const auto& placement:placements)
            SetWindowPos(placement.control,nullptr,placement.x,placement.y,placement.w,placement.h,flags|visibility(placement.control));
        if (editingName_) {
            // EM_SETRECT works on multiline EDIT controls; constrain ours to
            // a single left-aligned line at the font's vertical centre.
            RECT bounds{}; GetClientRect(editName_,&bounds);
            TEXTMETRICW metrics{};
            if (HDC dc=GetDC(editName_)) {
                HGDIOBJ old=SelectObject(dc,font_);
                GetTextMetricsW(dc,&metrics);
                SelectObject(dc,old);
                ReleaseDC(editName_,dc);
            }
            const int lineH=metrics.tmHeight>0?metrics.tmHeight:px(17);
            const int lineTop=std::max(0,(static_cast<int>(bounds.bottom)-lineH)/2);
            const RECT textRect{0,lineTop,
                std::max(px(1),static_cast<int>(bounds.right)),
                std::min(static_cast<int>(bounds.bottom),lineTop+lineH)};
            SendMessageW(editName_,EM_SETRECT,0,reinterpret_cast<LPARAM>(&textRect));
        }
        // Clip native child controls to the sliding panel. No control can draw
        // into a neighboring card while its section is still being revealed.
        for (const auto& placement:placements) {
            RECT clip{}; bool clipped=false;
            if (hardwareRow(placement.control) || hardwareNameButton(placement.control)) {
                clip={0,hardwareTop_,width_,hardwareTop_+hardwareVisibleHeight_}; clipped=hardwareProgress<1;
            }
            if (placement.control==editName_ && editingName_) {
                SetWindowRgn(placement.control,nullptr,FALSE);
            } else if (composedTransition_ && placement.control!=editName_) {
                HRGN empty=CreateRectRgn(0,0,0,0);
                if (!SetWindowRgn(placement.control,empty,FALSE)) DeleteObject(empty);
            } else if (clipped) {
                RECT local{std::clamp(static_cast<int>(clip.left)-placement.x,0,placement.w),std::clamp(static_cast<int>(clip.top)-placement.y,0,placement.h),
                    std::clamp(static_cast<int>(clip.right)-placement.x,0,placement.w),std::clamp(static_cast<int>(clip.bottom)-placement.y,0,placement.h)};
                HRGN region=CreateRectRgn(local.left,local.top,std::max(local.left,local.right),std::max(local.top,local.bottom));
                if (!SetWindowRgn(placement.control,region,FALSE)) DeleteObject(region);
            } else SetWindowRgn(placement.control,nullptr,FALSE);
        }
        layingOut_=false;
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME|RDW_NOERASE);
    }
    void drawTextAt(HDC dc, const std::wstring& value, RECT rect,
                    HFONT font, COLORREF foreground, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
        const auto old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);
        DrawTextW(dc, value.c_str(), -1, &rect, format | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    void drawSectionIcon(HDC dc,int kind,int x,int y,COLORREF color) {
        Gdiplus::Graphics g(dc); nightlight::configureIconGraphics(g);
        g.TranslateTransform(static_cast<float>(x),static_cast<float>(y));
        const float scale=static_cast<float>(dpi_)/96; g.ScaleTransform(scale,scale);
        Gdiplus::Pen pen(drawingColor(color),1.3f);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        if (kind==0) {
            g.DrawRectangle(&pen,1.0f,2.0f,14.0f,10.0f);
            g.DrawLine(&pen,8.0f,12.0f,8.0f,15.0f); g.DrawLine(&pen,5.0f,15.0f,11.0f,15.0f);
        } else if (kind==1) {
            g.DrawEllipse(&pen,5.0f,5.0f,6.0f,6.0f);
            for (int i=0;i<8;++i) {
                const double angle=i*3.141592653589793/4;
                g.DrawLine(&pen,static_cast<float>(8+6*std::cos(angle)),static_cast<float>(8+6*std::sin(angle)),
                    static_cast<float>(8+8*std::cos(angle)),static_cast<float>(8+8*std::sin(angle)));
            }
        } else {
            g.DrawLine(&pen,1.0f,5.0f,15.0f,5.0f); g.DrawLine(&pen,1.0f,12.0f,15.0f,12.0f);
            Gdiplus::SolidBrush brush(drawingColor(palette().card));
            g.FillEllipse(&brush,4.0f,2.5f,5.0f,5.0f); g.DrawEllipse(&pen,4.0f,2.5f,5.0f,5.0f);
            g.FillEllipse(&brush,9.0f,9.5f,5.0f,5.0f); g.DrawEllipse(&pen,9.0f,9.5f,5.0f,5.0f);
        }
    }
    void drawSliderThumb(HDC dc,float cx,float cy,double hover,double pressed,bool enabled) {
        const auto p=palette();
        const bool dark=nightlight::effectiveDarkTheme(
            model_.preferences().theme,systemDark_);
        const float radius=static_cast<float>(px(10));
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        if (!highContrast_) {
            Gdiplus::SolidBrush shadow(Gdiplus::Color(30,0,0,0));
            g.FillEllipse(&shadow,cx-radius,cy-radius+px(1),radius*2,radius*2);
        }
        const auto shell=highContrast_?p.card:(dark?RGB(69,69,69):RGB(255,255,255));
        const auto edge=highContrast_?p.text:(dark?RGB(92,92,92):RGB(214,214,214));
        Gdiplus::SolidBrush body(drawingColor(shell));
        Gdiplus::Pen border(drawingColor(edge),static_cast<float>(dpi_)/96);
        g.FillEllipse(&body,cx-radius,cy-radius,radius*2,radius*2);
        g.DrawEllipse(&border,cx-radius,cy-radius,radius*2,radius*2);
        const float dot=static_cast<float>(px(5)+px(1)*hover-px(1)*pressed);
        Gdiplus::SolidBrush accent(drawingColor(enabled?p.accent:p.muted));
        g.FillEllipse(&accent,cx-dot,cy-dot,dot*2,dot*2);
    }
    void drawSlider(HDC dc) {
        const bool enabled = hasMonitor() && selectedMonitor()->enabled;
        const Palette p=palette();
        const COLORREF stops[] = {
            RGB(237,103,76), RGB(248,167,99), RGB(255,224,161),
            RGB(220,235,245), RGB(239,243,246)
        };
        const int span=std::max(1,static_cast<int>(slider_.right-slider_.left));
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const float x=static_cast<float>(slider_.left),y=static_cast<float>(slider_.top);
        const float w=static_cast<float>(span),h=static_cast<float>(slider_.bottom-slider_.top);
        Gdiplus::GraphicsPath path;
        path.AddArc(x,y,h,h,180,90); path.AddArc(x+w-h,y,h,h,270,90);
        path.AddArc(x+w-h,y,h,h,0,90); path.AddArc(x,y,h,h,90,90); path.CloseFigure();
        if (highContrast_) {
            Gdiplus::SolidBrush brush(drawingColor(p.accent)); graphics.FillPath(&brush,&path);
        } else {
            Gdiplus::Color gradient[5]; const float positions[]{0,0.25f,0.5f,0.75f,1};
            for (int i=0;i<5;++i) gradient[i]=drawingColor(enabled?stops[i]:mix(stops[i],p.card,0.55));
            Gdiplus::LinearGradientBrush brush(Gdiplus::PointF(x,y),Gdiplus::PointF(x+w,y),gradient[0],gradient[4]);
            brush.SetInterpolationColors(gradient,positions,5); graphics.FillPath(&brush,&path);
        }
        const int current=hasMonitor()?selectedMonitor()->kelvin:5000;
        if (dragging_) temperaturePosition_.snap(current);
        else animate(temperaturePosition_,current,160);
        animate(temperatureEngagement_,dragging_?1.0:(temperatureHover_?0.35:0.0),120);
        const int cx=nightlight::temperatureSliderXFromKelvin(
            static_cast<int>(std::lround(temperaturePosition_.value(uiTime()))),
            slider_.left,slider_.right);
        const int cy=(slider_.top+slider_.bottom)/2;
        const double engagement=temperatureEngagement_.value(uiTime());
        drawSliderThumb(dc,static_cast<float>(cx),static_cast<float>(cy),temperatureHover_?engagement:0,
            dragging_?engagement:0,enabled);
    }
    void drawTransitionControls(HDC dc) {
        // Native HWND regions are temporarily empty, preserving focus/state
        // while every visual is painted into the same parent frame bitmap.
        for (HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            if (!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) continue;
            RECT rect{}; GetWindowRect(child,&rect);
            MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&rect),2);
            const int saved=SaveDC(dc);
            SetViewportOrgEx(dc,rect.left,rect.top,nullptr);
            IntersectClipRect(dc,0,0,rect.right-rect.left,rect.bottom-rect.top);
            if (hardwareRow(child)) {
                IntersectClipRect(dc,0,hardwareTop_-rect.top,rect.right-rect.left,
                    hardwareTop_+hardwareVisibleHeight_-rect.top);
                NMCUSTOMDRAW draw{}; draw.hdr.hwndFrom=child; draw.dwDrawStage=CDDS_PREPAINT; draw.hdc=dc;
                drawHardwareSlider(draw);
            } else if (child==editName_) {
                SendMessageW(child,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
            } else {
                DRAWITEMSTRUCT draw{}; draw.CtlType=ODT_BUTTON;draw.CtlID=GetDlgCtrlID(child);draw.hwndItem=child;draw.hDC=dc;
                GetClientRect(child,&draw.rcItem);
                const LRESULT state=SendMessageW(child,BM_GETSTATE,0,0);
                if (state&BST_PUSHED) draw.itemState|=ODS_SELECTED;
                if (GetFocus()==child) draw.itemState|=ODS_FOCUS;
                if (!IsWindowEnabled(child)) draw.itemState|=ODS_DISABLED;
                if (SendMessageW(child,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS) draw.itemState|=ODS_NOFOCUSRECT;
                drawButton(draw);
            }
            RestoreDC(dc,saved);
        }
    }
    void paintClient(HDC hdc,RECT client) {
        if (client.right<=0 || client.bottom<=0) return;
        HDC mem=pageSurface_.ensure(hdc,client.right,client.bottom);
        if (!mem) return;
        const Palette p=palette();
        fill(mem,client,p.page);
        SetViewportOrgEx(mem,0,0,nullptr);
        const int cardMargin=px(nightlight::kCardOuterMarginDip);
        const RECT card1{cardMargin,panelTop_,client.right-cardMargin,panelTop_+panelHeight_};
        rounded(mem,card1,px(2*nightlight::kCardCornerRadiusDip),p.card,p.border);
        const RECT card2{cardMargin,secondPanelTop_,client.right-cardMargin,secondPanelTop_+temperatureHeight_};
        rounded(mem,card2,px(2*nightlight::kCardCornerRadiusDip),p.card,p.border);
        const int innerLeft=cardMargin+px(nightlight::kCardInnerInsetDip);
        const int innerRight=client.right-innerLeft;
        drawSectionIcon(mem,0,innerLeft,panelTop_+px(11),p.subtle);
        RECT screensTitle{innerLeft+px(24),panelTop_+px(8),
            innerRight-px(48),panelTop_+px(30)};
        drawTextAt(mem,tr(L"Screens",L"Ecrãs"),screensTitle,sectionTitleFont_,p.text);
        drawSectionIcon(mem,1,innerLeft,secondPanelTop_+px(11),p.subtle);
        const bool narrowTemperature=width_<px(350);
        const int temperatureShift=narrowTemperature?
            px(nightlight::kTemperatureNarrowExtraDip):0;
        const int current=hasMonitor()?selectedMonitor()->kelvin:5000;
        const std::wstring temperatureText=formatNumber(current)+L" K";
        // Both headings occupy the same 24-DIP baseline on normal widths.
        // On narrow windows the Kelvin value uses the next line to avoid
        // colliding with the section title.
        const int valueReserve=std::max(px(88),textWidth(temperatureText,headlineFont_)+px(8));
        RECT label{innerLeft+px(24),secondPanelTop_+px(8),
            narrowTemperature?innerRight:innerRight-valueReserve,
            secondPanelTop_+px(32)};
        drawTextAt(mem,tr(L"Color temperature",L"Temperatura de cor"),
            label,sectionTitleFont_,p.text);
        // Use exactly the same right edge as the temperature slider:
        // the final K stays vertically aligned with the 6500 K endpoint.
        const int valueTop=secondPanelTop_+
            (narrowTemperature?px(32):px(8));
        RECT valueRect=nightlight::temperatureValueRect(slider_,
            valueReserve,valueTop,px(24));
        drawTextAt(mem,temperatureText,valueRect,headlineFont_,p.accent,
            DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        drawSlider(mem);
        const int footerY=secondPanelTop_+
            px(nightlight::kTemperatureScaleTopDip)+temperatureShift;
        // Use the slider's actual horizontal endpoints instead of card
        // padding, so the first and last scale labels stay precisely aligned
        // as the user changes the window width.
        const auto scaleLabels=nightlight::temperatureScaleLabelRects(
            slider_,footerY,px(18));
        drawTextAt(mem,L"1000 K",scaleLabels.low,smallFont_,p.muted,
            DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        drawTextAt(mem,L"6500 K",scaleLabels.high,smallFont_,p.muted,
            DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

        if (hardwareVisibleHeight_>0) {
            const int saved=SaveDC(mem);
            IntersectClipRect(mem,0,hardwareTop_,width_,hardwareTop_+hardwareVisibleHeight_);
            drawHardware(mem); RestoreDC(mem,saved);
        }
        if (composedTransition_) drawTransitionControls(mem);
        // Borderless footer painted on the page background: no divider.
        // The minimum window height reserves a 30-DIP strip for this label.
        RECT statusRect{px(12),std::max(0L,client.bottom-px(30)),
            std::max(static_cast<LONG>(px(12)),client.right-px(12)),client.bottom};
        const std::wstring version=L"NightLight v1.0.6";
        const int versionWidth=textWidth(version,smallFont_)+px(4);
        RECT backendRect=statusRect;
        backendRect.right=std::max(backendRect.left,statusRect.right-versionWidth-px(10));
        if (const auto* mon=selectedMonitor()) {
            const auto engine=mon->nvidiaTargetColor?L"NV target":mon->hardwareColor?L"DDC/CI":L"GDI gamma";
            std::wstring state=engine;
            COLORREF indicator=p.muted;
            const bool dark=nightlight::effectiveDarkTheme(model_.preferences().theme,systemDark_);
            if (mon->gammaStatus<0) {
                state+=tr(L" · Checking",L" · A verificar");
            } else if (mon->gammaStatus==0) {
                state+=tr(L" · Failed",L" · Falhou");
                indicator=dark?RGB(255,142,142):RGB(160,35,35);
            } else if (!mon->enabled || (mon->nvidiaTargetColor && mon->kelvin>=6500)) {
                state+=tr(L" · Inactive",L" · Inativo");
            } else if (mon->nvidiaTargetColor) {
                state+=tr(L" · Accepted",L" · Aceite");
            } else {
                state+=tr(L" · Applied",L" · Aplicado");
            }
            if(highContrast_) indicator=p.text;
            if(backendRect.right>backendRect.left)
                drawTextAt(mem,state,backendRect,smallFont_,indicator,
                    DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }
        statusRect.left=std::max(statusRect.left,statusRect.right-versionWidth);
        drawTextAt(mem,version,statusRect,smallFont_,p.muted,
            DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        SetViewportOrgEx(mem,0,0,nullptr);
        BitBlt(hdc,0,0,client.right,client.bottom,mem,0,0,SRCCOPY);
    }
    void paint() {
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(window_,&ps);
        RECT client{}; GetClientRect(window_,&client);
        paintClient(dc,client);
        EndPaint(window_,&ps);
    }
    bool hardwareNameButton(HWND control) const {
        for (const auto& row:hardwareRows_) if (row.nameButton==control) return true;
        return false;
    }
    HardwareRow* hardwareRow(HWND slider) {
        for (auto& row:hardwareRows_) if (row.brightness==slider || row.contrast==slider) return &row;
        return nullptr;
    }
    void ensureHardwareRows() {
        const auto& monitors=model_.monitors();
        bool same=hardwareRows_.size()==monitors.size();
        for (size_t i=0;same && i<monitors.size();++i)
            same=hardwareRows_[i].device==monitors[i].device &&
                hardwareRows_[i].physicalKey==nightlight::monitorProfileKey(monitors[i]);
        if (same) return;
        for (auto& row:hardwareRows_) { DestroyWindow(row.nameButton); DestroyWindow(row.brightness); DestroyWindow(row.contrast); }
        hardwareRows_.clear();
        for (size_t i=0;i<monitors.size();++i) {
            HardwareRow row;
            row.device=monitors[i].device;
            row.physicalKey=nightlight::monitorProfileKey(monitors[i]);
            row.nameButton=createButton(ID_DDC_MONITOR_BASE+static_cast<int>(i),L"");
            if (font_) SendMessageW(row.nameButton,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
            auto create=[&](int offset,const std::wstring& name) {
                HWND slider=CreateWindowExW(0,TRACKBAR_CLASSW,name.c_str(),WS_CHILD|WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,
                    0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DDC_SLIDER_BASE+i*2+offset)),instance_,nullptr);
                // Fine pointer positions keep the thumb moving at pixel precision;
                // monitor commands are still rounded to integer percentages.
                SendMessageW(slider,TBM_SETRANGE,TRUE,MAKELPARAM(0,10000));
                SendMessageW(slider,TBM_SETLINESIZE,0,100);
                SendMessageW(slider,TBM_SETPAGESIZE,0,1000);
                SendMessageW(slider,TBM_SETPOS,TRUE,model_.hardwarePercent(row.device,offset==1)*100);
                SendMessageW(slider,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
                // A custom WM_PAINT subclass replaces the native themed
                // trackbar paint entirely; its input/range logic remains.
                watchInteraction(slider); EnableWindow(slider,FALSE);
                return slider;
            };
            row.brightness=create(0,tr(L"Brightness",L"Brilho")+L" · "+monitors[i].displayName());
            row.contrast=create(1,tr(L"Contrast",L"Contraste")+L" · "+monitors[i].displayName());
            ddc_.seed(row.device,model_.hardwarePercent(row.device,false),model_.hardwarePercent(row.device,true),
                model_.hasHardwarePercent(row.device,false),model_.hasHardwarePercent(row.device,true));
            hardwareRows_.push_back(row);
        }
    }
    void refreshHardware() {
        if(!displayAwake_ || (displayRecoveryActive_ && recoveryPass_==0)) return;
        hardwareDiscovered_=true;
        ensureHardwareRows();
        for (const auto& row:hardwareRows_) ddc_.refresh(row.device);
        syncHardware();
    }
    void syncColorWatchdog() {
        const bool needed=displayAwake_ && !displayRecoveryActive_ && model_.needsColorWatchdog();
        if(needed==colorWatchdogRunning_) return;
        if(needed) colorWatchdogRunning_=SetTimer(window_,TIMER_VERIFY_COLOR,3000,nullptr)!=0;
        else { KillTimer(window_,TIMER_VERIFY_COLOR); colorWatchdogRunning_=false; }
    }
    void syncHardwarePoll() {
        const bool needed=ddcVisible_ && displayAwake_ && !displayRecoveryActive_ &&
            IsWindowVisible(window_) && !IsIconic(window_);
        if(needed==hardwarePollRunning_) return;
        if(needed) hardwarePollRunning_=SetTimer(window_,TIMER_READ_DDC,nightlight::kDdcReadbackIntervalMs,nullptr)!=0;
        else { KillTimer(window_,TIMER_READ_DDC); hardwarePollRunning_=false; }
    }
    void pollHardware() {
        // Real DDC/CI and WMI readings, not saved profile values, set the
        // displayed brightness/contrast. Keep all slow monitor I/O off the
        // UI thread, and do not query sleeping or hidden displays.
        if (!ddcVisible_ || !displayAwake_ || displayRecoveryActive_ ||
            screenOffGuard_.guarding() ||
            !IsWindowVisible(window_) || IsIconic(window_)) return;
        for (const auto& row:hardwareRows_) {
            if (GetCapture()==row.brightness || GetCapture()==row.contrast) continue;
            ddc_.poll(row.device);
        }
    }
    void scheduleDisplayRecovery() {
        if (!displayRecoveryActive_) {
            recoveryPass_=0;
            recoverySettleChecks_=0;
            recoveryVerificationIssued_=false;
            // Commit any active rename/drag while the old device mapping is
            // still intact. Otherwise an in-flight rename could be saved to
            // the wrong physical panel after Windows swaps DISPLAYx numbers.
            finishInlineRename(true);
            commitDrag();
            endHardwareDrag();
            finishScreenDrag();
            // A DISPLAYx identifier may now address another physical panel.
            // Discard any in-flight DDC observations before re-detection.
            ddc_.invalidateTopology();
            model_.beginDisplayRecovery();
        }
        displayRecoveryActive_=true;
        syncColorWatchdog(); syncHardwarePoll();
        if (displayAwake_) SetTimer(window_,TIMER_DISPLAYS,750,nullptr);
    }
    void syncHardware() {
        for (auto& row:hardwareRows_) {
            const auto state=ddc_.state(row.device);
            auto sync=[&](HWND slider,bool contrast,const nightlight::DdcValue& value) {
                const bool enabled=ddcVisible_ && displayAwake_ && !displayRecoveryActive_ && !state.loading && value.supported;
                if((IsWindowEnabled(slider)!=FALSE)!=enabled) EnableWindow(slider,enabled);
                const bool captured=GetCapture()==slider;
                if (!captured) {
                    const int current=static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0));
                    if ((current+50)/100!=value.percent) SendMessageW(slider,TBM_SETPOS,TRUE,value.percent*100);
                }
                // A display wake may temporarily report default/other-screen
                // DDC values. Do not overwrite the saved last good targets.
                if (displayAwake_ && !displayRecoveryActive_ && value.managed && !captured &&
                    model_.rememberHardwarePercent(row.device,contrast,value.percent))
                    SetTimer(window_,TIMER_SAVE_DDC,400,nullptr);
                InvalidateRect(slider,nullptr,FALSE);
            };
            sync(row.brightness,false,state.brightness); sync(row.contrast,true,state.contrast);
        }
    }
    void toggleHardware() {
        ddcVisible_=!ddcVisible_;
        animate(hardwareReveal_,ddcVisible_?1:0,240);
        if (ddcVisible_ && IsWindowVisible(window_)) refreshHardware();
        else { hardwareDiscovered_=false; syncHardware(); }
        syncHardwarePoll();
        layout();
        repaint();
    }
    void drawCenteredDdcGlyph(HDC dc,const wchar_t* glyph,POINT center,
        int sizeDip,COLORREF foreground) {
        // Draw the actual *ink* of the glyph about the slider track's
        // centre, not the font's line box (whose asymmetric bearing made
        // brightness and contrast appear vertically offset).
        Gdiplus::FontFamily fluent(L"Segoe Fluent Icons");
        Gdiplus::FontFamily fallback(L"Segoe MDL2 Assets");
        const Gdiplus::FontFamily* family=fluent.IsAvailable()?&fluent:&fallback;
        Gdiplus::GraphicsPath path;
        const Gdiplus::StringFormat* typography=
            Gdiplus::StringFormat::GenericTypographic();
        if (path.AddString(glyph,1,family,Gdiplus::FontStyleRegular,
                static_cast<Gdiplus::REAL>(px(sizeDip)),Gdiplus::PointF(0,0),
                typography)!=Gdiplus::Ok) return;
        Gdiplus::RectF ink{};
        if (path.GetBounds(&ink)!=Gdiplus::Ok || ink.Width<=0 || ink.Height<=0)
            return;
        Gdiplus::Matrix centerInk;
        centerInk.Translate(-ink.X-ink.Width/2.0f,-ink.Y-ink.Height/2.0f);
        path.Transform(&centerInk);
        Gdiplus::Graphics g(dc);
        nightlight::configureIconGraphics(g);
        g.TranslateTransform(static_cast<Gdiplus::REAL>(center.x),
            static_cast<Gdiplus::REAL>(center.y));
        Gdiplus::SolidBrush brush(drawingColor(foreground));
        g.FillPath(&brush,&path);
    }
    void drawHardware(HDC dc) {
        const Palette p=palette();
        const int cardMargin=px(nightlight::kCardOuterMarginDip);
        RECT card{cardMargin,hardwareTop_,width_-cardMargin,hardwareTop_+hardwareHeight_};
        rounded(dc,card,px(2*nightlight::kCardCornerRadiusDip),p.card,p.border);
        const int left=cardMargin+px(nightlight::kCardInnerInsetDip);
        const int right=width_-left;
        drawSectionIcon(dc,2,left,hardwareTop_+px(13),p.subtle);
        RECT heading{left+px(24),hardwareTop_+px(10),right,hardwareTop_+px(32)};
        drawTextAt(dc,tr(L"DDC/CI Controls",L"Controlos DDC/CI"),heading,sectionTitleFont_,p.text);
        if (hardwareRows_.empty()) {
            RECT empty{left,hardwareTop_+px(40),right,hardwareTop_+px(62)};
            drawTextAt(dc,tr(L"No connected displays",L"Nenhum ecrã ligado"),empty,font_,p.muted);
        }
        for (size_t i=0;i<hardwareRows_.size();++i) {
            const auto& row=hardwareRows_[i];
            const auto state=ddc_.state(row.device);
            // Centre the actual visible Fluent glyph with the middle of
            // each 28-DIP slider, and the percentage in that same row.
            // Contrast intentionally remains smaller (15 vs. 18 DIP).
            auto drawChannel=[&](HWND slider,int y,bool contrast) {
                const POINT iconCenter=nightlight::ddcSliderIconCenter(
                    left+px(16),y,px(28));
                drawCenteredDdcGlyph(dc,contrast?L"\uE7A1":L"\uE706",
                    iconCenter,contrast?15:18,p.subtle);
                // Match the exact right edge of the temperature gradient
                // and its 6500 K label, even with odd window widths.
                const int percentTextWidth=std::max(px(1),
                    textWidth(L"100 %",smallFont_));
                const RECT number=nightlight::ddcPercentageRect(
                    static_cast<int>(dpi_),width_,percentTextWidth,y,px(28));
                const int percent=GetCapture()==slider?
                    (static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0))+50)/100:
                    model_.hardwarePercent(row.device,contrast);
                drawTextAt(dc,formatNumber(percent)+L" %",number,
                    smallFont_,p.subtle,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            };
            drawChannel(row.brightness,row.top+px(36),false);
            drawChannel(row.contrast,row.top+px(66),true);
            std::wstring message;
            if (state.brightness.failed || state.contrast.failed)
                message=tr(L"The monitor did not accept the adjustment. Try again.",
                    L"O monitor não aceitou o ajuste. Tente novamente.");
            // Report DDC errors alongside the monitor name rather than
            // reserving a permanently empty footer below the sliders.
            if (!message.empty()) {
                const int nameWidth=std::max(px(44),std::min({
                    textWidth(model_.monitors()[i].displayName(),boldFont_)+px(42),
                    px(279),right-left}));
                const int statusLeft=std::min(right-px(1),left+nameWidth+px(6));
                RECT status{statusLeft,row.top,right-px(4),row.top+px(32)};
                drawTextAt(dc,message,status,smallFont_,p.muted,
                    DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            }
            // Monitor name buttons distinguish rows without separator lines.
        }
    }
    LRESULT drawHardwareSlider(NMCUSTOMDRAW& draw) {
        if (draw.dwDrawStage!=CDDS_PREPAINT) return CDRF_DODEFAULT;
        const Palette p=palette(); HWND slider=draw.hdr.hwndFrom;
        RECT client{},channel{},nativeThumb{}; GetClientRect(slider,&client);
        SendMessageW(slider,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&nativeThumb));
        if (client.right<=0 || client.bottom<=0) return CDRF_SKIPDEFAULT;
        auto& interaction=interactions_[slider];
        if (!interaction.surface) interaction.surface=std::make_unique<nightlight::PaintSurface>();
        HDC buffer=interaction.surface->ensure(draw.hdc,client.right,client.bottom);
        if (!buffer) {
            // Even an allocation failure must not fall back to the native
            // themed slider, which would flash a mismatched background.
            fill(draw.hdc,client,p.card);
            return CDRF_SKIPDEFAULT;
        }
        fill(buffer,client,p.card);
        const bool enabled=IsWindowEnabled(slider)!=FALSE;
        const COLORREF accent=enabled?p.accent:mix(p.accent,p.card,0.35);
        const auto now=uiTime();
        const int target=(nativeThumb.left+nativeThumb.right)/2;
        if (GetCapture()==slider) interaction.position.snap(target);
        else animate(interaction.position,target,110);
        const int cy=client.bottom/2;
        if (channel.right<=channel.left) { channel.left=px(10); channel.right=std::max(channel.left+1,client.right-px(10)); }
        RECT track{channel.left,cy-px(2),channel.right,cy+px(2)};
        const int cx=std::clamp(static_cast<int>(std::round(interaction.position.value(now))),static_cast<int>(track.left),static_cast<int>(track.right));
        rounded(buffer,track,px(4),p.border,p.border);
        RECT active=track; active.right=cx;
        if (active.right>active.left) rounded(buffer,active,px(4),accent,accent);
        drawSliderThumb(buffer,static_cast<float>(cx),static_cast<float>(cy),interaction.hover.value(now),interaction.press.value(now),enabled);
        BitBlt(draw.hdc,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY);
        return CDRF_SKIPDEFAULT;
    }
    void drawButton(const DRAWITEMSTRUCT& item) {
        RECT rc=item.rcItem;
        HDC dc=nullptr;
        HPAINTBUFFER paint=BeginBufferedPaint(item.hDC,&rc,BPBF_TOPDOWNDIB,nullptr,&dc);
        if (!paint) dc=item.hDC;
        const int savedDC=SaveDC(dc);
        const Palette p=palette();
        const bool pressed=(item.itemState&ODS_SELECTED)!=0;
        bool active=false;
        COLORREF background=p.chip,foreground=p.text,border=p.border;
        int id=static_cast<int>(item.CtlID);
        fill(dc,rc,p.card);
        std::wstring label;
        const bool screenButton=id>=ID_MONITOR_BASE &&
            id<ID_MONITOR_BASE+static_cast<int>(model_.monitors().size());
        const bool ddcButton=id>=ID_DDC_MONITOR_BASE &&
            id<ID_DDC_MONITOR_BASE+static_cast<int>(model_.monitors().size());
        const bool monitorButton=screenButton || ddcButton;
        if (monitorButton) {
            const size_t index=static_cast<size_t>(screenButton?id-ID_MONITOR_BASE:id-ID_DDC_MONITOR_BASE);
            const auto& mon=model_.monitors()[index];
            active=index==selected_;
            label=screenButton && editingName_ && mon.device==editingDevice_?L"":mon.displayName();
            if (active) {
                // Give the selected monitor the same darker background in
                // both Screens and DDC/CI, without affecting unselected chips.
                // Preserve the system palette in Windows high-contrast mode.
                background=!highContrast_
                    ? mix(p.selected,RGB(0,0,0),0.07) : p.selected;
                foreground=p.selectedText;
                border=p.border;
            }
        } else {
            wchar_t buf[256]{};
            GetWindowTextW(item.hwndItem,buf,256);
            label=buf;
            if (id==ID_GAMMA) {
                background=p.button; border=p.outline;
                foreground=p.selectedText;
            }
        }
        if(id==ID_REFRESH){label.clear();background=p.card;border=p.card;foreground=p.accent;}
        if(id==ID_SCREEN_OFF) {
            label.clear();
            background=p.chip;
            border=highContrast_?p.border:mix(p.border,p.accent,0.5);
            foreground=p.accent; // Same orange accent as other app actions.
        }
        auto& interaction=interactions_[item.hwndItem];
        const auto now=uiTime();
        const double hover=interaction.hover.value(now);
        const double push=std::max(interaction.press.value(now),pressed?1.0:0.0);
        background=mix(background,p.text,hover*0.035+push*0.055);
        if (!(item.itemState&ODS_DISABLED) && (item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT))
            border=mix(p.border,p.accent,0.55);
        if (item.itemState&ODS_DISABLED) foreground=p.muted;
        RECT text=rc;
        const int inset=static_cast<int>(std::round(px(1)*push));
        RECT surface=rc; InflateRect(&surface,-inset,-inset);
        // The refresh action is glyph-only, with no coloured button surface.
        if (id!=ID_REFRESH) rounded(dc,surface,px(8),background,border);
        if(id==ID_REFRESH) {
            // Convert the Fluent E777 glyph to a fixed ink-centred vector
            // path, instead of rotating a layout rectangle. Font bearings
            // are removed once before rotation, so the icon cannot orbit
            // off-centre or jump as the angle changes.
            Gdiplus::FontFamily fluent(L"Segoe Fluent Icons");
            Gdiplus::FontFamily mdl2(L"Segoe MDL2 Assets");
            const Gdiplus::FontFamily* family=fluent.IsAvailable()?&fluent:&mdl2;
            Gdiplus::GraphicsPath glyph;
            const Gdiplus::StringFormat* typography=
                Gdiplus::StringFormat::GenericTypographic();
            glyph.AddString(L"\uE777",1,family,Gdiplus::FontStyleRegular,19.0f,
                Gdiplus::PointF(0.0f,0.0f),typography);
            Gdiplus::RectF ink{};
            if (glyph.GetBounds(&ink)==Gdiplus::Ok &&
                ink.Width>0.0f && ink.Height>0.0f) {
                Gdiplus::Matrix zeroInkOrigin;
                zeroInkOrigin.Translate(-ink.X-ink.Width/2.0f,
                    -ink.Y-ink.Height/2.0f);
                glyph.Transform(&zeroInkOrigin);
                Gdiplus::Graphics g(dc);
                nightlight::configureIconGraphics(g);
                g.TranslateTransform(static_cast<float>(rc.left+rc.right)/2.0f,
                    static_cast<float>(rc.top+rc.bottom)/2.0f);
                // Positive angles rotate clockwise in Windows' y-down space.
                g.RotateTransform(static_cast<float>(
                    std::fmod(refreshTurn_.value(now),360.0)));
                // Keep the requested horizontally mirrored E777 orientation.
                const float scale=static_cast<float>(dpi_)/96.0f;
                g.ScaleTransform(-scale,scale);
                Gdiplus::SolidBrush brush(drawingColor(foreground));
                g.FillPath(&brush,&glyph);
            }
        }
        if (id==ID_SCREEN_OFF) {
            // U+E7E8 is Segoe Fluent's PowerButton glyph, centred by actual
            // ink bounds (the same technique as DDC brightness icons).
            const POINT center{(rc.left+rc.right)/2,(rc.top+rc.bottom)/2};
            drawCenteredDdcGlyph(dc,L"\uE7E8",center,17,foreground);
        }
        if (monitorButton) {
            drawSectionIcon(dc,0,rc.left+px(10),(rc.top+rc.bottom-px(16))/2,foreground);
            text.left+=px(32);
            text.right-=screenButton?px(52):px(8);
        }
        if (screenButton) {
            // One independent animated switch lives inside every monitor tab.
            const auto& monitor=model_.monitors()[static_cast<size_t>(id-ID_MONITOR_BASE)];
            animate(interaction.position,monitor.enabled?1:0,170);
            const double progress=interaction.position.value(now);
            const int switchW=px(32),switchH=px(18);
            const int switchLeft=rc.right-px(44);
            RECT r{switchLeft,(rc.top+rc.bottom-switchH)/2,
                switchLeft+switchW,(rc.top+rc.bottom+switchH)/2};
            const COLORREF track=mix(mix(p.card,p.accent,progress),p.text,hover*0.025+push*0.025);
            const COLORREF switchBorder=mix(p.muted,p.accent,progress);
            rounded(dc,r,px(18),track,switchBorder);
            const float dotX=static_cast<float>(r.left+px(9)+px(14)*progress);
            const float centerY=static_cast<float>((r.top+r.bottom)/2);
            smoothCircle(dc,dotX,centerY,static_cast<float>(px(6)),
                mix(p.subtle,RGB(255,255,255),progress));
        }
        drawTextAt(dc,label,text,monitorButton?boldFont_:font_,foreground,
            (monitorButton ? DT_LEFT : DT_CENTER)|DT_VCENTER|DT_SINGLELINE);
        RestoreDC(dc,savedDC);
        if (paint) EndBufferedPaint(paint,TRUE);
    }
    void updateTrayTooltip() {
        // addTray() installs the static tooltip. Avoid redundant shell IPC.
        if(!trayReady_ || wcscmp(notify_.szTip,L"NightLight")==0) return;
        notify_.uFlags=NIF_TIP|NIF_SHOWTIP;
        wcscpy_s(notify_.szTip,L"NightLight");
        Shell_NotifyIconW(NIM_MODIFY,&notify_);
    }
    void refreshConnectedDisplays() {
        if(displayRecoveryActive_ || !displayAwake_) { scheduleDisplayRecovery(); return; }
        finishInlineRename(true);commitDrag();endHardwareDrag();
        const std::wstring selectedDevice=hasMonitor()?
            nightlight::monitorProfileKey(*selectedMonitor()):L"";
        const bool changed=model_.refreshMonitors();
        if(changed) {
            // Keep selection tied to the physical display if discovery reorders tabs.
            const auto& monitors=model_.monitors();
            const auto previous=std::find_if(monitors.begin(),monitors.end(),
                [&](const Monitor& monitor) {
                    return nightlight::monitorProfileKey(monitor)==selectedDevice;
                });
            selected_=previous!=monitors.end()?static_cast<size_t>(previous-monitors.begin()):0;
            finishInlineRename(true);
        }
        if(!changed) model_.applyAll(nightlight::GammaRefresh::Restore);
        refreshTabs();refreshHardware();
        syncColorWatchdog();
        updateTrayTooltip();
    }
    void unlockGamma() {
        if (gammaHelper_) return;
        if (nightlight::gammaRangeUnlocked()) {
            syncGammaButton();
            MessageBoxW(window_, tr(L"Extended gamma range is already enabled. Restart Windows if you enabled it during this session.",
                L"O intervalo gama alargado já está ativo. Reinicie o Windows se o ativou nesta sessão.").c_str(), L"NightLight", MB_OK | MB_ICONINFORMATION);
            return;
        }
        wchar_t current[32768]{}; GetModuleFileNameW(nullptr, current, 32768);
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = window_; info.lpVerb = L"runas"; info.lpFile = current;
        info.lpParameters = L"--unlock-gamma"; info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) {
            if (GetLastError() != ERROR_CANCELLED)
                MessageBoxW(window_, tr(L"Could not start the administrator helper.", L"Não foi possível iniciar o auxiliar de administrador.").c_str(), L"NightLight", MB_OK | MB_ICONERROR);
            return;
        }
        gammaHelper_ = info.hProcess;
        EnableWindow(btnGamma_, FALSE);
        SetTimer(window_, TIMER_GAMMA, 250, nullptr);
    }

    void setKelvinFromX(int x, bool save) {
        if (!hasMonitor() || !selectedMonitor()->enabled) return;
        const int k=nightlight::kelvinFromTemperatureSliderX(
            x,slider_.left,slider_.right);
        if (save) model_.updateKelvin(selected_,k);
        else if (selectedMonitor()->kelvin != k) model_.previewKelvin(selected_,k);
        else return;
        // The Kelvin number updates immediately. The graphics
        // driver's gamma call is coalesced on Model's background worker.
        updateTrayTooltip();
        repaint();
    }
    void commitDrag() {
        if (!dragging_) return;
        dragging_=false;
        if (GetCapture()==window_) ReleaseCapture();
        if (hasMonitor()) model_.updateKelvin(selected_,selectedMonitor()->kelvin);
        repaint();
    }
    void populateLanguage(int idx) {
        Preferences pref=model_.preferences();
        pref.locale=idx==0?L"pt-PT":L"en-US";
        model_.updatePreferences(pref);
        updateLanguage();
        updateTheme();
    }
    void setTheme(const std::wstring& mode) {
        Preferences pref=model_.preferences();
        pref.theme=mode;
        model_.updatePreferences(pref);
        updateTheme();
    }
    void noteWindowPosition(bool flush=false) {
        // Do not capture startup, minimized, maximized or entrance-animation
        // coordinates, which are not the user's chosen normal window location.
        if (model_.isReady() && window_ && IsWindowVisible(window_) &&
            !IsIconic(window_) && !IsZoomed(window_) && !opening_) {
            RECT current{};
            if (GetWindowRect(window_,&current)) {
                const bool moved=model_.rememberWindowPosition(current.left,current.top);
                const bool sized=model_.rememberWindowSize(
                    current.right-current.left,current.bottom-current.top);
                if (moved || sized)
                    SetTimer(window_,TIMER_SAVE_WINDOW,600,nullptr);
            }
        }
        if (flush) {
            KillTimer(window_,TIMER_SAVE_WINDOW);
            model_.save();
        }
    }
    void turnOffScreens() {
        // Give the click's mouse-up 250ms to finish, like the Windhawk mod.
        // Repeated clicks reset only the initial one-shot delay.
        if (screenOffGuard_.guarding()) return;
        if (gammaTooltip_) SendMessageW(gammaTooltip_,TTM_POP,0,0);
        KillTimer(window_,TIMER_SCREEN_OFF_ACTION);
        if (!SetTimer(window_,TIMER_SCREEN_OFF_ACTION,
                      nightlight::kScreenOffActionDelayMs,nullptr)) {
            dispatchScreenOff();
        }
    }
    void dispatchScreenOff() {
        // No DDC/CI power-off or sleep/suspend: only ask Windows to turn
        // the displays off while normal apps and audio keep running.
        if (screenOffGuard_.guarding()) return;
        if (!screenOffGuard_.start()) {
            MessageBoxW(window_,tr(
                L"Could not install the three-second mouse and keyboard protection. Screens were not turned off.",
                L"Não foi possível ativar a proteção do rato e teclado durante três segundos. Os ecrãs não foram desligados.").c_str(),
                L"NightLight",MB_OK|MB_ICONWARNING);
            return;
        }
        // Exactly one asynchronous display-off broadcast per button press.
        // Subsequent requests occur only on physical input or a real wake,
        // at most 8 and no more often than every 450ms during protection.
        if (PostMessageW(HWND_BROADCAST,WM_SYSCOMMAND,
                         SC_MONITORPOWER,2)) {
            screenOffGuard_.markOffRequested();
        } else {
            screenOffGuard_.stop();
            MessageBoxW(window_,tr(
                L"Windows did not accept the screen-off request.",
                L"O Windows não aceitou o pedido para desligar os ecrãs.").c_str(),
                L"NightLight",MB_OK|MB_ICONWARNING);
        }
    }
    void openWindow(bool fromTray=false) {
        // Keep the fast tray-click path free of slow registry checks while
        // restoring the same animated entrance for all show requests.
        syncGammaButton();
        if (!fromTray) updateMotionPreference();
        const bool reveal=!IsWindowVisible(window_) || IsIconic(window_);
        if (reveal && animations_) {
            const auto style=GetWindowLongPtrW(window_,GWL_EXSTYLE);
            entranceAddedLayer_=(style&WS_EX_LAYERED)==0;
            SetWindowLongPtrW(window_,GWL_EXSTYLE,style|WS_EX_LAYERED);
            // First capture the resting position and move below it BEFORE
            // showing the window. Otherwise the first visible frame appears
            // at the final position and seems to jump when the slide begins.
            // Restoring a minimized window happens at zero alpha for the
            // same reason. Fade and slide then share one 150ms clock.
            if (SetLayeredWindowAttributes(window_,0,0,LWA_ALPHA)) {
                if (IsIconic(window_)) ShowWindow(window_,SW_RESTORE);
                GetWindowRect(window_,&entranceBounds_);
                entranceMove_=!IsZoomed(window_);
                opening_=true;
                constexpr BYTE initialAlpha=
                    nightlight::kWindowOpeningInitialAlpha;
                const double initialProgress=initialAlpha/255.0;
                entrance_.snap(initialProgress);
                if (entranceMove_) {
                    const int startY=entranceBounds_.top+
                        nightlight::windowOpeningSlideOffset(
                            static_cast<int>(dpi_),initialProgress);
                    SetWindowPos(window_,nullptr,entranceBounds_.left,startY,0,0,
                        SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                }
                SetLayeredWindowAttributes(window_,0,initialAlpha,LWA_ALPHA);
                if (!IsWindowVisible(window_)) ShowWindow(window_,SW_SHOW);
                animate(entrance_,1,nightlight::kWindowOpeningDurationMs);
                startAnimationTimer();
            } else {
                if (entranceAddedLayer_) SetWindowLongPtrW(window_,GWL_EXSTYLE,style);
                ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOW);
            }
        } else {
            ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOW);
        }
        SetForegroundWindow(window_);
        if(reveal) repaint();
        if (reveal && ddcVisible_ && displayAwake_ && !displayRecoveryActive_) {
            if (!hardwareDiscovered_) refreshHardware();
            else pollHardware();
        }
        syncHardwarePoll();
    }
    void hideWindowToTray() {
        // Minimize, Close (X) and tray-click dismissal share this path.
        // ShowWindow(SW_HIDE) asks Windows to activate another window; with
        // our hidden tool-window owner that can activate Explorer / Start
        // and spuriously display the Start-button tooltip.
        // SetWindowPos(SWP_HIDEWINDOW | SWP_NOACTIVATE) instead keeps the
        // focus transfer out of NightLight's control without requesting
        // another activation or changing its saved window geometry.
        if (trayReady_ && !shuttingDown_) {
            // If closed during the 150ms entrance, restore the normal
            // geometry and layered state before capturing its position.
            finishEntrance();
            // Dismiss only our own tooltip, not Explorer's taskbar tooltips.
            if (gammaTooltip_) SendMessageW(gammaTooltip_,TTM_POP,0,0);
            noteWindowPosition();
            SetWindowPos(window_,nullptr,0,0,0,0,nightlight::kHideToTrayFlags);
            if(!IsWindowVisible(window_)) {
                pageSurface_.reset();
                for(auto& item:interactions_) item.second.surface.reset();
            }
            syncHardwarePoll();
        }
    }
    bool cursorIsOverTrayIcon() const {
        // During a tray click Explorer can take foreground activation BEFORE
        // Windows delivers WM_TRAY_ICON. Do not treat this as an outside click.
        // Use the Shell's actual icon rectangle, including overflow placement
        // and non-primary monitors, rather than approximating the taskbar.
        if (!trayReady_ || !window_) return false;
        NOTIFYICONIDENTIFIER icon{};
        icon.cbSize=sizeof(icon);
        icon.hWnd=window_;
        icon.uID=TRAY_ID;
        RECT bounds{};
        if (FAILED(Shell_NotifyIconGetRect(&icon,&bounds))) return false;
        POINT cursor{};
        return GetCursorPos(&cursor) && PtInRect(&bounds,cursor)!=FALSE;
    }
    void toggleTrayWindow() {
        // Minimized windows are not displayed, so a click should restore them.
        if (IsWindowVisible(window_) && !IsIconic(window_)) {
            hideWindowToTray();
        } else {
            openWindow(true);
        }
    }
    void handleTrayActivation() {
        // Each real click toggles immediately. Event-mode filtering below
        // prevents duplicate legacy/v4 notifications, with no time throttle.
        toggleTrayWindow();
    }
    // The shell's theme may differ from the application's selected theme.
    // Read it only on startup/theme notifications, never on a polling timer.
    int trayIconResource() const {
        HIGHCONTRASTW hc{sizeof(hc)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) &&
            (hc.dwFlags&HCF_HIGHCONTRASTON)) {
            const COLORREF bg=GetSysColor(COLOR_WINDOW);
            const int luminance=299*GetRValue(bg)+587*GetGValue(bg)+114*GetBValue(bg);
            return luminance<128000?ICON_RESOURCE_DARK_TASKBAR:ICON_RESOURCE_LIGHT_TASKBAR;
        }
        DWORD light=1, bytes=sizeof(light);
        const LSTATUS status=RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);
        return status==ERROR_SUCCESS && light==0?ICON_RESOURCE_DARK_TASKBAR:ICON_RESOURCE_LIGHT_TASKBAR;
    }
    void refreshTrayIcon(bool preparingAdd=false) {
        if(!trayReady_ && !preparingAdd) return;
        const int resource=trayIconResource();
        const HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
        UINT dpi=taskbar?GetDpiForWindow(taskbar):GetDpiForSystem();
        if(!dpi) dpi=96;
        const int w=GetSystemMetricsForDpi(SM_CXSMICON,dpi);
        const int h=GetSystemMetricsForDpi(SM_CYSMICON,dpi);
        if(trayIcon_ && resource==trayIconResource_ &&
            w==trayIconWidth_ && h==trayIconHeight_) return;
        HICON next=static_cast<HICON>(LoadImageW(instance_,MAKEINTRESOURCEW(resource),
            IMAGE_ICON,w,h,LR_DEFAULTCOLOR));
        if(!next) return;
        if(trayReady_ && !preparingAdd) {
            NOTIFYICONDATAW update{};
            update.cbSize=sizeof(update);
            update.hWnd=window_; update.uID=TRAY_ID;
            update.uFlags=NIF_ICON; update.hIcon=next;
            if(!Shell_NotifyIconW(NIM_MODIFY,&update)) { DestroyIcon(next); return; }
            notify_.hIcon=next;
        }
        if(trayIcon_) DestroyIcon(trayIcon_);
        trayIcon_=next; trayIconResource_=resource;
        trayIconWidth_=w; trayIconHeight_=h;
    }
    bool addTray() {
        ZeroMemory(&notify_,sizeof(notify_));
        notify_.cbSize=sizeof(notify_);
        notify_.hWnd=window_;
        notify_.uID=TRAY_ID;
        notify_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
        notify_.uCallbackMessage=WM_TRAY_ICON;
        refreshTrayIcon(true);
        notify_.hIcon=trayIcon_?trayIcon_:LoadIconW(instance_,MAKEINTRESOURCEW(ICON_RESOURCE));
        wcscpy_s(notify_.szTip,L"NightLight");
        if (!Shell_NotifyIconW(NIM_ADD,&notify_)) return false;
        notify_.uVersion=NOTIFYICON_VERSION_4;
        // Version 4 changes the callback wParam/lParam layout; mouse-down
        // remains the fastest reliable activation on either Shell version.
        trayVersion4_=Shell_NotifyIconW(NIM_SETVERSION,&notify_)!=FALSE;
        trayClickFilter_={};
        trayReady_=true;
        return true;
    }
    void removeTray() {
        if (trayReady_) Shell_NotifyIconW(NIM_DELETE,&notify_);
        trayReady_=false; trayVersion4_=false;
        if(trayIcon_) { DestroyIcon(trayIcon_); trayIcon_=nullptr; }
        notify_.hIcon=nullptr;
        trayIconResource_=trayIconWidth_=trayIconHeight_=0;
    }
    HMENU buildTrayMenu(std::vector<HBITMAP>& icons,bool startupEnabled,HRESULT startupStatus) {
        auto createMenu=[] {
            HMENU result=CreatePopupMenu();
            MENUINFO settings{sizeof(settings)}; settings.fMask=MIM_STYLE;
            // Keep the bitmap gutter enabled. MNS_NOCHECK also suppresses
            // item bitmaps on some Windows menu themes.
            settings.dwStyle=0;
            SetMenuInfo(result,&settings);
            return result;
        };
        HMENU menu=createMenu();
        auto append=[&](HMENU target,UINT flags,UINT_PTR command,const wchar_t* text,nightlight::MenuIcon icon) {
            AppendMenuW(target,flags,command,text);
            HBITMAP bitmap=nightlight::menuIconBitmap(icon,dpi_,GetSysColor(flags&MF_GRAYED?COLOR_GRAYTEXT:COLOR_MENUTEXT),(flags&MF_CHECKED)!=0);
            if (bitmap) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_BITMAP;item.hbmpItem=bitmap;
                SetMenuItemInfoW(target,GetMenuItemCount(target)-1,TRUE,&item);
                icons.push_back(bitmap);
            }
        };
        HMENU monitors=createMenu();
        HMENU languages=createMenu();
        append(languages,MF_STRING|(!pt()?MF_CHECKED:0),ID_TRAY_LANGUAGE_BASE,tr(L"English",L"Inglês").c_str(),nightlight::MenuIcon::Language);
        append(languages,MF_STRING|(pt()?MF_CHECKED:0),ID_TRAY_LANGUAGE_BASE+1,tr(L"Portuguese (Portugal)",L"Português (Portugal)").c_str(),nightlight::MenuIcon::Language);
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(languages),tr(L"Language",L"Idioma").c_str(),nightlight::MenuIcon::Language);
        HMENU themes=createMenu();
        const auto& themeMode=model_.preferences().theme;
        append(themes,MF_STRING|(themeMode==L"system"?MF_CHECKED:0),
            ID_TRAY_THEME_BASE+2,tr(L"Follow system",L"Seguir sistema").c_str(),
            nightlight::MenuIcon::Theme);
        append(themes,MF_STRING|(themeMode==L"light"?MF_CHECKED:0),
            ID_TRAY_THEME_BASE,tr(L"Light",L"Claro").c_str(),nightlight::MenuIcon::Light);
        append(themes,MF_STRING|(themeMode==L"dark"?MF_CHECKED:0),
            ID_TRAY_THEME_BASE+1,tr(L"Dark",L"Escuro").c_str(),nightlight::MenuIcon::Dark);
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(themes),
            tr(L"Theme",L"Tema").c_str(),nightlight::MenuIcon::Theme);
        append(menu,MF_STRING|(startupEnabled?MF_CHECKED:0)|(FAILED(startupStatus)?MF_GRAYED:0),
            ID_TRAY_STARTUP,tr(L"Start with Windows",L"Iniciar com o Windows").c_str(),
            nightlight::MenuIcon::Start);
        append(menu,MF_STRING|(model_.preferences().closeByClickingOutside?MF_CHECKED:0),
            ID_TRAY_CLOSE_OUTSIDE,tr(L"Close by clicking outside",L"Fechar ao clicar fora").c_str(),
            nightlight::MenuIcon::Window);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        append(menu,MF_STRING|(ddcVisible_?MF_CHECKED:0),ID_DDC,tr(L"DDC/CI Controls",L"Controlos DDC/CI").c_str(),nightlight::MenuIcon::Adjust);
        HMENU engines=createMenu();
        if (model_.monitors().empty())
            append(engines,MF_STRING|MF_GRAYED,0,tr(L"No connected displays",L"Nenhum ecrã ligado").c_str(),nightlight::MenuIcon::Monitor);
        for (size_t i=0;i<model_.monitors().size() && i<1000;++i) {
            const auto& mon=model_.monitors()[i];
            HMENU options=createMenu();
            append(options,MF_STRING|(!mon.hardwareColor && !mon.nvidiaTargetColor?MF_CHECKED:0),ID_TRAY_COLOR_GPU_BASE+i,
                tr(L"GPU gamma",L"Gama da GPU").c_str(),nightlight::MenuIcon::Adjust);
            append(options,MF_STRING|(mon.hardwareColor?MF_CHECKED:0),ID_TRAY_COLOR_HW_BASE+i,
                tr(L"Monitor hardware (DDC/CI)",L"Hardware do ecrã (DDC/CI)").c_str(),nightlight::MenuIcon::Monitor);
            append(options,MF_STRING|(mon.nvidiaTargetColor?MF_CHECKED:0),ID_TRAY_COLOR_TARGET_BASE+i,
                tr(L"NVIDIA target gamma",L"Gama alvo NVIDIA").c_str(),nightlight::MenuIcon::Adjust);
            auto name=mon.displayName();
            if(mon.nvidiaTargetColor && mon.enabled && mon.kelvin<6500 && mon.gammaStatus==1)
                name+=tr(L" (driver accepted)",L" (aceite pelo controlador)");
            if((mon.hardwareColor || mon.nvidiaTargetColor) && mon.gammaStatus==0) name+=tr(L" (unavailable)",L" (indisponível)");
            if((mon.hardwareColor || mon.nvidiaTargetColor) && mon.gammaStatus<0) name+=tr(L" (checking)",L" (a verificar)");
            append(engines,MF_POPUP,reinterpret_cast<UINT_PTR>(options),name.c_str(),nightlight::MenuIcon::Monitor);
        }
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(engines),
            tr(L"Colour correction engine",L"Motor de correção de cor").c_str(),nightlight::MenuIcon::Adjust);
        if (model_.monitors().empty()) {
            append(monitors,MF_STRING|MF_GRAYED,0,
                tr(L"No connected displays",L"Nenhum ecrã ligado").c_str(),nightlight::MenuIcon::Monitor);
        }
        for (size_t i=0;i<model_.monitors().size();++i) {
            const auto& mon=model_.monitors()[i];
            const std::wstring name=mon.displayName();
            append(monitors,MF_STRING|(!mon.enabled?MF_CHECKED:0),
                        ID_TRAY_DISABLE_BASE+static_cast<UINT>(i),name.c_str(),nightlight::MenuIcon::Monitor);
        }
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(monitors),
                    tr(L"Disable for:",L"Desativar para:").c_str(),nightlight::MenuIcon::Disable);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        append(menu,MF_STRING,ID_TRAY_EXIT,tr(L"Exit",L"Sair").c_str(),nightlight::MenuIcon::Exit);
        return menu;
    }
    void popupTray() {
        // Query Task Scheduler only when the user opens the menu.
        bool startupEnabled=false;
        const HRESULT startupStatus=nightlight::queryStartupTask(startupEnabled);
        std::vector<HBITMAP> icons;
        HMENU menu=buildTrayMenu(icons,startupEnabled,startupStatus);
        POINT point{}; GetCursorPos(&point);
        SetForegroundWindow(window_);
        trayMenuOpen_=true;
        const UINT cmd=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON|
            TPM_NONOTIFY,point.x,point.y,0,window_,nullptr);
        trayMenuOpen_=false;
        DestroyMenu(menu);
        for (HBITMAP bitmap:icons) DeleteObject(bitmap);
        PostMessageW(window_,WM_NULL,0,0);
        if (cmd==ID_TRAY_EXIT) {
            shuttingDown_=true;
            DestroyWindow(window_);
        } else if ((cmd>=ID_TRAY_COLOR_GPU_BASE && cmd<ID_TRAY_COLOR_GPU_BASE+1000) ||
                   (cmd>=ID_TRAY_COLOR_HW_BASE && cmd<ID_TRAY_COLOR_HW_BASE+1000) ||
                   (cmd>=ID_TRAY_COLOR_TARGET_BASE && cmd<ID_TRAY_COLOR_TARGET_BASE+1000)) {
            const bool target=cmd>=ID_TRAY_COLOR_TARGET_BASE;
            const bool hardware=!target && cmd>=ID_TRAY_COLOR_HW_BASE;
            const size_t index=cmd-(target?ID_TRAY_COLOR_TARGET_BASE:hardware?ID_TRAY_COLOR_HW_BASE:ID_TRAY_COLOR_GPU_BASE);
            const auto backend=target?nightlight::ColorBackend::NvidiaTargetGamma:
                hardware?nightlight::ColorBackend::MonitorHardware:nightlight::ColorBackend::GpuGamma;
            if (index<model_.monitors().size()) {
                bool proceed=true;
                if(hardware && !model_.monitors()[index].hardwareColor)
                    proceed=MessageBoxW(window_,tr(
                        L"Use monitor RGB gain controls instead of GPU gamma? Requires DDC/CI and a compatible monitor. White cursor and all screen pixels will be warmed. Original gains are backed up beside NightLight.exe. Kelvin values are approximate.",
                        L"Usar os ganhos RGB do ecrã em vez da gama da GPU? Requer DDC/CI e um ecrã compatível. O cursor branco e todos os píxeis ficam mais quentes. Os ganhos originais são guardados junto ao NightLight.exe. Os valores Kelvin são aproximados.").c_str(),
                        L"NightLight",MB_YESNO|MB_ICONINFORMATION|MB_DEFBUTTON2)==IDYES;
                if(target && !model_.monitors()[index].nvidiaTargetColor)
                    proceed=MessageBoxW(window_,tr(
                        L"Experimental NVIDIA target gamma: reset NVIDIA Control Panel desktop colour settings to defaults BEFORE using this mode. Requires SDR with Advanced Color disabled. The API cannot read back the original target LUT; NightLight restores a neutral LUT, not your custom NVIDIA calibration. No browser or MPO settings are changed. Continue?",
                        L"Gama alvo NVIDIA experimental: restaure as predefinições de cor do ambiente de trabalho no Painel de Controlo NVIDIA ANTES de usar este modo. Requer SDR sem cor avançada. A API não permite ler a LUT original; o NightLight restaura uma LUT neutra, não a calibração personalizada. O navegador e o MPO não são alterados. Continuar?").c_str(),
                        L"NightLight",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;
                if(proceed) {model_.updateColorBackend(index,backend); syncColorWatchdog(); updateTrayTooltip(); repaint();}
            }
        } else if (cmd==ID_DDC) {
            toggleHardware();
        } else if (cmd==ID_TRAY_LANGUAGE_BASE || cmd==ID_TRAY_LANGUAGE_BASE+1) {
            populateLanguage(cmd==ID_TRAY_LANGUAGE_BASE?1:0);
        } else if (cmd>=ID_TRAY_THEME_BASE && cmd<=ID_TRAY_THEME_BASE+2) {
            setTheme(cmd==ID_TRAY_THEME_BASE?L"light":
                cmd==ID_TRAY_THEME_BASE+1?L"dark":L"system");
        } else if (cmd==ID_TRAY_STARTUP) {
            // Recheck just before toggling in case an external tool changed it.
            bool enabled=false;
            HRESULT hr=nightlight::queryStartupTask(enabled);
            if(SUCCEEDED(hr)) hr=nightlight::setStartupTask(!enabled);
            if(FAILED(hr)) {
                wchar_t code[32]{};
                swprintf_s(code,L"0x%08lX",static_cast<unsigned long>(hr));
                const auto message=tr(L"Could not change Windows startup. Task Scheduler returned ",
                    L"Não foi possível alterar o arranque com o Windows. O Agendador de Tarefas devolveu ")+
                    std::wstring(code);
                MessageBoxW(window_,message.c_str(),L"NightLight",MB_OK|MB_ICONERROR);
            }
        } else if (cmd==ID_TRAY_CLOSE_OUTSIDE) {
            Preferences pref=model_.preferences();
            pref.closeByClickingOutside=!pref.closeByClickingOutside;
            model_.updatePreferences(pref);
        } else if (cmd>=ID_TRAY_DISABLE_BASE &&
                   cmd<ID_TRAY_DISABLE_BASE+model_.monitors().size()) {
            const size_t index=cmd-ID_TRAY_DISABLE_BASE;
            toggleMonitor(index);
        }
    }
    void handleCommand(int id,int code) {
        if (code==BN_CLICKED) {
            if (id==ID_REFRESH) { refreshTurn_.to(refreshTurn_.value(uiTime())+360.0,uiTime(),460,animations_ && IsWindowVisible(window_)); if(animations_)startAnimationTimer(); refreshConnectedDisplays(); }
            else if (id==ID_SCREEN_OFF) turnOffScreens();
            else if (id==ID_GAMMA) unlockGamma();
            else if (id>=ID_MONITOR_BASE &&
                     id<ID_MONITOR_BASE+static_cast<int>(tabs_.size())) {
                select(static_cast<size_t>(id-ID_MONITOR_BASE));
            } else if (id>=ID_DDC_MONITOR_BASE &&
                       id<ID_DDC_MONITOR_BASE+static_cast<int>(hardwareRows_.size())) {
                select(static_cast<size_t>(id-ID_DDC_MONITOR_BASE));
            }
        }
    }
    LRESULT dispatch(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam) {
        if (taskbarCreated_ && msg==taskbarCreated_) {
            trayReady_=false;
            if (addTray()) KillTimer(hwnd,TIMER_TRAY_RETRY);
            else SetTimer(hwnd,TIMER_TRAY_RETRY,1000,nullptr);
            return 0;
        }
        switch (msg) {
        case WM_CREATE:
            initControls(); return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0)==SC_MINIMIZE && trayReady_ && !shuttingDown_) {
                // Handle the title-bar button, Alt+Space+N and Win+Down
                // BEFORE Windows enters its minimized state. Calling
                // ShowWindow(SW_HIDE) reentrantly from WM_SIZE can recurse
                // into WM_SHOWWINDOW/layout while the client is minimized.
                // Defer hiding until the current system command returns.
                // No nested WM_SHOWWINDOW or layout inside the system handler.
                PostMessageW(hwnd,WM_MINIMIZE_TO_TRAY,TRUE,0);
                return 0;
            }
            break;
        case WM_SIZE:
            if (wParam==SIZE_MINIMIZED) {
                // Fallback for ShowWindow(SW_MINIMIZE), Win+M and external
                // minimize requests that bypass WM_SYSCOMMAND. Never hide
                // synchronously inside WM_SIZE.
                if (trayReady_ && !shuttingDown_)
                    PostMessageW(hwnd,WM_MINIMIZE_TO_TRAY,0,0);
            } else layout();
            return 0;
        case WM_MINIMIZE_TO_TRAY:
            // wParam=TRUE is a queued SC_MINIMIZE, intercepted before iconify.
            // wParam=FALSE is an external minimize, which may have been restored.
            if (trayReady_ && !shuttingDown_ && IsWindowVisible(hwnd) &&
                (wParam != FALSE || IsIconic(hwnd)))
                hideWindowToTray();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam)==WA_INACTIVE && model_.preferences().closeByClickingOutside &&
                trayReady_ && !shuttingDown_ && !trayMenuOpen_ &&
                IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                // Avoid hiding when switching to a modal/owned dialog of this app.
                const HWND next=reinterpret_cast<HWND>(lParam);
                bool belongsToApp=false;
                for (HWND owner=next;owner;owner=GetWindow(owner,GW_OWNER)) {
                    if (owner==hwnd) { belongsToApp=true; break; }
                }
                // Clicking our own notification icon must be handled by
                // WM_TRAY_ICON, which toggles the window once. Otherwise this
                // deactivation hides it first and the same click reopens it.
                if (!belongsToApp && !cursorIsOverTrayIcon())
                    hideWindowToTray();
            }
            break;
        case WM_CLOSE:
            if (trayReady_ && !shuttingDown_) hideWindowToTray();
            else { noteWindowPosition(true); DestroyWindow(hwnd); }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* sizes=reinterpret_cast<MINMAXINFO*>(lParam);
            RECT current{}; GetWindowRect(hwnd,&current);
            const SIZE minimum=requiredWindowSizeForWidth(
                std::max(px(200),static_cast<int>(current.right-current.left)));
            // Honor an actual 200-DIP outer frame width. The content may
            // truncate labels, but slider tracks continue to respond.
            sizes->ptMinTrackSize.x=px(200);
            sizes->ptMinTrackSize.y=std::max(static_cast<LONG>(minimum.cy),sizes->ptMinTrackSize.y);
            return 0;
        }
        case WM_SIZING: {
            // When a side border is dragged, narrower width may wrap tabs and
            // require a taller window. Clamp the actual drag rectangle.
            auto* proposed=reinterpret_cast<RECT*>(lParam);
            if (!proposed) return TRUE;
            const int minWidth=px(200);
            if (proposed->right-proposed->left<minWidth) {
                if (wParam==WMSZ_LEFT || wParam==WMSZ_TOPLEFT || wParam==WMSZ_BOTTOMLEFT)
                    proposed->left=proposed->right-minWidth;
                else proposed->right=proposed->left+minWidth;
            }
            const SIZE finalMinimum=requiredWindowSizeForWidth(
                proposed->right-proposed->left);
            if (proposed->bottom-proposed->top<finalMinimum.cy) {
                if (wParam==WMSZ_TOP || wParam==WMSZ_TOPLEFT || wParam==WMSZ_TOPRIGHT)
                    proposed->top=proposed->bottom-finalMinimum.cy;
                else proposed->bottom=proposed->top+finalMinimum.cy;
            }
            return TRUE;
        }
        case WM_DPICHANGED: {
                dpi_=HIWORD(wParam);
            const RECT* suggested=reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,
                suggested->right-suggested->left,suggested->bottom-suggested->top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            updateFonts(); layout(); refreshTrayIcon(); return 0;
        }
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            updateMotionPreference(); updateTheme(); syncGammaButton();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: {
            RECT client{}; GetClientRect(hwnd,&client);
            paintClient(reinterpret_cast<HDC>(wParam),client); return 0;
        }
        case WM_ENTERSIZEMOVE:
            finishEntrance();finishReordering(); hardwareReveal_.finish();
            layout(); return 0;
        case WM_EXITSIZEMOVE:
            noteWindowPosition(true);
            return 0;
        case WM_WINDOWPOSCHANGED: {
            const auto* position=reinterpret_cast<const WINDOWPOS*>(lParam);
            if (position && (!(position->flags & SWP_NOMOVE) ||
                             !(position->flags & SWP_NOSIZE)))
                noteWindowPosition();
            // Default processing must still generate WM_MOVE and WM_SIZE.
            break;
        }
        case WM_SHOWWINDOW:
            if (!wParam) {
                finishEntrance();finishReordering(); hardwareReveal_.finish();
                // Minimized windows have zero/invalid client geometry.
                // Reflow after restore, not while Windows is minimizing.
                if (!IsIconic(hwnd)) layout();
                stopAnimationTimer();
                endHardwareDrag();finishScreenDrag();
                temperatureHover_=false; temperatureEngagement_.snap(0);
                for (auto& item:interactions_) { item.second.hover.snap(0); item.second.press.snap(0); }
                if (dragging_) commitDrag();
                pageSurface_.reset();
                for(auto& item:interactions_) item.second.surface.reset();
                KillTimer(hwnd,TIMER_READ_DDC); hardwarePollRunning_=false;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item && item->CtlType==ODT_BUTTON) {
                drawButton(*item); return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HDC dc=reinterpret_cast<HDC>(wParam);
            const Palette p=palette();
            const COLORREF background=(reinterpret_cast<HWND>(lParam)==editName_ && editingName_)?p.selected:p.card;
            SetTextColor(dc,p.text);
            SetBkColor(dc,background);
            // Reuse static DC_BRUSH, avoiding an extra GDI allocation on every message.
            SetDCBrushColor(dc,background);
            return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND:
            handleCommand(LOWORD(wParam),HIWORD(wParam)); return 0;
        case WM_TRAY_ICON: {
            // For NOTIFYICON_VERSION_4, LOWORD(lParam) is the message and
            // HIWORD(lParam) is our icon ID; legacy Shells use wParam as ID.
            if (trayVersion4_) {
                if (HIWORD(lParam)!=TRAY_ID) return 0;
            } else if (static_cast<UINT>(wParam)!=TRAY_ID) return 0;
            const UINT event=trayVersion4_?LOWORD(lParam):
                static_cast<UINT>(lParam);
            if (event==WM_CONTEXTMENU || event==WM_RBUTTONUP) {
                popupTray();
                return 0;
            }
            // Handle the second click in a quick pair: Windows replaces
            // WM_LBUTTONDOWN with WM_LBUTTONDBLCLK on that physical press.
            // A following mouse-up/NIN_SELECT must not toggle twice.
            bool toggle=false;
            const auto messageTime=static_cast<unsigned long>(GetMessageTime());
            switch (event) {
            case WM_LBUTTONDOWN:
                toggle=trayClickFilter_.shouldToggle(
                    nightlight::TrayClickEvent::MouseDown,messageTime);
                break;
            case WM_LBUTTONDBLCLK:
                toggle=trayClickFilter_.shouldToggle(
                    nightlight::TrayClickEvent::MouseDoubleClick,messageTime);
                break;
            case WM_LBUTTONUP:
                toggle=trayClickFilter_.shouldToggle(
                    nightlight::TrayClickEvent::MouseUp,messageTime);
                break;
            case NIN_SELECT:
                toggle=trayClickFilter_.shouldToggle(
                    nightlight::TrayClickEvent::Select,messageTime);
                break;
            case NIN_KEYSELECT:
                toggle=trayClickFilter_.shouldToggle(
                    nightlight::TrayClickEvent::KeySelect,messageTime);
                break;
            }
            if (toggle) handleTrayActivation();
            return 0;
        }
        case nightlight::WM_DDC_COMPLETE:
            syncHardware(); repaint(); return 0;
        case WM_HSCROLL: {
            HWND slider=reinterpret_cast<HWND>(lParam);
            if (auto* row=hardwareRow(slider)) {
                const bool contrast=slider==row->contrast;
                const int percent=(static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0))+50)/100;
                if (displayAwake_ && !displayRecoveryActive_ && ddcVisible_ && ddc_.set(row->device,contrast?nightlight::DdcFeature::Contrast:nightlight::DdcFeature::Brightness,percent)) {
                    if (model_.rememberHardwarePercent(row->device,contrast,percent)) SetTimer(hwnd,TIMER_SAVE_DDC,400,nullptr);
                }
                // Repaint the whole channel once; native partial thumb redraws
                // otherwise leave stale pieces of the custom filled track.
                InvalidateRect(slider,nullptr,FALSE);
                repaint(); return 0;
            }
            break;
        }
        case WM_NOTIFY:
            if(!lParam)return 0;
            if (reinterpret_cast<NMHDR*>(lParam)->code==NM_CUSTOMDRAW && hardwareRow(reinterpret_cast<NMHDR*>(lParam)->hwndFrom))
                return drawHardwareSlider(*reinterpret_cast<NMCUSTOMDRAW*>(lParam));
            break;
        case WM_GAMMA_COMPLETE:
            if (model_.collectGammaResults()) { updateTrayTooltip(); repaint(); }
            syncColorWatchdog();
            return 0;
        case WM_SETCURSOR: {
            POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
            if(LOWORD(lParam)==HTCLIENT && screenDragArmed_) {SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
            break;
        }
        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            // Use a 72-pixel-high pointer target at 100% scaling, even
            // though the visible gradient track is only 12 pixels high.
            // Slight extra horizontal padding makes the endpoints reachable.
            RECT touch=nightlight::sliderHitRect(slider_,px(12),px(30));

            if (PtInRect(&touch,p) && hasMonitor() && selectedMonitor()->enabled) {
                dragging_=true;
                SetCapture(hwnd);
                SetFocus(hwnd);
                setKelvinFromX(p.x,false);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if(screenDragArmed_){moveScreenDrag({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});return 0;}
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0}; TrackMouseEvent(&tracking);
            RECT target=nightlight::sliderHitRect(slider_,px(12),px(18));

            POINT cursor{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            const bool hover=PtInRect(&target,cursor)!=FALSE;
            if (hover!=temperatureHover_) { temperatureHover_=hover; repaint(); }
            if (hover) SetCursor(LoadCursorW(nullptr,IDC_HAND));
            if (dragging_) setKelvinFromX(GET_X_LPARAM(lParam),false);
            return 0;
        }
        case WM_MOUSELEAVE: temperatureHover_=false; repaint(); return 0;
        case WM_LBUTTONUP:
            if(screenDragArmed_){moveScreenDrag({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});finishScreenDrag(true);return 0;}
            if (dragging_) {
                setKelvinFromX(GET_X_LPARAM(lParam),false);
                commitDrag();
            }
            return 0;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            finishScreenDrag();
            if (dragging_) commitDrag();
            return 0;
        case WM_KEYDOWN:
            if(screenDragArmed_ && wParam==VK_ESCAPE){finishScreenDrag();return 0;}
            if (hasMonitor() && selectedMonitor()->enabled) {
                int value=selectedMonitor()->kelvin;
                if (wParam==VK_LEFT || wParam==VK_DOWN) value-=10;
                else if (wParam==VK_RIGHT || wParam==VK_UP) value+=10;
                else if (wParam==VK_PRIOR) value+=100;
                else if (wParam==VK_NEXT) value-=100;
                else if (wParam==VK_HOME) value=1000;
                else if (wParam==VK_END) value=6500;
                else break;
                model_.updateKelvin(selected_,value);
                updateTrayTooltip();
                repaint(); return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
        case WM_DEVICECHANGE:
            if (msg==WM_DISPLAYCHANGE) {
                pageSurface_.reset();
                for (auto& item:interactions_) item.second.surface.reset();
                // Drivers often reset DDC/CI levels when a panel reconnects.
                restoreDdcAfterWake_=true;
            }
            scheduleDisplayRecovery();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam==PBT_POWERSETTINGCHANGE && lParam) {
                const auto* setting=reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (IsEqualGUID(setting->PowerSetting,displayStateGuid) && setting->DataLength==sizeof(DWORD)) {
                    DWORD state=0; CopyMemory(&state,setting->Data,sizeof(state));
                    const bool wasAwake=displayAwake_;
                    displayAwake_=state!=0;
                    if (!displayAwake_) {
                        if (wasAwake) {
                            // Preserve the in-memory/saved last applied levels,
                            // and discard I/O to the now sleeping screens.
                            ddc_.invalidateTopology();
                            model_.beginDisplayRecovery();
                            restoreDdcAfterWake_=true;
                        }
                        KillTimer(hwnd,TIMER_DISPLAYS);
                        displayRecoveryActive_=false;
                        syncColorWatchdog(); syncHardwarePoll();
                    } else {
                        if(!wasAwake) restoreDdcAfterWake_=true;
                        scheduleDisplayRecovery();
                        // The screen-off input guard may still be preventing
                        // an accidental immediate wake. Defer recovery.
                        if (screenOffGuard_.guarding())
                            SetTimer(hwnd,TIMER_DISPLAYS,3500,nullptr);
                    }
                }
            } else if (wParam==PBT_APMRESUMEAUTOMATIC || wParam==PBT_APMRESUMESUSPEND) {
                displayAwake_=true;
                restoreDdcAfterWake_=true;
                scheduleDisplayRecovery();
            } else if (wParam==PBT_APMSUSPEND) {
                displayAwake_=false;
                restoreDdcAfterWake_=true;
                displayRecoveryActive_=false;
                ddc_.invalidateTopology();
                model_.beginDisplayRecovery();
                KillTimer(hwnd,TIMER_DISPLAYS);
                syncColorWatchdog(); syncHardwarePoll();
            }
            return TRUE;
        case WM_TIMER:
            if (wParam==TIMER_TRAY_RETRY) {
                if (trayReady_ || addTray()) KillTimer(hwnd,TIMER_TRAY_RETRY);
                return 0;
            }
            if (wParam==TIMER_SCREEN_OFF_ACTION) {
                KillTimer(hwnd,TIMER_SCREEN_OFF_ACTION);
                dispatchScreenOff();
                return 0;
            }
            if (wParam==TIMER_ANIMATION) { if (animationTimerRunning_) tickAnimations(); return 0; }
            if (wParam==TIMER_VERIFY_COLOR) {
                if (displayAwake_ && !displayRecoveryActive_ && !dragging_)
                    model_.applyAll(nightlight::GammaRefresh::Verify);
                syncColorWatchdog();
                return 0;
            }
            if (wParam==TIMER_READ_DDC) {
                pollHardware();
                return 0;
            }
            if (wParam==TIMER_SAVE_DDC) { KillTimer(hwnd,TIMER_SAVE_DDC); model_.save(); return 0; }
            if (wParam==TIMER_SAVE_WINDOW) { KillTimer(hwnd,TIMER_SAVE_WINDOW); model_.save(); return 0; }
            if (wParam==TIMER_GAMMA && gammaHelper_ && WaitForSingleObject(gammaHelper_,0)==WAIT_OBJECT_0) {
                DWORD code=1; GetExitCodeProcess(gammaHelper_, &code);
                CloseHandle(gammaHelper_); gammaHelper_=nullptr;
                KillTimer(hwnd,TIMER_GAMMA);
                EnableWindow(btnGamma_,TRUE);
                const bool ok=code==0 && nightlight::gammaRangeUnlocked();
                syncGammaButton();
                MessageBoxW(hwnd, ok ? tr(L"Extended gamma range enabled. Restart Windows to activate it for independent monitor colors.",
                    L"Intervalo gama alargado ativado. Reinicie o Windows para o aplicar às cores independentes por ecrã.").c_str() :
                    tr(L"Windows could not enable the extended gamma range.", L"O Windows não conseguiu ativar o intervalo gama alargado.").c_str(),
                    L"NightLight", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                model_.applyAll();
            }
            if (wParam==TIMER_DISPLAYS) {
                KillTimer(hwnd,TIMER_DISPLAYS);
                if (!displayAwake_ || !displayRecoveryActive_) return 0;
                if (screenOffGuard_.guarding()) {
                    SetTimer(hwnd,TIMER_DISPLAYS,1000,nullptr);
                    return 0;
                }
                // Count successful topology samples separately from restore
                // passes. Slow displays must receive the same full sequence
                // even if enumeration takes several seconds after wake.
                bool stable=false;
                const auto selectedKey=hasMonitor()?
                    nightlight::monitorProfileKey(*selectedMonitor()):L"";
                const bool changed=model_.refreshMonitors(true,&stable);
                if(!stable) {
                    if(recoveryPass_!=0) ddc_.invalidateTopology();
                    recoveryPass_=0;
                    recoverySettleChecks_=0;
                    recoveryVerificationIssued_=false;
                    SetTimer(hwnd,TIMER_DISPLAYS,1000,nullptr);
                    return 0;
                }
                if(changed) {
                    // GDI display numbers may have swapped. Never reuse the
                    // former device's queued DDC jobs or cached percentages.
                    ddc_.invalidateTopology();
                    recoveryPass_=0;
                    recoverySettleChecks_=0;
                    recoveryVerificationIssued_=false;
                    restoreDdcAfterWake_=true;
                    const auto& monitors=model_.monitors();
                    const auto found=std::find_if(monitors.begin(),monitors.end(),
                        [&](const Monitor& mon) {
                            return nightlight::monitorProfileKey(mon)==selectedKey;
                        });
                    selected_=found==monitors.end()?0:
                        static_cast<size_t>(found-monitors.begin());
                    refreshTabs();
                }
                constexpr UINT delays[]{1500,2500,4000,6000,8000,12000};
                if(recoveryPass_<std::size(delays)) {
                    // Reassert ALL screens, including ones already marked
                    // successful. A driver can accept a LUT before its
                    // display pipeline is completely awake. Pending work
                    // is not canceled or starved by the next pass.
                    model_.reassertWakeColors();
                    ensureHardwareRows();
                    for(const auto& mon:model_.monitors()) {
                        const auto brightness=model_.hardwarePercent(mon.device,false);
                        const auto contrast=model_.hardwarePercent(mon.device,true);
                        const bool savedBrightness=model_.hasHardwarePercent(mon.device,false);
                        const bool savedContrast=model_.hasHardwarePercent(mon.device,true);
                        ddc_.seed(mon.device,brightness,contrast,savedBrightness,savedContrast);
                        if(restoreDdcAfterWake_)
                            ddc_.restore(mon.device,brightness,contrast,savedBrightness,savedContrast,mon.physicalId);
                    }
                    const auto delay=delays[recoveryPass_++];
                    if(recoveryPass_==1) refreshHardware();
                    syncHardware();
                    updateTrayTooltip();
                    repaint();
                    SetTimer(hwnd,TIMER_DISPLAYS,delay,nullptr);
                    return 0;
                }
                // After the last delayed write, explicitly read the monitors
                // back even when the controls have never been opened. A
                // successful transport call is not proof of actual brightness.
                // Keep saved targets protected throughout this verification.
                if(restoreDdcAfterWake_ && !recoveryVerificationIssued_) {
                    bool writePending=false;
                    for(const auto& mon:model_.monitors()) {
                        const auto current=ddc_.state(mon.device);
                        writePending=writePending || current.brightness.pending || current.contrast.pending;
                    }
                    if(writePending && recoverySettleChecks_<3) {
                        ++recoverySettleChecks_;
                        SetTimer(hwnd,TIMER_DISPLAYS,6000,nullptr);
                        return 0;
                    }
                    for(const auto& mon:model_.monitors()) ddc_.refresh(mon.device);
                    recoveryVerificationIssued_=true;
                    SetTimer(hwnd,TIMER_DISPLAYS,2000,nullptr);
                    return 0;
                }
                // Wait for the actual workers; retry a bounded number of
                // times on a failed write or a mismatching settled readback.
                model_.collectGammaResults();
                bool pending=model_.wakeColorsPending();
                bool failed=false;
                for(const auto& mon:model_.monitors()) {
                    failed=failed || mon.gammaStatus==0;
                    const auto current=ddc_.state(mon.device);
                    pending=pending || current.loading || current.brightness.pending || current.contrast.pending;
                    const bool retryBrightness=restoreDdcAfterWake_ && current.brightness.failed &&
                        model_.hasHardwarePercent(mon.device,false);
                    const bool retryContrast=restoreDdcAfterWake_ && current.contrast.failed &&
                        model_.hasHardwarePercent(mon.device,true);
                    failed=failed || retryBrightness || retryContrast;
                    if(recoverySettleChecks_<3 && (retryBrightness || retryContrast))
                        ddc_.restore(mon.device,model_.hardwarePercent(mon.device,false),
                            model_.hardwarePercent(mon.device,true),retryBrightness,retryContrast,mon.physicalId);
                }
                if((pending || failed) && recoverySettleChecks_++<3) {
                    model_.retryFailedColors();
                    // A retry needs another verification after its grace period.
                    if(failed) recoveryVerificationIssued_=false;
                    SetTimer(hwnd,TIMER_DISPLAYS,6000,nullptr);
                    return 0;
                }
                ddc_.finishRestore();
                displayRecoveryActive_=false;
                restoreDdcAfterWake_=false;
                recoveryPass_=0;
                recoveryVerificationIssued_=false;
                syncHardware();
                syncColorWatchdog(); syncHardwarePoll();
                updateTrayTooltip(); repaint();
            }
            return 0;
        case WM_QUERYENDSESSION:
            noteWindowPosition(true); commitDrag(); model_.save(); return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                noteWindowPosition(true);
                model_.save();
                model_.restoreOriginals();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd,TIMER_TRAY_RETRY);
            KillTimer(hwnd,TIMER_SCREEN_OFF_ACTION);
            screenOffGuard_.stop();
            noteWindowPosition(true);
            shuttingDown_=true;
            endHardwareDrag();finishScreenDrag();
            finishEntrance(); stopAnimationTimer();
            KillTimer(hwnd,TIMER_VERIFY_COLOR); KillTimer(hwnd,TIMER_READ_DDC); KillTimer(hwnd,TIMER_DISPLAYS);
            if (displayPowerNotification_) { UnregisterPowerSettingNotification(displayPowerNotification_); displayPowerNotification_=nullptr; }
            KillTimer(hwnd,TIMER_GAMMA);
            KillTimer(hwnd,TIMER_SAVE_DDC);
            KillTimer(hwnd,TIMER_SAVE_WINDOW);
            commitDrag();
                model_.save();
            model_.restoreOriginals();
            ddc_.stop();
            if (gammaHelper_) { CloseHandle(gammaHelper_); gammaHelper_=nullptr; }
            removeTray();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd,msg,wParam,lParam);
    }
    static LRESULT CALLBACK staticWindowProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam) {
        NativeApp* app=reinterpret_cast<NativeApp*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) {
            const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lParam);
            app=static_cast<NativeApp*>(create->lpCreateParams);
            app->window_=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
        }
        return app?app->dispatch(hwnd,msg,wParam,lParam):
            DefWindowProcW(hwnd,msg,wParam,lParam);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE, PWSTR, int show) {
    // The narrow elevated helper must run before singleton/UI/config initialization.
    int helperArgc=0;
    LPWSTR* helperArgs=CommandLineToArgvW(GetCommandLineW(), &helperArgc);
    const bool helper=helperArgs && helperArgc==2 && wcscmp(helperArgs[1], L"--unlock-gamma")==0;
    if (helperArgs) LocalFree(helperArgs);
    if (helper) return static_cast<int>(nightlight::unlockGammaRange());
    HANDLE mutex=CreateMutexW(nullptr,TRUE,L"NightLight_SingleInstance_Mutex");
    if (!mutex) return 1;
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        // Scheduled Tasks, autorun, and a second manual launch must not
        // bring the existing window to the foreground. The tray icon is
        // the sole control for showing and hiding the main window.
        CloseHandle(mutex);
        return 0;
    }
    int exitCode=1;
    try {
        NativeApp app(instance);
        exitCode=app.run(show);
    } catch (const std::exception&) {
        MessageBoxW(nullptr,L"NightLight could not initialize.",
                    L"NightLight",MB_OK|MB_ICONERROR);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}

