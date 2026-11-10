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
#include <cmath>
#include <cwchar>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "core.hpp"
#include "ddc.hpp"
#include "motion.hpp"
#include "menu_icons.hpp"
#include "paint_surface.hpp"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using nightlight::Model;
using nightlight::Monitor;
using nightlight::Preferences;

namespace {
constexpr int ID_DDC = 1011, ID_DDC_SLIDER_BASE = 5000;
constexpr int ID_ENABLE = 1005, ID_RENAME_EDIT = 1006;
constexpr int ID_GAMMA = 1010, ID_REFRESH = 1012;
constexpr UINT TIMER_GAMMA = 11;
constexpr UINT TIMER_STATE = 12;
constexpr UINT TIMER_SAVE_DDC = 13;
constexpr UINT TIMER_ANIMATION = 16;
constexpr UINT TIMER_VERIFY_COLOR = 14, TIMER_RESTORE_DDC = 15;
// The interactive-session notification also covers display sleep without a
// topology change (WM_DISPLAYCHANGE alone does not).
constexpr GUID displayStateGuid{0x2b84c20e,0xad23,0x4ddf,{0x93,0xdb,0x05,0xff,0xbd,0x7e,0xfc,0xa5}};
constexpr int ID_MONITOR_BASE = 2000;
constexpr int ID_TRAY_DISABLE_BASE = 3000, ID_TRAY_EXIT = 3999;
constexpr int ID_TRAY_STARTUP_BASE = 4100;
constexpr int ID_TRAY_LANGUAGE_BASE = 4200, ID_TRAY_THEME_BASE = 4300;
constexpr UINT WM_TRAY_ICON = WM_APP + 37;
constexpr UINT WM_SHOW_EXISTING = WM_APP + 38;
constexpr UINT WM_GAMMA_COMPLETE = WM_APP + 41;
constexpr UINT TIMER_DISPLAYS = 10;
constexpr wchar_t CLASS_NAME[] = L"NightLight.Native.Win32";
constexpr int ICON_RESOURCE = 101;
constexpr UINT TRAY_ID = 100;

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
    NativeApp(HINSTANCE instance, std::wstring argument)
        : instance_(instance), argument_(std::move(argument)), model_() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&drawingToken_,&input,nullptr);
        BufferedPaintInit();
    }
    ~NativeApp() {
        if (trayReady_) removeTray();
        if (taskbarOwner_) DestroyWindow(taskbarOwner_);
        for (HFONT font : fonts_) if (font) DeleteObject(font);
        if (drawingToken_) Gdiplus::GdiplusShutdown(drawingToken_);
        BufferedPaintUnInit();
    }

    int run(int show) {
        gammaUnlocked_ = nightlight::gammaRangeUnlocked();
        if (model_.load()) model_.refreshMonitors();
        if (!model_.isReady()) {
            MessageBoxW(nullptr, model_.lastError().c_str(), L"NightLight",
                        MB_ICONERROR | MB_OK);
            return 2;
        }
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
                       IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        wc.hbrBackground = nullptr;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) return 3;

        const int x = (GetSystemMetrics(SM_CXSCREEN) - 560) / 2;
        const int initialHeight=std::clamp((gammaUnlocked_?338:388)+56+
            104*static_cast<int>(model_.monitors().size()),260,std::max(260,GetSystemMetrics(SM_CYSCREEN)-80));
        const int y = (GetSystemMetrics(SM_CYSCREEN) - initialHeight) / 2;
        // A hidden owner keeps the visible window off the taskbar without
        // changing its regular title bar, resizing, or caption buttons.
        taskbarOwner_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"NightLight Owner",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
        if (!taskbarOwner_) return 4;
        window_ = CreateWindowExW(0, CLASS_NAME, L"NightLight",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
            std::max(0, x), std::max(0, y), 560, initialHeight,
            taskbarOwner_, nullptr, instance_, this);
        if (!window_) return 4;

        const std::wstring launch = nightlight::resolveStartup(
            model_.preferences().startup, argument_, trayReady_);
        if (trayReady_ && (launch == L"tray" || launch == L"minimized")) {
            ShowWindow(window_, SW_HIDE);
        } else {
            ShowWindow(window_, show == SW_SHOWDEFAULT ? SW_SHOWNORMAL : show);
        }
        UpdateWindow(window_);
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
    std::wstring argument_;
    Model model_;
    nightlight::DdcController ddc_;
    HWND window_ = nullptr;
    HWND hardwareDrag_=nullptr;
    int hardwareDragLeft_=0,hardwareDragRight_=1;
    bool animationTimerRunning_=false;
    nightlight::PaintSurface pageSurface_;
    HWND taskbarOwner_ = nullptr;
    struct HardwareRow {
        std::wstring device;
        HWND brightness = nullptr, contrast = nullptr;
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
    unsigned recoveryAttempt_=0;
    int hardwareTop_ = 0;
    int hardwareHeight_ = 186;
    HWND btnRefresh_=nullptr, btnEnable_=nullptr, editName_=nullptr;
    bool editingName_=false;
    std::wstring editingDevice_;
    std::map<std::wstring,int> tabDesiredWidths_;
    std::vector<HWND> tabs_;
    HWND btnGamma_ = nullptr;
    bool gammaUnlocked_ = false;
    bool trayMenuOpen_=false;
    int temperatureHeight_ = 180;
    HWND gammaTooltip_ = nullptr;
    std::wstring gammaHelp_;
    HANDLE gammaHelper_ = nullptr;

    HFONT font_ = nullptr, boldFont_ = nullptr, headlineFont_ = nullptr, smallFont_ = nullptr;
    std::vector<HFONT> fonts_;
    size_t selected_ = 0;
    UINT dpi_ = 96;
    int width_ = 870, height_ = 440;
    RECT slider_ = {};
    int scrollOffset_=0, contentHeight_=0;
    bool layingOut_=false;
    unsigned layoutDepth_=0;
    ULONGLONG layoutTime_=0;
    int panelTop_ = 84, panelHeight_ = 142, secondPanelTop_ = 238;
    bool dragging_ = false;
    bool trayReady_ = false, shuttingDown_ = false;
    UINT taskbarCreated_ = 0;
    NOTIFYICONDATAW notify_ = {};

    struct Interaction {
        nightlight::Motion hover{0},press{0},position;
        bool repainting=false;
        std::unique_ptr<nightlight::PaintSurface> surface;
    };
    std::map<HWND,Interaction> interactions_;
    nightlight::Motion temperaturePosition_,temperatureEngagement_{0},entrance_{0};
    nightlight::Motion hardwareReveal_{1},refreshTurn_{0};
    bool sizeAnchorValid_=false;
    RECT sizeAnchor_{};
    int anchorClientHeight_=0,hardwareVisibleHeight_=0;
    bool animations_=true,highContrast_=false,opening_=false,temperatureHover_=false;
    bool entranceAddedLayer_=false,entranceMove_=true,parentAnimating_=false,reflowAnimating_=false,composedTransition_=false;
    RECT entranceBounds_{};

    bool pt() const { return model_.preferences().locale == L"pt-PT"; }
    std::wstring tr(const wchar_t* english, const wchar_t* portuguese) const {
        return pt() ? portuguese : english;
    }
    Palette palette() const {
        auto p=colors(model_.preferences().theme==L"dark");
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
            if (btnEnable_) layout();
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
            if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
            if(msg==WM_LBUTTONDBLCLK) {
                app->finishScreenDrag();
                app->startInlineRename(static_cast<size_t>(tab-app->tabs_.begin()));return 0;
            }
            if(msg==WM_LBUTTONDOWN) {
                POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};MapWindowPoints(control,app->window_,&point,1);
                app->beginScreenDrag(static_cast<size_t>(tab-app->tabs_.begin()),point);return 0;
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
        if (IsWindowVisible(window_) && animations_) DwmFlush();
        const auto now=uiTime();
        bool active=false;
        if (opening_) {
            const double progress=entrance_.value(now);
            SetLayeredWindowAttributes(window_,0,static_cast<BYTE>(std::round(255*progress)),LWA_ALPHA);
            if (entranceMove_) SetWindowPos(window_,nullptr,entranceBounds_.left,entranceBounds_.top+static_cast<int>(std::round(px(8)*(1-progress))),
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
        if (window_) InvalidateRect(window_, nullptr, FALSE);
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
        headlineFont_ = create(30, FW_NORMAL);
        tabDesiredWidths_.clear();
        const HWND controls[] = {btnRefresh_,btnEnable_,btnGamma_,editName_};
        for (HWND control : controls) {
            if (control) SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(font_), TRUE);
        }
        for (auto& row:hardwareRows_) for (HWND slider:{row.brightness,row.contrast})
            SendMessageW(slider,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
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
        btnEnable_ = createButton(ID_ENABLE, L"");
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
        refreshHardware();
        restoreHardware();
        displayPowerNotification_=RegisterPowerSettingNotification(window_,&displayStateGuid,DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(window_,TIMER_VERIFY_COLOR,3000,nullptr);
        SetTimer(window_,TIMER_RESTORE_DDC,30000,nullptr);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        trayReady_ = addTray();
        SetTimer(window_, TIMER_STATE, 250, nullptr);
        model_.setStatusWindow(window_);
        model_.collectGammaResults();
        updateTrayTooltip();
        // Native window controls render immediately; no WebView runtime.
    }
    void updateTheme() {
        const BOOL dark = model_.preferences().theme == L"dark";
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
        repaint();
        for (HWND h : tabs_) InvalidateRect(h, nullptr,FALSE);
        const HWND buttons[] = {btnRefresh_,btnEnable_,btnGamma_};
        for (HWND b : buttons) if (b) InvalidateRect(b, nullptr,FALSE);
        for (const auto& row:hardwareRows_) { InvalidateRect(row.brightness,nullptr,FALSE); InvalidateRect(row.contrast,nullptr,FALSE); }
    }
    void updateLanguage() {
        for (size_t i=0;i<hardwareRows_.size();++i) {
            const auto& row=hardwareRows_[i];
            const auto suffix=L" · "+model_.monitors()[i].displayName();
            SetWindowTextW(row.brightness,(tr(L"Brightness",L"Brilho")+suffix).c_str());
            SetWindowTextW(row.contrast,(tr(L"Contrast",L"Contraste")+suffix).c_str());
        }
        SetWindowTextW(btnRefresh_,tr(L"Refresh displays",L"Atualizar ecrãs").c_str());
        SetWindowTextW(btnEnable_, tr(L"Enabled", L"Ativo").c_str());
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
        if (selected_ >= model_.monitors().size()) selected_ = 0;
        for (size_t index = 0; index < model_.monitors().size(); ++index) {
            HWND tab = createButton(ID_MONITOR_BASE + static_cast<int>(index), L"");
            tabs_.push_back(tab);
            if (font_) SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        ensureHardwareRows();
        layout();
    }
    void rememberSizeAnchor() {
        if (sizeAnchorValid_ || !IsWindowVisible(window_) || IsZoomed(window_) || IsIconic(window_)) return;
        finishEntrance(); GetWindowRect(window_,&sizeAnchor_);
        anchorClientHeight_=height_; sizeAnchorValid_=true;
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
        InvalidateRect(btnEnable_, nullptr,FALSE);
        for (HWND tab : tabs_) InvalidateRect(tab, nullptr,FALSE);
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
            if(!hardwareRow(control)) {
                auto& motion=reorderPositions_[control];
                motion.x.to(x,layoutTime_,180,reorderInProgress_ && animations_ && IsWindowVisible(window_));
                motion.y.to(y,layoutTime_,180,reorderInProgress_ && animations_ && IsWindowVisible(window_));
                x=static_cast<int>(std::round(motion.x.value(layoutTime_)));
                y=static_cast<int>(std::round(motion.y.value(layoutTime_)));
            }
            placements.push_back({control,x,y-scrollOffset_,std::max(1,w),std::max(1,h)});
        };
        const double hardwareProgress=hardwareReveal_.value(layoutTime_);
        composedTransition_=IsWindowVisible(window_) && (hardwareReveal_.active(layoutTime_) || reorderInProgress_);
        const int margin=px(20),gap=px(8),inset=px(16);
        panelTop_=px(12);
        const int innerLeft=margin+inset,innerRight=width_-margin-inset;
        const int tabH=px(32),tabGap=px(6);
        const int enableW=textWidth(tr(L"Enabled",L"Ativo"))+px(42);
        const int tabsRight=innerRight-enableW-gap;
        place(btnRefresh_,innerLeft,panelTop_+px(40),px(32),tabH,TRUE);
        const int tabStart=innerLeft+px(38);
        int tabX=tabStart,tabY=panelTop_+px(40),rows=1;
        for (size_t i=0;i<tabs_.size();++i) {
            const auto& m=model_.monitors()[i];
            auto stored=tabDesiredWidths_.find(m.device);
            if(stored==tabDesiredWidths_.end())stored=tabDesiredWidths_.emplace(m.device,textWidth(m.displayName(),boldFont_)+px(50)).first;
            const int tabW=std::max(px(44),std::min({stored->second,px(279),tabsRight-tabStart}));
            if (tabX>tabStart && tabX+tabW>tabsRight) { tabX=tabStart; tabY+=px(38); ++rows; }
            place(tabs_[i],tabX,tabY,tabW,tabH,TRUE);
            if(editingName_ && m.device==editingDevice_) {
                // Edit directly in the tab; Enter saves and Escape cancels.
                place(editName_,tabX,tabY,tabW,tabH,TRUE);
            }
            tabX+=tabW+tabGap;
        }
        place(btnEnable_,std::min(tabX,tabsRight+gap),tabY,enableW,tabH,TRUE);
        panelHeight_=px(46+rows*38);
        secondPanelTop_=panelTop_+panelHeight_+px(12);
        temperatureHeight_=px(gammaUnlocked_?112:162);
        place(btnGamma_,innerRight-px(220),secondPanelTop_+px(119),px(220),px(29),TRUE);
        const int sliderW=std::min(px(480),innerRight-innerLeft-px(8));
        const int sliderLeft=(width_-sliderW)/2;
        slider_={sliderLeft,secondPanelTop_+px(62),sliderLeft+sliderW,secondPanelTop_+px(66)};
        hardwareTop_=secondPanelTop_+temperatureHeight_+px(12);
        hardwareHeight_=px(hardwareRows_.empty()?72:44+104*static_cast<int>(hardwareRows_.size()));
        for (size_t i=0;i<hardwareRows_.size();++i) {
            auto& row=hardwareRows_[i];
            row.topMotion.to(hardwareTop_+px(36+104*static_cast<int>(i)),layoutTime_,180,reorderInProgress_ && animations_ && IsWindowVisible(window_));
            row.top=static_cast<int>(std::round(row.topMotion.value(layoutTime_)));
            place(row.brightness,innerLeft+px(92),row.top+px(20),innerRight-innerLeft-px(144),px(32),TRUE);
            place(row.contrast,innerLeft+px(92),row.top+px(52),innerRight-innerLeft-px(144),px(32),TRUE);
        }
        hardwareVisibleHeight_=static_cast<int>(std::round(hardwareHeight_*hardwareProgress));
        contentHeight_=secondPanelTop_+temperatureHeight_+margin+
            static_cast<int>(std::round((hardwareHeight_+px(12))*hardwareProgress));
        // Fit the outer frame to the measured content, preserving the user's
        // width. Keep the window inside its own monitor's usable work area.
        if (!IsZoomed(window_) && !IsIconic(window_)) {
            RECT outer{}; GetWindowRect(window_,&outer);
            MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor)) {
                const int chrome=(outer.bottom-outer.top)-height_;
                const int available=std::max(px(260),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
                const int desired=std::clamp(contentHeight_+chrome,px(260),available);
                // Keep the window inside the work area as display rows change.
                const int preferredTop=sizeAnchorValid_?sizeAnchor_.top:outer.top;
                const int y=std::clamp(preferredTop,static_cast<int>(monitor.rcWork.top),
                    std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-desired));
                if (outer.bottom-outer.top!=desired || outer.top!=y) {
                    rememberSizeAnchor();
                    finishEntrance();
                    SetWindowPos(window_,nullptr,outer.left,y,outer.right-outer.left,desired,
                        SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
                    layingOut_=false; layout(); return;
                }
            }
        }
        const int correctedScroll=std::clamp(scrollOffset_,0,std::max(0,contentHeight_-height_));
        const bool scrollChanged=correctedScroll!=scrollOffset_;
        scrollOffset_=correctedScroll;
        SCROLLINFO info{sizeof(info)};
        info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
        info.nMin=0; info.nMax=contentHeight_-1;
        info.nPage=static_cast<UINT>(height_); info.nPos=scrollOffset_;
        SetScrollInfo(window_,SB_VERT,&info,TRUE);
        RECT updated{}; GetClientRect(window_,&updated);
        if (scrollChanged || updated.right!=width_) { layingOut_=false; layout(); return; }
        // Do not let MoveWindow copy old pixels while its siblings still occupy
        // their previous coordinates. All children move before any can repaint.
        const UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW;
        auto visibility=[&](HWND control) -> UINT {
            if (control==btnGamma_) return gammaUnlocked_?SWP_HIDEWINDOW:SWP_SHOWWINDOW;
            if(control==editName_)return editingName_?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
            if (hardwareRow(control)) return hardwareProgress>0?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
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
            const RECT textRect{px(8),lineTop,
                std::max(px(9),static_cast<int>(bounds.right)-px(8)),
                std::min(static_cast<int>(bounds.bottom),lineTop+lineH)};
            SendMessageW(editName_,EM_SETRECT,0,reinterpret_cast<LPARAM>(&textRect));
        }
        // Clip native child controls to the sliding panel. No control can draw
        // into a neighboring card while its section is still being revealed.
        for (const auto& placement:placements) {
            RECT clip{}; bool clipped=false;
            if (hardwareRow(placement.control)) {
                clip={0,hardwareTop_-scrollOffset_,width_,hardwareTop_+hardwareVisibleHeight_-scrollOffset_}; clipped=hardwareProgress<1;
            }
            if (placement.control==editName_ && editingName_) {
                HRGN roundedEdit=CreateRoundRectRgn(0,0,placement.w+1,placement.h+1,px(16),px(16));
                if (!SetWindowRgn(placement.control,roundedEdit,FALSE)) DeleteObject(roundedEdit);
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
        if (sizeAnchorValid_ && !hardwareReveal_.active(uiTime()) && height_==anchorClientHeight_)
            sizeAnchorValid_=false;
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
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
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
        const bool dark=model_.preferences().theme==L"dark";
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
        const int cx=slider_.left+static_cast<int>(std::round((temperaturePosition_.value(uiTime())-1000)*span/5500));
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
                IntersectClipRect(dc,0,hardwareTop_-scrollOffset_-rect.top,rect.right-rect.left,
                    hardwareTop_+hardwareVisibleHeight_-scrollOffset_-rect.top);
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
        SetViewportOrgEx(mem,0,-scrollOffset_,nullptr);
        const RECT card1{px(20),panelTop_,client.right-px(20),panelTop_+panelHeight_};
        rounded(mem,card1,px(16),p.card,p.border);
        const RECT card2{px(20),secondPanelTop_,client.right-px(20),secondPanelTop_+temperatureHeight_};
        rounded(mem,card2,px(16),p.card,p.border);
        drawSectionIcon(mem,0,px(36),panelTop_+px(11),p.subtle);
        RECT screensTitle{px(60),panelTop_+px(8),client.right-px(36),panelTop_+px(30)};
        drawTextAt(mem,tr(L"Screens",L"Ecrãs"),screensTitle,boldFont_,p.text);
        drawSectionIcon(mem,1,px(36),secondPanelTop_+px(11),p.subtle);
        RECT label{px(60),secondPanelTop_+px(8),client.right-px(176),secondPanelTop_+px(30)};
        drawTextAt(mem,tr(L"Color temperature",L"Temperatura de cor"),label,boldFont_,p.text);
        const int current=hasMonitor()?selectedMonitor()->kelvin:5000;
        RECT valueRect{client.right-px(176),secondPanelTop_+px(9),client.right-px(36),secondPanelTop_+px(59)};
        drawTextAt(mem,formatNumber(current)+L" K",valueRect,headlineFont_,p.accent,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        drawSlider(mem);
        RECT low{px(36),secondPanelTop_+px(84),px(270),secondPanelTop_+px(106)};
        RECT high{client.right-px(270),secondPanelTop_+px(84),client.right-px(36),secondPanelTop_+px(106)};
        drawTextAt(mem,L"1000 K",low,smallFont_,p.muted);
        drawTextAt(mem,L"6500 K",high,smallFont_,p.muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

        if (hardwareVisibleHeight_>0) {
            const int saved=SaveDC(mem);
            IntersectClipRect(mem,0,hardwareTop_,width_,hardwareTop_+hardwareVisibleHeight_);
            drawHardware(mem); RestoreDC(mem,saved);
        }
        if (composedTransition_) drawTransitionControls(mem);
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
    HardwareRow* hardwareRow(HWND slider) {
        for (auto& row:hardwareRows_) if (row.brightness==slider || row.contrast==slider) return &row;
        return nullptr;
    }
    void ensureHardwareRows() {
        const auto& monitors=model_.monitors();
        bool same=hardwareRows_.size()==monitors.size();
        for (size_t i=0;same && i<monitors.size();++i) same=hardwareRows_[i].device==monitors[i].device;
        if (same) return;
        for (auto& row:hardwareRows_) { DestroyWindow(row.brightness); DestroyWindow(row.contrast); }
        hardwareRows_.clear();
        for (size_t i=0;i<monitors.size();++i) {
            HardwareRow row; row.device=monitors[i].device;
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
                SetWindowTheme(slider,L"",L""); watchInteraction(slider); EnableWindow(slider,FALSE);
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
        ensureHardwareRows();
        for (const auto& row:hardwareRows_) ddc_.refresh(row.device);
        syncHardware();
    }
    void restoreHardware() {
        if (!displayAwake_) return;
        // This runs independently of panel visibility. Saved targets, never
        // reset monitor readbacks or placeholder percentages, are authoritative.
        for (const auto& monitor:model_.monitors()) {
            const bool brightness=model_.hasHardwarePercent(monitor.device,false),contrast=model_.hasHardwarePercent(monitor.device,true);
            if (!brightness && !contrast) ddc_.refresh(monitor.device);
            else ddc_.restore(monitor.device,model_.hardwarePercent(monitor.device,false),model_.hardwarePercent(monitor.device,true),brightness,contrast);
        }
    }
    void scheduleDisplayRecovery() {
        recoveryAttempt_=0;
        SetTimer(window_,TIMER_DISPLAYS,500,nullptr);
    }
    void syncHardware() {
        for (auto& row:hardwareRows_) {
            const auto state=ddc_.state(row.device);
            auto sync=[&](HWND slider,bool contrast,const nightlight::DdcValue& value) {
                EnableWindow(slider,ddcVisible_ && !state.loading && value.supported);
                const bool captured=GetCapture()==slider;
                if (!captured) {
                    const int current=static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0));
                    if ((current+50)/100!=value.percent) SendMessageW(slider,TBM_SETPOS,TRUE,value.percent*100);
                }
                if (value.managed && !captured && model_.rememberHardwarePercent(row.device,contrast,value.percent))
                    SetTimer(window_,TIMER_SAVE_DDC,400,nullptr);
                InvalidateRect(slider,nullptr,FALSE);
            };
            sync(row.brightness,false,state.brightness); sync(row.contrast,true,state.contrast);
        }
    }
    void toggleHardware() {
        rememberSizeAnchor();
        ddcVisible_=!ddcVisible_;
        animate(hardwareReveal_,ddcVisible_?1:0,240);
        if (ddcVisible_) refreshHardware();
        else syncHardware();
        layout();
        repaint();
    }
    void drawHardware(HDC dc) {
        const Palette p=palette();
        RECT card{px(20),hardwareTop_,width_-px(20),hardwareTop_+hardwareHeight_};
        rounded(dc,card,px(16),p.card,p.border);
        drawSectionIcon(dc,2,px(36),hardwareTop_+px(13),p.subtle);
        RECT heading{px(60),hardwareTop_+px(10),width_-px(36),hardwareTop_+px(32)};
        drawTextAt(dc,tr(L"DDC/CI Controls",L"Controlos DDC/CI"),heading,boldFont_,p.text);
        if (hardwareRows_.empty()) {
            RECT empty{px(36),hardwareTop_+px(40),width_-px(36),hardwareTop_+px(62)};
            drawTextAt(dc,tr(L"No connected displays",L"Nenhum ecrã ligado"),empty,font_,p.muted);
        }
        for (size_t i=0;i<hardwareRows_.size();++i) {
            const auto& row=hardwareRows_[i];
            const auto& monitor=model_.monitors()[i];
            const auto state=ddc_.state(row.device);
            drawSectionIcon(dc,0,px(36),row.top+px(2),p.subtle);
            RECT name{px(60),row.top,width_-px(36),row.top+px(20)};
            drawTextAt(dc,monitor.displayName(),name,boldFont_,p.text);
            auto label=[&](const std::wstring& text,HWND slider,int y,bool contrast) {
                RECT caption{px(36),y,px(120),y+px(32)};
                drawTextAt(dc,text,caption,font_,p.text);
                RECT number{width_-px(84),y,width_-px(36),y+px(32)};
                const int percent=GetCapture()==slider?(static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0))+50)/100:
                    model_.hardwarePercent(row.device,contrast);
                drawTextAt(dc,formatNumber(percent)+L" %",number,smallFont_,p.subtle,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            };
            label(tr(L"Brightness",L"Brilho"),row.brightness,row.top+px(20),false);
            label(tr(L"Contrast",L"Contraste"),row.contrast,row.top+px(52),true);
            std::wstring message;
            if (state.brightness.failed || state.contrast.failed)
                message=tr(L"The monitor did not accept the adjustment. Try again.",L"O monitor não aceitou o ajuste. Tente novamente.");
            RECT status{px(36),row.top+px(84),width_-px(36),row.top+px(100)};
            drawTextAt(dc,message,status,smallFont_,p.muted);
            if (i+1<hardwareRows_.size()) {
                RECT divider{px(36),row.top+px(103),width_-px(36),row.top+px(104)};
                fill(dc,divider,p.border);
            }
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
        if (!buffer) return CDRF_DODEFAULT;
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
        if (id>=ID_MONITOR_BASE && id<ID_MONITOR_BASE+static_cast<int>(model_.monitors().size())) {
            const size_t index=static_cast<size_t>(id-ID_MONITOR_BASE);
            const auto& mon=model_.monitors()[index];
            active=index==selected_;
            label=editingName_ && mon.device==editingDevice_?L"":mon.displayName();
            if (active) { background=p.selected; foreground=p.selectedText; border=p.border; }
        } else if (id==ID_ENABLE) {
            active=hasMonitor() && selectedMonitor()->enabled;
            label=tr(L"Enabled",L"Ativo");
            background=p.card; border=p.card;
        } else {
            wchar_t buf[256]{};
            GetWindowTextW(item.hwndItem,buf,256);
            label=buf;
            if (id==ID_GAMMA) {
                background=p.button; border=p.outline;
                foreground=p.selectedText;
            }
        }
        if(id==ID_REFRESH){label.clear();background=p.accent;border=p.accent;foreground=RGB(255,255,255);}
        auto& interaction=interactions_[item.hwndItem];
        const auto now=uiTime();
        const double hover=interaction.hover.value(now);
        const double push=std::max(interaction.press.value(now),pressed?1.0:0.0);
        background=mix(background,p.text,hover*0.035+push*0.055);
        if (!(item.itemState&ODS_DISABLED) && (item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT))
            border=mix(p.border,p.accent,0.55);
        if (item.itemState&ODS_DISABLED) foreground=p.muted;
        RECT text=rc;
        const bool monitorButton=id>=ID_MONITOR_BASE && id<ID_MONITOR_BASE+static_cast<int>(model_.monitors().size());
        const int inset=static_cast<int>(std::round(px(1)*push));
        RECT surface=rc; InflateRect(&surface,-inset,-inset);
        if (id!=ID_ENABLE) rounded(dc,surface,px(8),background,border);
        if(id==ID_REFRESH) {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            const float scale=static_cast<float>(dpi_)/96.0f;
            g.TranslateTransform(static_cast<float>(rc.left+rc.right)/2,static_cast<float>(rc.top+rc.bottom)/2);
            g.RotateTransform(static_cast<float>(std::fmod(refreshTurn_.value(now),360.0)));
            g.ScaleTransform(scale,scale);
            Gdiplus::Pen pen(drawingColor(foreground),1.7f);
            pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);
            constexpr float r=7.0f;
            g.DrawArc(&pen,-r,-r,2*r,2*r,45.0f,315.0f);
            constexpr float radians=0.0f;
            const float ex=r*std::cos(radians),ey=r*std::sin(radians);
            const float tx=-std::sin(radians),ty=std::cos(radians);
            const float nx=std::cos(radians),ny=std::sin(radians);
            // Close the stroke with a complete downward-pointing arrowhead.
            const Gdiplus::PointF arrow[]{
                {ex,ey+1.2f},
                {ex-4.5f*tx+2.8f*nx,ey-4.5f*ty+2.8f*ny},
                {ex-4.5f*tx-2.8f*nx,ey-4.5f*ty-2.8f*ny}};
            Gdiplus::SolidBrush tip(drawingColor(foreground));
            g.FillPolygon(&tip,arrow,3);
        }
        if (monitorButton && !(editingName_ && model_.monitors()[id-ID_MONITOR_BASE].device==editingDevice_)) {
            drawSectionIcon(dc,0,rc.left+px(10),(rc.top+rc.bottom-px(16))/2,foreground);
            text.left+=px(32);text.right-=px(8);
        }
        if (id==ID_ENABLE) {
            animate(interaction.position,active?1:0,170);
            const double progress=interaction.position.value(now);
            const int switchW=px(32),switchH=px(18);
            RECT r{rc.left+px(2),rc.top+(rc.bottom-switchH)/2,
                rc.left+px(2)+switchW,rc.top+(rc.bottom+switchH)/2};
            const COLORREF track=mix(mix(p.card,p.accent,progress),p.text,hover*0.025+push*0.025);
            const COLORREF switchBorder=mix(p.muted,p.accent,progress);
            rounded(dc,r,px(18),track,switchBorder);
            const float dotX=static_cast<float>(r.left+px(9)+px(14)*progress);
            const float centerY=static_cast<float>((r.top+r.bottom)/2);
            smoothCircle(dc,dotX,centerY,static_cast<float>(px(6)),mix(p.subtle,RGB(255,255,255),progress));
            text.left+=px(42);
        }
        drawTextAt(dc,label,text,monitorButton?boldFont_:font_,foreground,
            (id==ID_ENABLE || monitorButton ? DT_LEFT : DT_CENTER)|DT_VCENTER|DT_SINGLELINE);
        RestoreDC(dc,savedDC);
        if (paint) EndBufferedPaint(paint,TRUE);
    }
    void updateTrayTooltip() {
        if(!trayReady_)return;
        notify_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
        wcscpy_s(notify_.szTip,L"NightLight");
        Shell_NotifyIconW(NIM_MODIFY,&notify_);
    }
    void refreshConnectedDisplays() {
        finishInlineRename(true);commitDrag();endHardwareDrag();
        const std::wstring selectedDevice=hasMonitor()?selectedMonitor()->device:L"";
        const bool changed=model_.refreshMonitors();
        if(changed) {
            // Keep selection tied to the physical display if discovery reorders tabs.
            const auto& monitors=model_.monitors();
            const auto previous=std::find_if(monitors.begin(),monitors.end(),[&](const Monitor& monitor){return monitor.device==selectedDevice;});
            selected_=previous!=monitors.end()?static_cast<size_t>(previous-monitors.begin()):0;
            finishInlineRename(true);
        }
        refreshTabs();refreshHardware();
        if(changed)restoreHardware();
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
        const int span=std::max(1,static_cast<int>(slider_.right-slider_.left));
        int k=1000+static_cast<int>(std::round(
            std::clamp(x-static_cast<int>(slider_.left),0,span)*5500.0/span/10.0))*10;
        k=std::clamp(k,1000,6500);
        if (save) model_.updateKelvin(selected_,k);
        else if (selectedMonitor()->kelvin != k) model_.previewKelvin(selected_,k);
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
    void populateStartup(int idx) {
        Preferences pref=model_.preferences();
        pref.startup=idx==2?L"tray":idx==1?L"minimized":L"normal";
        model_.updatePreferences(pref);
        updateLanguage();
    }
    void populateLanguage(int idx) {
        Preferences pref=model_.preferences();
        pref.locale=idx==0?L"pt-PT":L"en-US";
        model_.updatePreferences(pref);
        updateLanguage();
        updateTheme();
    }
    void setTheme(bool dark) {
        Preferences pref=model_.preferences();
        pref.theme=dark?L"dark":L"light";
        model_.updatePreferences(pref);
        updateLanguage();
        updateTheme();
    }
    void openWindow() {
        syncGammaButton();
        updateMotionPreference();
        const bool reveal=!IsWindowVisible(window_) || IsIconic(window_);
        if (reveal && animations_) {
            const auto style=GetWindowLongPtrW(window_,GWL_EXSTYLE);
            entranceAddedLayer_=(style&WS_EX_LAYERED)==0;
            SetWindowLongPtrW(window_,GWL_EXSTYLE,style|WS_EX_LAYERED);
            if (SetLayeredWindowAttributes(window_,0,0,LWA_ALPHA)) {
                // Restore behind zero opacity so a minimized window never
                // flashes at full opacity or animates from its iconic bounds.
                ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOW);
                GetWindowRect(window_,&entranceBounds_);
                entranceMove_=!IsZoomed(window_);
                opening_=true; entrance_.snap(0); animate(entrance_,1,180);
                startAnimationTimer();
            } else {
                if (entranceAddedLayer_) SetWindowLongPtrW(window_,GWL_EXSTYLE,style);
                ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOW);
            }
        } else {
            ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOW);
        }
        SetForegroundWindow(window_);
    }
    bool addTray() {
        ZeroMemory(&notify_,sizeof(notify_));
        notify_.cbSize=sizeof(notify_);
        notify_.hWnd=window_;
        notify_.uID=TRAY_ID;
        notify_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
        notify_.uCallbackMessage=WM_TRAY_ICON;
        notify_.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(ICON_RESOURCE));
        wcscpy_s(notify_.szTip,L"NightLight");
        if (!Shell_NotifyIconW(NIM_ADD,&notify_)) return false;
        notify_.uVersion=NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION,&notify_);
        trayReady_=true;
        return true;
    }
    void removeTray() {
        if (trayReady_) Shell_NotifyIconW(NIM_DELETE,&notify_);
        trayReady_=false;
    }
    HMENU buildTrayMenu(std::vector<HBITMAP>& icons) {
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
        append(themes,MF_STRING|(model_.preferences().theme!=L"dark"?MF_CHECKED:0),ID_TRAY_THEME_BASE,tr(L"Light",L"Claro").c_str(),nightlight::MenuIcon::Light);
        append(themes,MF_STRING|(model_.preferences().theme==L"dark"?MF_CHECKED:0),ID_TRAY_THEME_BASE+1,tr(L"Dark",L"Escuro").c_str(),nightlight::MenuIcon::Dark);
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(themes),tr(L"Theme",L"Tema").c_str(),nightlight::MenuIcon::Theme);
        HMENU startup=createMenu();
        const auto& mode=model_.preferences().startup;
        append(startup,MF_STRING|(mode!=L"tray" && mode!=L"minimized"?MF_CHECKED:0),ID_TRAY_STARTUP_BASE,L"Normal",nightlight::MenuIcon::Window);
        append(startup,MF_STRING|(mode==L"minimized"?MF_CHECKED:0),ID_TRAY_STARTUP_BASE+1,tr(L"Minimized",L"Minimizado").c_str(),nightlight::MenuIcon::Minimize);
        append(startup,MF_STRING|(mode==L"tray"?MF_CHECKED:0),ID_TRAY_STARTUP_BASE+2,tr(L"Notification area",L"Área de notificação").c_str(),nightlight::MenuIcon::Tray);
        append(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(startup),tr(L"Start",L"Iniciar").c_str(),nightlight::MenuIcon::Start);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        append(menu,MF_STRING|(ddcVisible_?MF_CHECKED:0),ID_DDC,tr(L"DDC/CI Controls",L"Controlos DDC/CI").c_str(),nightlight::MenuIcon::Adjust);
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
        std::vector<HBITMAP> icons;
        HMENU menu=buildTrayMenu(icons);
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
        } else if (cmd==ID_DDC) {
            toggleHardware();
        } else if (cmd==ID_TRAY_LANGUAGE_BASE || cmd==ID_TRAY_LANGUAGE_BASE+1) {
            populateLanguage(cmd==ID_TRAY_LANGUAGE_BASE?1:0);
        } else if (cmd==ID_TRAY_THEME_BASE || cmd==ID_TRAY_THEME_BASE+1) {
            setTheme(cmd==ID_TRAY_THEME_BASE+1);
        } else if (cmd>=ID_TRAY_STARTUP_BASE && cmd<=ID_TRAY_STARTUP_BASE+2) {
            populateStartup(static_cast<int>(cmd-ID_TRAY_STARTUP_BASE));
        } else if (cmd>=ID_TRAY_DISABLE_BASE &&
                   cmd<ID_TRAY_DISABLE_BASE+model_.monitors().size()) {
            const size_t index=cmd-ID_TRAY_DISABLE_BASE;
            model_.updateEnabled(index,!model_.monitors()[index].enabled);
            updateTrayTooltip();
            InvalidateRect(btnEnable_,nullptr,FALSE);
            repaint();
        }
    }
    void handleCommand(int id,int code) {
        if (code==BN_CLICKED) {
            if (id==ID_REFRESH) { refreshTurn_.to(refreshTurn_.value(uiTime())+360.0,uiTime(),460,animations_ && IsWindowVisible(window_)); if(animations_)startAnimationTimer(); refreshConnectedDisplays(); }
            else if (id==ID_GAMMA) unlockGamma();
            else if (id==ID_ENABLE && hasMonitor()) {
                model_.updateEnabled(selected_,!selectedMonitor()->enabled);
                updateTrayTooltip();
                repaint();
                InvalidateRect(btnEnable_,nullptr,FALSE);
            }
            else if (id>=ID_MONITOR_BASE &&
                     id<ID_MONITOR_BASE+static_cast<int>(tabs_.size())) {
                select(static_cast<size_t>(id-ID_MONITOR_BASE));
            }
        }
    }
    LRESULT dispatch(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam) {
        if (taskbarCreated_ && msg==taskbarCreated_) {
            trayReady_=false;
            addTray();
            return 0;
        }
        switch (msg) {
        case WM_CREATE:
            initControls(); return 0;
        case WM_SIZE:
            if (wParam==SIZE_MINIMIZED) {
                if (trayReady_) {
                    ShowWindow(hwnd,SW_HIDE);
                }
            } else layout();
            return 0;
        case WM_CLOSE:
                if (trayReady_ && !shuttingDown_) ShowWindow(hwnd,SW_HIDE);
            else DestroyWindow(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* sizes=reinterpret_cast<MINMAXINFO*>(lParam);
            sizes->ptMinTrackSize.x=px(420);
            sizes->ptMinTrackSize.y=px(260);
            return 0;
        }
        case WM_DPICHANGED: {
                dpi_=HIWORD(wParam);
            const RECT* suggested=reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,
                suggested->right-suggested->left,suggested->bottom-suggested->top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            updateFonts(); layout(); return 0;
        }
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            updateMotionPreference(); updateTheme();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: {
            RECT client{}; GetClientRect(hwnd,&client);
            paintClient(reinterpret_cast<HDC>(wParam),client); return 0;
        }
        case WM_ENTERSIZEMOVE:
            finishEntrance();finishReordering(); hardwareReveal_.finish();
            sizeAnchorValid_=false; layout(); return 0;
        case WM_SHOWWINDOW:
            if (!wParam) {
                finishEntrance();finishReordering(); hardwareReveal_.finish(); layout();
                stopAnimationTimer();
                endHardwareDrag();finishScreenDrag();
                temperatureHover_=false; temperatureEngagement_.snap(0);
                for (auto& item:interactions_) { item.second.hover.snap(0); item.second.press.snap(0); }
                if (dragging_) commitDrag();
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
            const UINT event=LOWORD(lParam);
            if(event==WM_CONTEXTMENU || event==WM_RBUTTONUP)popupTray();
            else if(event==WM_LBUTTONDBLCLK || event==WM_LBUTTONUP || event==NIN_SELECT || event==NIN_KEYSELECT)openWindow();
            return 0;
        }
        case WM_SHOW_EXISTING: openWindow(); return 0;
        case nightlight::WM_DDC_COMPLETE:
            syncHardware(); repaint(); return 0;
        case WM_HSCROLL: {
            HWND slider=reinterpret_cast<HWND>(lParam);
            if (auto* row=hardwareRow(slider)) {
                const bool contrast=slider==row->contrast;
                const int percent=(static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0))+50)/100;
                if (ddcVisible_ && ddc_.set(row->device,contrast?nightlight::DdcFeature::Contrast:nightlight::DdcFeature::Brightness,percent)) {
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
            OffsetRect(&touch,0,-scrollOffset_);
            if (PtInRect(&touch,p) && hasMonitor() && selectedMonitor()->enabled) {
                dragging_=true;
                SetCapture(hwnd);
                SetFocus(hwnd);
                setKelvinFromX(p.x,false);
            }
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof(info)}; info.fMask=SIF_ALL;
            GetScrollInfo(hwnd,SB_VERT,&info);
            int position=scrollOffset_;
            switch (LOWORD(wParam)) {
            case SB_LINEUP: position-=px(28); break;
            case SB_LINEDOWN: position+=px(28); break;
            case SB_PAGEUP: position-=height_; break;
            case SB_PAGEDOWN: position+=height_; break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: position=info.nTrackPos; break;
            case SB_TOP: position=0; break;
            case SB_BOTTOM: position=contentHeight_; break;
            }
            scrollOffset_=std::clamp(position,0,std::max(0,contentHeight_-height_));
            layout(); return 0;
        }
        case WM_MOUSEWHEEL:
            if (contentHeight_>height_) {
                scrollOffset_=std::clamp(scrollOffset_-GET_WHEEL_DELTA_WPARAM(wParam)*px(48)/WHEEL_DELTA,
                    0,std::max(0,contentHeight_-height_));
                layout(); return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if(screenDragArmed_){moveScreenDrag({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});return 0;}
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0}; TrackMouseEvent(&tracking);
            RECT target=nightlight::sliderHitRect(slider_,px(12),px(18));
            OffsetRect(&target,0,-scrollOffset_);
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
            if (msg==WM_DISPLAYCHANGE) { pageSurface_.reset(); for (auto& item:interactions_) item.second.surface.reset(); }
            scheduleDisplayRecovery();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam==PBT_POWERSETTINGCHANGE && lParam) {
                const auto* setting=reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (IsEqualGUID(setting->PowerSetting,displayStateGuid) && setting->DataLength==sizeof(DWORD)) {
                    DWORD state=0; CopyMemory(&state,setting->Data,sizeof(state));
                    displayAwake_=state!=0;
                    if (displayAwake_) scheduleDisplayRecovery();
                    else KillTimer(hwnd,TIMER_DISPLAYS);
                }
            } else if (wParam==PBT_APMRESUMEAUTOMATIC || wParam==PBT_APMRESUMESUSPEND) {
                displayAwake_=true; scheduleDisplayRecovery();
            } else if (wParam==PBT_APMSUSPEND) {
                displayAwake_=false; KillTimer(hwnd,TIMER_DISPLAYS);
            }
            return TRUE;
        case WM_TIMER:
            if (wParam==TIMER_ANIMATION) { if (animationTimerRunning_) tickAnimations(); return 0; }
            if (wParam==TIMER_VERIFY_COLOR) {
                if (displayAwake_ && !dragging_) model_.applyAll(nightlight::GammaRefresh::Verify);
                return 0;
            }
            if (wParam==TIMER_RESTORE_DDC) {
                bool adjusting=dragging_;
                for (const auto& row:hardwareRows_) if (GetCapture()==row.brightness || GetCapture()==row.contrast) adjusting=true;
                if (!adjusting) restoreHardware();
                return 0;
            }
            if (wParam==TIMER_SAVE_DDC) { KillTimer(hwnd,TIMER_SAVE_DDC); model_.save(); return 0; }
            if (wParam==TIMER_STATE) { syncGammaButton(); return 0; }
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
                if (!displayAwake_) return 0;
                if (model_.refreshMonitors()) refreshTabs();
                model_.applyAll(nightlight::GammaRefresh::Restore);
                refreshHardware(); restoreHardware();
                // Display drivers and DDC transports can become ready several
                // seconds apart. Retry beyond the first successful enumeration.
                const UINT delays[]{1000,2500,5000,10000};
                if (recoveryAttempt_<4) SetTimer(hwnd,TIMER_DISPLAYS,delays[recoveryAttempt_++],nullptr);
                updateTrayTooltip();
                repaint();
            }
            return 0;
        case WM_QUERYENDSESSION:
            commitDrag(); model_.save(); return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                model_.save();
                model_.restoreOriginals();
            }
            return 0;
        case WM_DESTROY:
            shuttingDown_=true;
            endHardwareDrag();finishScreenDrag();
            finishEntrance(); stopAnimationTimer();
            KillTimer(hwnd,TIMER_STATE);
            KillTimer(hwnd,TIMER_VERIFY_COLOR); KillTimer(hwnd,TIMER_RESTORE_DDC); KillTimer(hwnd,TIMER_DISPLAYS);
            if (displayPowerNotification_) { UnregisterPowerSettingNotification(displayPowerNotification_); displayPowerNotification_=nullptr; }
            KillTimer(hwnd,TIMER_GAMMA);
            KillTimer(hwnd,TIMER_SAVE_DDC);
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
        HWND existing=FindWindowW(CLASS_NAME,nullptr);
        if (existing) PostMessageW(existing,WM_SHOW_EXISTING,0,0);
        CloseHandle(mutex);
        return 0;
    }
    int exitCode=1;
    try {
        int argc=0;
        LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&argc);
        std::wstring override;
        if (args) {
            for (int i=1;i<argc;++i) {
                if (wcscmp(args[i],L"--tray")==0 || wcscmp(args[i],L"--normal")==0 ||
                    wcscmp(args[i],L"--minimized")==0) override=args[i];
            }
            LocalFree(args);
        }
        NativeApp app(instance,override);
        exitCode=app.run(show);
    } catch (const std::exception&) {
        MessageBoxW(nullptr,L"NightLight could not initialize.",
                    L"NightLight",MB_OK|MB_ICONERROR);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}

