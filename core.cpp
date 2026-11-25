#include "core.hpp"
#include "display_identity.hpp"
#include "ddc.hpp"
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <array>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <climits>
#include <stdexcept>
#include <set>
#include <utility>

namespace nightlight {
namespace fs = std::filesystem;

bool shouldCorrectScreenWake(std::uint64_t nowMs,std::uint64_t previousMs,
                             unsigned correctionCount) {
    if (correctionCount>=kScreenOffMaxCorrections) return false;
    // Non-monotonic clocks are treated as ineligible for another correction.
    if (previousMs!=0 &&
        (nowMs<previousMs ||
         nowMs-previousMs<kScreenOffMinCorrectionIntervalMs)) return false;
    return true;
}

std::wstring fromUtf8(const std::string& input) {
    if (input.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.c_str(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(),
        static_cast<int>(input.size()), result.data(), size);
    return result;
}
std::string toUtf8(const std::wstring& input) {
    if (input.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.c_str(),
        static_cast<int>(input.size()), result.data(), size, nullptr, nullptr);
    return result;
}
bool systemAppsUseDarkTheme() {
    // The Windows *app* theme is independent of the taskbar/system theme.
    // A missing or unreadable value defaults to Windows' usual light mode.
    constexpr wchar_t key[]=
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD useLight=1, type=0, bytes=sizeof(useLight);
    HKEY handle=nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,key,0,KEY_QUERY_VALUE,&handle)!=ERROR_SUCCESS)
        return false;
    const LSTATUS result=RegQueryValueExW(handle,L"AppsUseLightTheme",
        nullptr,&type,reinterpret_cast<LPBYTE>(&useLight),&bytes);
    RegCloseKey(handle);
    return result==ERROR_SUCCESS && type==REG_DWORD &&
        bytes==sizeof(useLight) && useLight==0;
}
bool effectiveDarkTheme(const std::wstring& preference,bool systemDark) {
    return preference==L"dark" ||
        (preference==L"system" && systemDark);
}
fs::path executableDirectory() {
    std::vector<wchar_t> path(32768, 0);
    const DWORD written = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (!written || written >= path.size())
        throw std::runtime_error("Cannot discover executable folder");
    return fs::path(std::wstring(path.data(), written)).parent_path();
}
ColorGains kelvinGains(int kelvin) {
    const double k = std::clamp(kelvin, 1000, 6500);
    if (k == 6500) return {1, 1, 1};
    const double t = k / 100.0;
    const double d65 = (99.4708025861 * std::log(65.0) - 161.1195681661) / 255.0;
    double green = (99.4708025861 * std::log(std::max(t, 19.0)) - 161.1195681661) / 255.0 / d65;
    if (k < 1900) green *= std::pow((k - 1000.0) / 900.0, 1.2);
    const double blue = t <= 19 ? 0 :
        (138.5177312231 * std::log(t - 10) - 305.0447927307) / 255.0;
    return {1, std::clamp(green, 0.0, 1.0), std::clamp(blue, 0.0, 1.0)};
}
DWORD hardwareGainTarget(DWORD minimum,DWORD baseline,double factor) {
    if (baseline<=minimum) return minimum;
    return minimum+static_cast<DWORD>(std::llround((baseline-minimum)*std::clamp(factor,0.0,1.0)));
}
// CSC works in linear light. Convert the existing encoded white point to
// linear RGB so the nominal white chromaticity follows the Kelvin slider.
double nvidiaLinearGain(double encodedGain) {
    const double x=std::clamp(encodedGain,0.0,1.0);
    return x<=0.04045?x/12.92:std::pow((x+0.055)/1.055,2.4);
}
std::wstring monitorTooltip(const std::vector<Monitor>& monitors) {
    std::wstring text = L"NightLight";
    for (const auto& m : monitors) {
        text += L"\n" + m.displayName() + L": " + std::to_wstring(m.kelvin) + L" K";
        if (m.hardwareColor) text += L" [DDC/CI]";
        if (m.nvidiaColor) text += L" [NVIDIA CSC]";
        if (!m.enabled) text += L" (off)";
        else if (m.gammaStatus == 0) text += L" (unavailable)";
    }
    return text;
}
namespace {
constexpr wchar_t gammaKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";
}
bool gammaRangeUnlocked() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, gammaKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)) return false;
    DWORD type = 0, value = 0, size = sizeof(value);
    const LSTATUS status = RegQueryValueExW(key, L"GdiICMGammaRange", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && value == 256;
}
DWORD unlockGammaRange() {
    HKEY key = nullptr;
    DWORD result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, gammaKey, 0, nullptr, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    const DWORD value = 256;
    result = RegSetValueExW(key, L"GdiICMGammaRange", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result;
}
Ramp buildKelvinRamp(int kelvin) {
    const auto gains = kelvinGains(kelvin);
    const double redGain = gains.red, greenGain = gains.green, blueGain = gains.blue;
    Ramp ramp{};
    for (int i = 0; i < 256; ++i) {
        const double value = static_cast<double>(i) * 257.0;
        ramp.red[i] = static_cast<WORD>(std::round(value * redGain));
        ramp.green[i] = static_cast<WORD>(std::round(value * greenGain));
        ramp.blue[i] = static_cast<WORD>(std::round(value * blueGain));
    }
    return ramp;
}

Ramp buildAppliedRamp(int kelvin, bool enabled, const Ramp* originalRamp) {
    // Never restore the captured original when the filter is ENABLED and the
    // slider reaches 6500 K. The old behaviour was the 6500K-is-warmer bug:
    // a previously tinted/calibrated original could be re-applied at 6500K.
    //
    // Only disabling the filter (or exiting the app) restores that original.
    if (!enabled && originalRamp) return *originalRamp;
    return buildKelvinRamp(enabled ? kelvin : 6500);
}

int sliderPositionFromX(int x,int left,int right,int minimum,int maximum) {
    if (right<=left || maximum<=minimum) return minimum;
    const auto span=static_cast<std::int64_t>(right)-left;
    const auto offset=std::clamp<std::int64_t>(static_cast<std::int64_t>(x)-left,0,span);
    return minimum+static_cast<int>((offset*(static_cast<std::int64_t>(maximum)-minimum)+span/2)/span);
}
RECT sliderHitRect(RECT visualTrack, int horizontalPadding, int verticalPadding) {
    InflateRect(&visualTrack, std::max(0, horizontalPadding),
                std::max(0, verticalPadding));
    return visualTrack;
}

bool sameRamp(const Ramp& a, const Ramp& b) {
    for (int i = 0; i < 256; ++i)
        if (a.red[i] != b.red[i] || a.green[i] != b.green[i] ||
            a.blue[i] != b.blue[i]) return false;
    return true;
}
namespace {
std::wstring envValue(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return L"";
    std::wstring value(needed, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), needed);
    if (length >= needed) return L"";
    value.resize(length);
    return value;
}
fs::path roamingConfig() {
    PWSTR folder = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT,
                                        nullptr, &folder)) && folder) {
        result = fs::path(folder) / L"NightLight" / L"config.json";
        CoTaskMemFree(folder);
    } else {
        const std::wstring appdata = envValue(L"APPDATA");
        if (!appdata.empty()) result = fs::path(appdata) / L"NightLight" / L"config.json";
    }
    return result;
}
bool readWholeFile(const fs::path& file, std::string& out) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(handle); return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD received = 0;
    const BOOL ok = ReadFile(handle, out.data(), static_cast<DWORD>(out.size()),
                              &received, nullptr);
    CloseHandle(handle);
    if (!ok || received != out.size()) return false;
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return true;
}
bool atomicWrite(const fs::path& path, const std::string& data) {
    fs::path tmp = path; tmp += L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()),
        &written, nullptr);
    if (ok) ok = FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || written != data.size() ||
        !MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
struct EnumerateContext {
    std::vector<Monitor> entries;
    const DisplayTargetSnapshot* targets=nullptr;
    bool complete=true;
};
BOOL CALLBACK enumerateMonitors(HMONITOR hMon, HDC, LPRECT, LPARAM user) {
    auto* context = reinterpret_cast<EnumerateContext*>(user);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMon, &info)) { context->complete=false; return FALSE; }
    Monitor monitor;
    monitor.device = info.szDevice;
    monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitor.windowsName = monitor.device;
    for (DWORD index = 0; index < 32; ++index) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(info.szDevice, index, &dd, 0)) break;
        if (dd.DeviceString[0]) {
            monitor.windowsName = dd.DeviceString;
            if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) break;
        }
    }
    if (context->targets) {
        const auto found=context->targets->bySource.find(
            normalizedDisplayId(monitor.device));
        if (found!=context->targets->bySource.end() &&
            found->second.size()==1) {
            monitor.physicalId=found->second.front().physicalId;
            if (!found->second.front().friendlyName.empty())
                monitor.windowsName=found->second.front().friendlyName;
        } else {
            // Clone/mirror sources and unfinished target enumeration cannot
            // be safely attached to one unique physical monitor profile.
            context->complete=false;
        }
    }
    context->entries.push_back(std::move(monitor));
    return TRUE;
}
int profileDefaultKelvin(const portable::Json& root) {
    return std::clamp(root.get("screen").get("kelvin").integer(5000), 1000, 6500);
}
}

bool updateGammaRamp(const Ramp& target,GammaRefresh refresh,GammaRampState& state,
    const std::function<bool(Ramp&)>& read,const std::function<bool(const Ramp&)>& write) {
    auto matches=[&](const Ramp& actual) {
        for (int i=0;i<256;++i)
            if (std::abs(int(actual.red[i])-int(target.red[i]))>768 ||
                std::abs(int(actual.green[i])-int(target.green[i]))>768 ||
                std::abs(int(actual.blue[i])-int(target.blue[i]))>768) return false;
        return true;
    };
    bool needed=refresh==GammaRefresh::Restore || !state.valid || !sameRamp(state.ramp,target);
    if (!needed && refresh==GammaRefresh::Verify) {
        Ramp actual{};
        needed=!read(actual) || !matches(actual);
    }
    if (!needed) return true;
    bool ok=write(target);
    Ramp actual{};
    if (ok && read(actual)) ok=matches(actual);
    state.valid=ok;
    if (ok) state.ramp=target;
    return ok;
}

namespace {
// Keep the Kelvin-to-RGB calibration untouched. This exponent only changes
// how physical slider distance is distributed across the same 1000-6500 K
// range: the cool end becomes more responsive when dragged toward warm.
constexpr double kWarmBiasedSliderExponent=1.25;
}
int kelvinFromTemperatureSliderX(int x,int left,int right) {
    const int span=right-left;
    if(span<=0) return 1000;
    const double position=std::clamp(
        (static_cast<double>(x)-left)/span,0.0,1.0);
    const double progress=std::pow(position,kWarmBiasedSliderExponent);
    const int result=1000+static_cast<int>(std::lround(progress*550.0))*10;
    return std::clamp(result,1000,6500);
}
int temperatureSliderXFromKelvin(int kelvin,int left,int right) {
    const int span=right-left;
    if(span<=0) return left;
    const double kelvinProgress=
        std::clamp((kelvin-1000)/5500.0,0.0,1.0);
    const double position=std::pow(kelvinProgress,
        1.0/kWarmBiasedSliderExponent);
    return left+static_cast<int>(std::lround(position*span));
}

int windowOpeningSlideOffset(int dpi,double progress) {
    // Both position and alpha sample the same progress curve. A zero
    // offset at progress=1 returns the exact saved/resting window position.
    const int distance=MulDiv(kWindowOpeningSlideDip,std::max(96,dpi),96);
    return static_cast<int>(std::lround(distance*
        (1.0-std::clamp(progress,0.0,1.0))));
}

int temperatureSliderWidth(int dpi,int clientWidth) {
    const auto px=[dpi](int value) { return MulDiv(value,std::max(96,dpi),96); };
    // Keep the visual track 20 DIP inside each card. The card itself now
    // has 16-DIP outer margins, so each endpoint is 36 DIP from the window.
    return std::max(px(16),clientWidth-2*px(kCardOuterMarginDip+20));
}
TemperatureScaleLabelRects temperatureScaleLabelRects(
    const RECT& sliderTrack,int top,int height) {
    const int midpoint=sliderTrack.left+(sliderTrack.right-sliderTrack.left)/2;
    return {
        RECT{sliderTrack.left,top,midpoint,top+height},
        RECT{midpoint,top,sliderTrack.right,top+height}
    };
}

RECT temperatureValueRect(const RECT& sliderTrack,int desiredWidth,int top,int height) {
    const int right=sliderTrack.right;
    return RECT{std::max(static_cast<int>(sliderTrack.left),
        right-std::max(1,desiredWidth)),top,right,top+height};
}
bool TrayClickFilter::shouldToggle(TrayClickEvent event,unsigned long timestamp) {
    switch (event) {
    case TrayClickEvent::MouseDown:
    case TrayClickEvent::MouseDoubleClick:
        // Windows reports the second physical press as WM_LBUTTONDBLCLK,
        // not another WM_LBUTTONDOWN. Both presses must toggle immediately.
        mousePressPending=true;
        recentRelease=false;
        return true;
    case TrayClickEvent::MouseUp:
        releaseTime=timestamp;
        recentRelease=true;
        if (mousePressPending) {
            mousePressPending=false;
            return false; // Release of the press we already processed.
        }
        return true; // Legacy shells that report only mouse-up.
    case TrayClickEvent::Select:
        if (mousePressPending) {
            // Some shells send NIN_SELECT before the corresponding mouse-up.
            // Do not consume the pending release.
            return false;
        }
        if (recentRelease && (timestamp-releaseTime)<=150UL) {
            recentRelease=false; // The same mouse-up produced NIN_SELECT.
            return false;
        }
        recentRelease=false;
        return true; // Shells that report only NIN_SELECT.
    case TrayClickEvent::KeySelect:
        return true;
    }
    return false;
}

POINT ddcSliderIconCenter(int iconColumnCenterX,int sliderTop,int sliderHeight) {
    return POINT{iconColumnCenterX,sliderTop+sliderHeight/2};
}

RECT ddcPercentageRect(int dpi,int clientWidth,int maxPercentTextWidth,
                       int top,int height) {
    // Match the temperature gradient's exact computed edge, including odd
    // client widths and DPI rounding, rather than approximating with padding.
    const int temperatureWidth=temperatureSliderWidth(dpi,clientWidth);
    const int right=(clientWidth-temperatureWidth)/2+temperatureWidth;
    const int left=right-std::max(1,maxPercentTextWidth);
    return RECT{left,top,right,top+height};
}
int ddcSliderWidth(int dpi,int clientWidth,int maxPercentTextWidth) {
    const auto px=[dpi](int value) { return MulDiv(value,std::max(96,dpi),96); };
    const int sliderLeft=px(kCardOuterMarginDip+kCardInnerInsetDip+
                            kDdcSliderLeftInsetDip);
    // The trackbar HWND ends at the percentage text box's left edge.
    // Since the native track is inset inside the HWND, this preserves its
    // existing small visual gap before the percentage characters.
    const RECT percent=ddcPercentageRect(dpi,clientWidth,maxPercentTextWidth,0,1);
    return std::max(px(16),static_cast<int>(percent.left)-sliderLeft);
}

WindowContentMinimum calculateWindowContentMinimum(int dpi, int clientWidth,
    const std::vector<int>& tabDesiredWidths, size_t ddcMonitorRows,
    bool gammaUnlocked, bool ddcVisible) {
    const auto px=[dpi](int value) { return MulDiv(value,std::max(96,dpi),96); };
    const int tabGap=px(6);
    const int tabStart=px(kCardOuterMarginDip+kCardInnerInsetDip);
    // Width is user-controlled down to an outer-frame minimum of 200 DIP.
    // Never enlarge the window to fit labels: instead ellipsize monitor tabs
    // when they cannot fit within a narrow client area.
    const int minimumWidth=px(1);
    const int width=std::max(clientWidth,minimumWidth);
    const int tabsRight=width-px(kCardOuterMarginDip+kCardInnerInsetDip);
    const int availableTabWidth=std::max(px(1),tabsRight-tabStart);
    int x=tabStart, rows=1;
    for (int desired:tabDesiredWidths) {
        const int tabWidth=std::min(std::max(px(108),desired),availableTabWidth);
        if (x>tabStart && x+tabWidth>tabsRight) { x=tabStart; ++rows; }
        x+=tabWidth+tabGap;
    }
    // The screen-off button follows all monitor tabs. Count its wrap in the
    // required minimum height, exactly as in NativeApp::layout(), so a narrow
    // window never clips the last row or DDC controls.
    if (x>tabStart && x+px(kScreenOffButtonSizeDip)>tabsRight) ++rows;
    const int panelTop=px(kCardsTopMarginDip);
    const int panelHeight=px(46+rows*38);
    const int secondPanelTop=panelTop+panelHeight+px(kCardsGapDip);
    const int temperatureHeight=px((gammaUnlocked?
        kTemperatureUnlockedHeightDip:kTemperatureLockedHeightDip)+
        (width<px(350)?kTemperatureNarrowExtraDip:0));
    const int hardwareHeight=px(ddcMonitorRows==0?72:
        kDdcSectionBaseDip+kDdcRowStepDip*
            static_cast<int>(std::min<size_t>(ddcMonitorRows,100)));
    const int height=secondPanelTop+temperatureHeight+px(kCardOuterMarginDip)+
        (ddcVisible?hardwareHeight+px(kCardsGapDip):0);
    // Width remains user-controlled; only height is content-constrained.
    return {width,height,rows};
}

Model::Model() : Model(executableDirectory()) {}
Model::Model(fs::path directory) :
    directory_(std::move(directory)), configPath_(directory_ / L"config.json") {
    // All potentially slow SetDeviceGammaRamp calls run here, not in the
    // window's input/paint thread. Pending updates for a monitor are
    // coalesced so slider dragging always targets the newest Kelvin.
    gammaEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    try { gammaThread_ = std::thread(&Model::gammaLoop, this); }
    catch (...) { if (gammaEvent_) CloseHandle(gammaEvent_); gammaEvent_=nullptr; throw; }
}
Model::~Model() { save(); restoreOriginals(); }

bool Model::load() {
    lastError_.clear();
    loaded_=false; savedStampValid_=false; windowPosition_.reset(); windowSize_.reset();
    std::error_code ec;
    const bool exists=fs::exists(configPath_,ec);
    if (ec) { lastError_=L"Cannot access config.json; your file has not been modified."; return false; }
    if (!exists) {
        const fs::path legacy = roamingConfig();
        if (!legacy.empty() && fs::exists(legacy, ec)) {
            // Never remove or overwrite the old roaming settings.
            fs::copy_file(legacy, configPath_, fs::copy_options::skip_existing, ec);
        }
    }
    std::string data;
    if (readWholeFile(configPath_, data)) {
        try {
            root_ = portable::Json::parse(data);
            if (!root_.isObject()) throw std::runtime_error("Root JSON is not an object");
        } catch (const std::exception&) {
            // A corrupt user file is preserved for recovery; never silently
            // erase it when the user changes a slider.
            lastError_ = L"Config contains invalid JSON; your file has not been modified.";
            loaded_ = false;
            return false;
        }
    } else {
        const bool present=fs::exists(configPath_,ec);
        if (ec || present) { lastError_=L"Cannot read config.json; your file has not been modified."; return false; }
        root_ = portable::Json::object();
        root_["screen"]["kelvin"] = 5000;
        root_["screen"]["monitor_profiles"] = portable::Json::object();
    }
    const auto& general = root_.get("general");
    const std::string lang = general.get("language").str("en-US");
    preferences_.locale = (lang == "pt" || lang == "pt-PT") ? L"pt-PT" : L"en-US";
    const std::string appearance=general.get("appearance").str("system");
    preferences_.theme=appearance=="light"?L"light":
        appearance=="dark"?L"dark":L"system";
    preferences_.closeByClickingOutside = general.get("close_by_clicking_outside").boolean(true);
    const auto& position=general.get("window_position");
    if (position.isObject()) {
        const int x=position.get("x").integer(INT_MIN);
        const int y=position.get("y").integer(INT_MIN);
        if (x!=INT_MIN && y!=INT_MIN &&
            x>=-1000000 && x<=1000000 && y>=-1000000 && y<=1000000)
            windowPosition_=POINT{x,y};
        const int width=position.get("width").integer(-1);
        const int height=position.get("height").integer(-1);
        if (width>=200 && width<=10000 && height>=260 && height<=10000)
            windowSize_=SIZE{width,height};
    }
    loaded_ = true;
    // Read legacy profiles once, then retain only settings this app implements.
    if (!root_.get("screen").get("monitor_profiles").isObject())
        root_["screen"]["monitor_profiles"] = portable::Json::object();
    save();
    migrateAutorunEntry();
    return true;
}
std::wstring Model::profileKeyForDevice(const std::wstring& device) const {
    const auto found=std::find_if(monitors_.begin(),monitors_.end(),
        [&](const Monitor& m){return m.device==device;});
    return found==monitors_.end()?device:monitorProfileKey(*found);
}
bool Model::hasHardwarePercent(const std::wstring& device,bool contrast) const {
    return root_.get("screen").get("monitor_profiles").get(toUtf8(profileKeyForDevice(device)))
        .get(contrast?"ddc_contrast":"ddc_brightness").integer(-1)>=0;
}
int Model::hardwarePercent(const std::wstring& device,bool contrast) const {
    return std::clamp(root_.get("screen").get("monitor_profiles").get(toUtf8(profileKeyForDevice(device)))
        .get(contrast?"ddc_contrast":"ddc_brightness").integer(50),0,100);
}
bool Model::rememberHardwarePercent(const std::wstring& device,bool contrast,int percent) {
    if (!loaded_ || device.empty()) return false;
    percent=std::clamp(percent,0,100);
    auto& profile=root_["screen"]["monitor_profiles"][toUtf8(profileKeyForDevice(device))];
    const char* key=contrast?"ddc_contrast":"ddc_brightness";
    if (profile.get(key).integer(-1)==percent) return false;
    profile[key]=percent;
    return true;
}
bool Model::rememberWindowPosition(int x,int y) {
    if (!loaded_ || x < -1000000 || x > 1000000 ||
        y < -1000000 || y > 1000000) return false;
    if (windowPosition_ && windowPosition_->x==x && windowPosition_->y==y) return false;
    windowPosition_=POINT{x,y};
    return true;
}
bool Model::rememberWindowSize(int width,int height) {
    if (!loaded_ || width<200 || width>10000 || height<260 || height>10000)
        return false;
    if (windowSize_ && windowSize_->cx==width && windowSize_->cy==height)
        return false;
    windowSize_=SIZE{width,height};
    return true;
}
bool Model::save() {
    if (!loaded_) return false;
    root_["general"]["language"] = toUtf8(preferences_.locale);
    root_["general"]["appearance"] = toUtf8(preferences_.theme);
    root_["general"]["close_by_clicking_outside"] = preferences_.closeByClickingOutside;
    for (const auto& mon : monitors_) {
        auto& profile = root_["screen"]["monitor_profiles"][toUtf8(monitorProfileKey(mon))];
        profile["kelvin"] = mon.kelvin;
        profile["enabled"] = mon.enabled;
        profile["custom_name"] = toUtf8(mon.alias);
        if (mon.hardwareColor) profile["hardware_color"] = true;
        else profile["hardware_color"] = false;
        profile["nvidia_color"] = mon.nvidiaColor;
    }
    // Persist only settings implemented by this version. Keep profiles for
    // disconnected displays so their current targets survive reconnects.
    auto clean=portable::Json::object();
    clean["screen"]["kelvin"]=std::clamp(root_.get("screen").get("kelvin").integer(5000),1000,6500);
    clean["general"]=portable::Json::object();
    for(const char* key:{"language","appearance","close_by_clicking_outside"})
        clean["general"][key]=root_.get("general").get(key);
    if (windowPosition_) {
        clean["general"]["window_position"]["x"]=windowPosition_->x;
        clean["general"]["window_position"]["y"]=windowPosition_->y;
    }
    if (windowSize_) {
        clean["general"]["window_position"]["width"]=windowSize_->cx;
        clean["general"]["window_position"]["height"]=windowSize_->cy;
    }
    auto profiles=portable::Json::object();
    if(auto values=std::get_if<portable::Json::Object>(&root_.get("screen").get("monitor_profiles").value))
        for(const auto& item:*values) {
            auto profile=portable::Json::object();
            for(const char* key:{"kelvin","enabled","custom_name","ddc_brightness","ddc_contrast","hardware_color","nvidia_color"}) {
                const auto& value=item.second.get(key);
                if(!std::holds_alternative<std::nullptr_t>(value.value))profile[key]=value;
            }
            if(!std::get<portable::Json::Object>(profile.value).empty())profiles[item.first]=profile;
        }
    clean["screen"]["monitor_profiles"]=profiles;
    if(root_.get("screen").get("monitor_order").isArray())
        clean["screen"]["monitor_order"]=root_.get("screen").get("monitor_order");
    root_=std::move(clean);
    const std::string document=root_.dump();
    WIN32_FILE_ATTRIBUTE_DATA current{};
    if (savedStampValid_ && document==savedDocument_ &&
        GetFileAttributesExW(configPath_.c_str(),GetFileExInfoStandard,&current) &&
        current.nFileSizeHigh==0 && current.nFileSizeLow==document.size() &&
        CompareFileTime(&current.ftLastWriteTime,&savedStamp_)==0) {
        lastError_.clear(); return true;
    }
    if (!atomicWrite(configPath_, document)) {
        lastError_ = L"Cannot write config.json beside the EXE. Choose a writable folder.";
        return false;
    }
    savedDocument_=document;
    savedStampValid_=GetFileAttributesExW(configPath_.c_str(),GetFileExInfoStandard,&current)!=FALSE;
    if (savedStampValid_) savedStamp_=current.ftLastWriteTime;
    lastError_.clear();
    return true;
}
void Model::readProfile(Monitor& m) const {
    const auto& profiles=root_.get("screen").get("monitor_profiles");
    const auto& physical=profiles.get(toUtf8(monitorProfileKey(m)));
    // Migrate existing DISPLAYx profiles once; physical IDs take priority
    // from then on, including after Windows renumbers outputs.
    const auto& profile=physical.isObject()?physical:
        profiles.get(toUtf8(m.device));
    m.enabled = profile.get("enabled").boolean(true);
    m.kelvin = std::clamp(
        profile.get("kelvin").integer(profileDefaultKelvin(root_)), 1000, 6500);
    m.alias = fromUtf8(profile.get("custom_name").str());
    m.nvidiaColor = profile.get("nvidia_color").boolean(false);
    m.hardwareColor = !m.nvidiaColor && profile.get("hardware_color").boolean(false);
}
static bool sameDisplaySnapshot(const std::vector<Monitor>& a,
                                const std::vector<Monitor>& b) {
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i)
        if (a[i].device!=b[i].device ||
            a[i].physicalId!=b[i].physicalId ||
            a[i].windowsName!=b[i].windowsName ||
            a[i].primary!=b[i].primary) return false;
    return true;
}
bool Model::refreshMonitors(bool afterWake,bool* stable) {
    if (stable) *stable=false;
    const auto physical=queryPhysicalDisplayTargets();
    EnumerateContext context;
    context.targets=&physical;
    if (!EnumDisplayMonitors(nullptr,nullptr,enumerateMonitors,
                             reinterpret_cast<LPARAM>(&context)) ||
        context.entries.empty()) return false;
    auto& entries=context.entries;
    std::vector<std::wstring> order;
    if (auto saved=std::get_if<portable::Json::Array>(
            &root_.get("screen").get("monitor_order").value))
        for(const auto& value:*saved)order.push_back(fromUtf8(value.str()));
    // Even without an explicitly customized order, preserve the current
    // physical tab order across a GDI DISPLAY1/DISPLAY2 reassignment.
    for (const auto& old:monitors_) {
        const auto key=monitorProfileKey(old);
        if (std::find(order.begin(),order.end(),key)==order.end())
            order.push_back(key);
    }
    auto orderIndex=[&](const Monitor& monitor) {
        auto pos=std::find(order.begin(),order.end(),monitorProfileKey(monitor));
        if(pos==order.end())
            pos=std::find(order.begin(),order.end(),monitor.device);
        return static_cast<size_t>(pos-order.begin());
    };
    std::sort(entries.begin(),entries.end(),[&](const Monitor& a,const Monitor& b) {
        const auto ai=orderIndex(a),bi=orderIndex(b);
        if (ai!=bi) return ai<bi;
        return a.device<b.device;
    });
    if (afterWake) {
        // QueryDisplayConfig can report still-active-but-unavailable targets.
        // Never replace live monitors with such a partial/suspect snapshot.
        if (!physical.complete || !context.complete ||
            !wakeTopologyIsUsable(monitors_,entries)) {
            wakeCandidate_.clear();
            wakeConsistentSamples_=0;
            return false;
        }
        if (sameDisplaySnapshot(wakeCandidate_,entries))
            ++wakeConsistentSamples_;
        else {
            wakeCandidate_=entries;
            wakeConsistentSamples_=1;
        }
        // Two identical samples, on distinct timer ticks, are required.
        if (wakeConsistentSamples_<2) return false;
        if (stable) *stable=true;
    } else {
        wakeCandidate_.clear();
        wakeConsistentSamples_=0;
    }
    if (sameDisplaySnapshot(monitors_,entries)) return false;
    for(auto& mon:entries) {
        const auto old=std::find_if(monitors_.begin(),monitors_.end(),
            [&](const Monitor& previous) {
                return samePhysicalMonitor(previous,mon);
            });
        if (old==monitors_.end()) readProfile(mon);
        else {
            mon.kelvin=old->kelvin;
            mon.enabled=old->enabled;
            mon.alias=old->alias;
            mon.hardwareColor=old->hardwareColor;
            mon.nvidiaColor=old->nvidiaColor;
        }
    }
    monitors_=std::move(entries);
    applyAll();
    // Persist tab order with physical IDs even when the user never dragged
    // the tabs. Also migrate legacy GDI-based order entries while preserving
    // saved order for currently disconnected monitor profiles.
    if (physical.complete && context.complete) {
        auto migrated=portable::Json::array();
        auto& values=std::get<portable::Json::Array>(migrated.value);
        std::set<std::string> seen;
        if (auto saved=std::get_if<portable::Json::Array>(
                &root_.get("screen").get("monitor_order").value))
            for (const auto& item:*saved) {
                const auto original=fromUtf8(item.str());
                const auto found=std::find_if(monitors_.begin(),monitors_.end(),
                    [&](const Monitor& mon){return mon.device==original;});
                const auto key=found==monitors_.end()?item.str():
                    toUtf8(monitorProfileKey(*found));
                if (seen.insert(key).second) values.emplace_back(key);
            }
        for (const auto& monitor:monitors_) {
            const auto key=toUtf8(monitorProfileKey(monitor));
            if (seen.insert(key).second) values.emplace_back(key);
        }
        root_["screen"]["monitor_order"]=std::move(migrated);
        save();
    }
    return true;
}
bool Model::moveMonitor(size_t from,size_t to) {
    if(from>=monitors_.size() || to>=monitors_.size() || from==to)return false;
    Monitor moved=std::move(monitors_[from]);
    monitors_.erase(monitors_.begin()+from);
    monitors_.insert(monitors_.begin()+to,std::move(moved));
    auto order=portable::Json::array();
    auto& values=std::get<portable::Json::Array>(order.value);
    for(const auto& monitor:monitors_)values.emplace_back(toUtf8(monitorProfileKey(monitor)));
    if(auto saved=std::get_if<portable::Json::Array>(&root_.get("screen").get("monitor_order").value))
        for(const auto& value:*saved)
            if(std::none_of(values.begin(),values.end(),[&](const portable::Json& item){return item.str()==value.str();}))values.push_back(value);
    root_["screen"]["monitor_order"]=std::move(order);
    return true;
}
void Model::adoptOriginal(const std::wstring& device, HDC dc) {
    if (originals_.find(device) != originals_.end()) return;
    Ramp baseline{};
    if (GetDeviceGammaRamp(dc, &baseline)) originals_.emplace(device, baseline);
}
void Model::setStatusWindow(HWND window) {
    std::lock_guard<std::mutex> guard(gammaMutex_);
    statusWindow_ = window;
}

void Model::queueGamma(const Monitor& monitor, const Ramp& ramp, GammaRefresh refresh) {
    const auto& device=monitor.device;
    std::lock_guard<std::mutex> guard(gammaMutex_);
    if (gammaStopping_) return;
    const std::uint64_t generation = ++newestGeneration_[device];
    auto pending=pendingGamma_.find(device);
    if (pending!=pendingGamma_.end() && static_cast<int>(pending->second.refresh)>static_cast<int>(refresh))
        refresh=pending->second.refresh;
    pendingGamma_[device] = GammaJob{generation, ramp,refresh,monitor};
    gammaResults_.erase(device);
    if (gammaEvent_) SetEvent(gammaEvent_); else gammaWake_.notify_one();
}

// Resolve a fresh, unique physical target before any monitor writes.
static bool uniquePhysicalTarget(const Monitor& mon) {
    if (mon.physicalId.empty()) return false;
    const auto topology=queryPhysicalDisplayTargets();
    const auto it=topology.bySource.find(normalizedDisplayId(mon.device));
    return topology.complete && it!=topology.bySource.end() && it->second.size()==1 &&
        normalizedDisplayId(it->second.front().physicalId)==normalizedDisplayId(mon.physicalId);
}
struct HardwareFind { const std::wstring& device; HMONITOR handle=nullptr; };
static BOOL CALLBACK findHardwareDisplay(HMONITOR monitor,HDC,LPRECT,LPARAM param) {
    auto& find=*reinterpret_cast<HardwareFind*>(param);
    MONITORINFOEXW info{}; info.cbSize=sizeof(info);
    if (GetMonitorInfoW(monitor,&info) && find.device==info.szDevice) {
        find.handle=monitor; return FALSE;
    }
    return TRUE;
}
struct PhysicalColorHandle {
    PHYSICAL_MONITOR value{};
    bool valid=false;
    explicit PhysicalColorHandle(const Monitor& mon) {
        HardwareFind find{mon.device};
        EnumDisplayMonitors(nullptr,nullptr,findHardwareDisplay,reinterpret_cast<LPARAM>(&find));
        DWORD count=0;
        if (find.handle && GetNumberOfPhysicalMonitorsFromHMONITOR(find.handle,&count) && count==1)
            valid=GetPhysicalMonitorsFromHMONITOR(find.handle,1,&value)!=FALSE;
    }
    ~PhysicalColorHandle() { if(valid) DestroyPhysicalMonitors(1,&value); }
};
bool Model::applyHardwareColor(const Monitor& mon,bool active) {
    // Keep the original hardware gains on disk BEFORE modifying any channel.
    // Monitor firmware can retain its settings after a crash or power loss.
    // A later disable/exit/restart can restore from this recovery journal.
    const auto journalPath=directory_/L"hardware-color-backup.json";
    portable::Json journal=portable::Json::object();
    std::string document;
    if (readWholeFile(journalPath,document)) {
        try { journal=portable::Json::parse(document); }
        catch (...) { return false; }
        if (!journal.isObject()) return false;
    } else {
        const DWORD attrs=GetFileAttributesW(journalPath.c_str());
        if (attrs!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
    }
    const auto key=toUtf8(monitorProfileKey(mon));
    const auto& record=journal.get(key);
    const bool saved=record.isObject();
    if (!saved && !active) return true; // GPU mode normally does no DDC I/O.
    if (!uniquePhysicalTarget(mon)) return false;
    std::lock_guard<std::mutex> io(physicalDdcMutex());
    PhysicalColorHandle physical(mon);
    if (!physical.valid || !uniquePhysicalTarget(mon)) return false;
    const HANDLE handle=physical.value.hPhysicalMonitor;
    constexpr std::array<MC_GAIN_TYPE,3> channels{MC_RED_GAIN,MC_GREEN_GAIN,MC_BLUE_GAIN};
    constexpr std::array<const char*,3> keys{"red","green","blue"};
    std::array<DWORD,3> lo{},current{},hi{},original{},target{};
    for (size_t i=0;i<3;++i) {
        if (!GetMonitorRedGreenOrBlueGain(handle,channels[i],&lo[i],&current[i],&hi[i]) ||
            hi[i]<=lo[i] || current[i]<lo[i] || current[i]>hi[i] || hi[i]>65535) return false;
        if (saved) {
            const int previous=record.get(keys[i]).integer(-1);
            if (previous<0 || static_cast<DWORD>(previous)<lo[i] ||
                static_cast<DWORD>(previous)>hi[i]) return false;
            original[i]=static_cast<DWORD>(previous);
        } else original[i]=current[i];
    }
    if (!saved) {
        for (size_t i=0;i<3;++i) journal[key][keys[i]]=static_cast<int>(original[i]);
        if (!atomicWrite(journalPath,journal.dump())) return false;
    }
    const auto gains=kelvinGains(mon.kelvin);
    const std::array<double,3> factors{gains.red,gains.green,gains.blue};
    for (size_t i=0;i<3;++i)
        target[i]=active?hardwareGainTarget(lo[i],original[i],factors[i]):original[i];
    if (!uniquePhysicalTarget(mon)) return false;
    bool ok=true;
    for (size_t i=0;i<3;++i) {
        if (current[i]!=target[i] && !SetMonitorRedGreenOrBlueGain(handle,channels[i],target[i])) {
            ok=false; break;
        }
    }
    if (!ok) {
        // Roll back to the original calibration, never leave a partially
        // applied set of RGB values intentionally. Retain the recovery file.
        for (size_t i=0;i<3;++i)
            SetMonitorRedGreenOrBlueGain(handle,channels[i],original[i]);
        return false;
    }
    if (!active) {
        std::get<portable::Json::Object>(journal.value).erase(key);
        if (!atomicWrite(journalPath,journal.dump())) return false;
    }
    return true;
}

namespace {
// Private NVAPI CSC V1 wire layout, independently declared from the public
// interface observations at https://github.com/ledoge/novideo_srgb/blob/master/novideo_srgb/Novideo.cs
// Public NVAPI entry-point IDs: NVIDIA/nvapi, nvapi_interface.h.
// No NVIDIA SDK, bundled DLL, overlay, or process injection is required.
struct NvidiaCscV1 {
    std::uint32_t version=0x1007c, input=0, output=0, reservedA=0, reservedB=0, enableA=0;
    float a[12]{};
    std::uint32_t enableB=0;
    float b[12]{};
};
static_assert(sizeof(NvidiaCscV1)==0x7c,"NVAPI CSC V1 ABI size");
struct NvidiaCscApi {
    using Query=void* (__cdecl*)(std::uint32_t);
    using Init=int (__cdecl*)();
    using DisplayId=int (__cdecl*)(const char*,std::uint32_t*);
    using Csc=int (__cdecl*)(std::uint32_t,NvidiaCscV1*);
    HMODULE module=nullptr;
    Init unload=nullptr;
    DisplayId displayId=nullptr;
    Csc get=nullptr,set=nullptr;
    bool initialized=false;
    NvidiaCscApi() {
#if defined(_WIN64)
        module=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        module=LoadLibraryExW(L"nvapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
        if (!module) return;
        auto query=reinterpret_cast<Query>(GetProcAddress(module,"nvapi_QueryInterface"));
        if (!query) return;
        auto init=reinterpret_cast<Init>(query(0x0150e828));
        unload=reinterpret_cast<Init>(query(0xd22bdd7e));
        if (!init || init()!=0) return;
        initialized=true;
        displayId=reinterpret_cast<DisplayId>(query(0xae457190));
        get=reinterpret_cast<Csc>(query(0x8159e87a));
        set=reinterpret_cast<Csc>(query(0xfcabd23a));
    }
    ~NvidiaCscApi() {
        if (initialized && unload) unload();
        if (module) FreeLibrary(module);
    }
    bool available() const { return initialized && displayId && get && set; }
};
void logNvidiaStatus(const wchar_t* operation,int status) {
    const auto message=std::wstring(L"NightLight NVAPI ")+operation+L": "+std::to_wstring(status)+L"\n";
    OutputDebugStringW(message.c_str());
}
bool nvidiaRead(NvidiaCscApi& api,std::uint32_t id,NvidiaCscV1& value) {
    value=NvidiaCscV1{};
    const int status=api.get(id,&value);
    if (status!=0) logNvidiaStatus(L"GetColorSpaceConversion",status);
    return status==0;
}
bool nvidiaWrite(NvidiaCscApi& api,std::uint32_t id,NvidiaCscV1 value) {
    const int status=api.set(id,&value);
    if (status!=0) logNvidiaStatus(L"SetColorSpaceConversion",status);
    return status==0;
}
bool matchesNvidiaMatrix(const NvidiaCscV1& actual,const NvidiaCscV1& target) {
    if (actual.input!=target.input || actual.output!=target.output ||
        actual.enableA!=target.enableA || actual.enableB!=target.enableB) return false;
    if (target.enableA) for (size_t i=0;i<12;++i)
        if (!std::isfinite(actual.a[i]) || std::abs(actual.a[i]-target.a[i])>0.001f) return false;
    return true;
}
}
bool Model::applyNvidiaColor(const Monitor& mon,bool active) {
    // Only claim an otherwise unused CSC pipeline; do not overwrite an ICC
    // calibration, HDR transformation, or another application's active CSC.
    // Persist ownership before writing so crash recovery can disable our CSC.
    const auto journalPath=directory_/L"nvidia-color-backup.json";
    portable::Json journal=portable::Json::object();
    std::string document;
    if (readWholeFile(journalPath,document)) {
        try { journal=portable::Json::parse(document); }
        catch (...) { return false; }
        if (!journal.isObject()) return false;
    } else {
        const DWORD attrs=GetFileAttributesW(journalPath.c_str());
        if (attrs!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
    }
    const auto key=toUtf8(monitorProfileKey(mon));
    const auto& record=journal.get(key);
    const bool owned=record.isObject();
    if (!owned && !active) return true;
    if (!uniquePhysicalTarget(mon)) return false;
    static NvidiaCscApi api;
    if (!api.available()) { logNvidiaStatus(L"API unavailable",-104); return false; }
    std::uint32_t id=0;
    const auto name=toUtf8(mon.device);
    const int idStatus=api.displayId(name.c_str(),&id);
    if (idStatus!=0 || !id) { logNvidiaStatus(L"GetDisplayIdByDisplayName",idStatus); return false; }
    NvidiaCscV1 current;
    if (!nvidiaRead(api,id,current)) return false;
    // HDR uses color-space 12 in this private API. Never modify that mode.
    if (current.input==12 || current.output==12) return false;
    if (!owned && (current.output!=0 || current.enableA || current.enableB ||
        (current.input!=0 && current.input!=2))) {
        logNvidiaStatus(L"existing colour conversion; leaving unchanged",-104);
        return false;
    }
    // The only supported baseline is disabled CSC. Refuse a corrupt journal.
    if (owned && record.get("baseline_disabled").boolean(false)!=true) return false;
    // Refuse to overwrite an obviously foreign conversion even with an old
    // recovery marker. Our matrix is diagonal with zero offsets, input 2.
    if (owned && current.output!=0) {
        if (current.input!=2 || current.output!=2 || current.enableA!=1 || current.enableB) return false;
        for (size_t i=0;i<12;++i) {
            const float x=current.a[i];
            if (!std::isfinite(x)) return false;
            if (i==0 || i==5 || i==10) {
                if (x<0 || x>1.001f) return false;
            } else if (std::abs(x)>0.001f) return false;
        }
        if (std::abs(current.a[0]-1.0f)>0.001f) return false;
    }
    if (!owned) {
        journal[key]["baseline_disabled"]=true;
        if (!atomicWrite(journalPath,journal.dump())) return false;
    }
    NvidiaCscV1 target;
    target.input=2; // sRGB degamma
    if (active) {
        target.output=2; // sRGB regamma
        target.enableA=1;
        const auto gains=kelvinGains(mon.kelvin);
        target.a[0]=static_cast<float>(nvidiaLinearGain(gains.red));
        target.a[5]=static_cast<float>(nvidiaLinearGain(gains.green));
        target.a[10]=static_cast<float>(nvidiaLinearGain(gains.blue));
    }
    if (!uniquePhysicalTarget(mon)) return false;
    const bool same=active?matchesNvidiaMatrix(current,target):
        (current.output==0 && !current.enableA && !current.enableB);
    if (!same && !nvidiaWrite(api,id,target)) {
        // If the attempted activation failed, retain the recovery marker.
        // Never fall through to GDI gamma or change compositor settings.
        return false;
    }
    if (!active) {
        std::get<portable::Json::Object>(journal.value).erase(key);
        if (!atomicWrite(journalPath,journal.dump())) return false;
    }
    return true;
}

void Model::gammaLoop() {
    // Apply warm colour through each output's hardware LUT alone. A fullscreen
    // Magnification colour matrix affects the composited desktop and can tint
    // the pointer. The hardware cursor usually bypasses gamma ramp changes,
    // so its white pixels remain white (driver-dependent).
    std::map<std::wstring,GammaRampState> appliedRamps;
    while (true) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        std::map<std::wstring,GammaJob> batch;
        {
            std::lock_guard<std::mutex> guard(gammaMutex_);
            if (gammaStopping_) break;
            batch.swap(pendingGamma_);
        }
        if (batch.empty()) {
            if (gammaEvent_) {
                // Keep the existing message-aware worker wait so colour
                // updates never block the UI thread while idle.
                const DWORD result=MsgWaitForMultipleObjectsEx(1,&gammaEvent_,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if (result==WAIT_FAILED) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            } else {
                std::unique_lock<std::mutex> guard(gammaMutex_);
                gammaWake_.wait_for(guard,std::chrono::milliseconds(30),[this]{return gammaStopping_ || !pendingGamma_.empty();});
            }
            continue;
        }
        // Drain a complete frame fairly: a frequently dragged display must not
        // starve queued updates for the other displays.
        for (auto& item:batch) {
        const auto& device=item.first;
        auto& job=item.second;
        // This call can block for several frames on some graphics drivers;
        // the UI thread is never forced to wait for it.
        const auto key=monitorProfileKey(job.monitor);
        int result=0;
        {
            std::lock_guard<std::mutex> guard(gammaMutex_);
            if (gammaStopping_ || newestGeneration_[device]!=job.generation) continue;
        }
        if (job.monitor.hardwareColor || job.monitor.nvidiaColor) {
            // Restore the GPU calibration once on transition, but never
            // apply a warm GPU ramp (including verification) in hardware mode.
            bool neutral=true;
            if (gammaTouched_.count(key)) {
                neutral=uniquePhysicalTarget(job.monitor);
                HDC dc=neutral?CreateDCW(L"DISPLAY",device.c_str(),nullptr,nullptr):nullptr;
                neutral=dc && SetDeviceGammaRamp(dc,&job.ramp)!=FALSE;
                if (dc) DeleteDC(dc);
                if (neutral) { gammaTouched_.erase(key); appliedRamps.erase(device); }
            }
            if (neutral) {
                const bool active=job.monitor.enabled && job.monitor.kelvin<6500;
                if (job.monitor.nvidiaColor) {
                    if (applyHardwareColor(job.monitor,false))
                        result=applyNvidiaColor(job.monitor,active)?1:0;
                } else if (applyNvidiaColor(job.monitor,false)) {
                    result=applyHardwareColor(job.monitor,active)?1:0;
                }
            }
        } else if (applyNvidiaColor(job.monitor,false) && applyHardwareColor(job.monitor,false)) {
            HDC dc=CreateDCW(L"DISPLAY",device.c_str(),nullptr,nullptr);
            if (dc) {
                result=updateGammaRamp(job.ramp,job.refresh,appliedRamps[device],
                    [&](Ramp& actual) { return GetDeviceGammaRamp(dc,&actual)!=FALSE; },
                    [&](const Ramp& desired) {
                        gammaTouched_.insert(key);
                        return SetDeviceGammaRamp(dc,const_cast<Ramp*>(&desired))!=FALSE;
                    })?1:0;
                DeleteDC(dc);
            }
        }
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> guard(gammaMutex_);
            if (!gammaStopping_ && newestGeneration_[device] == job.generation) {
                gammaResults_[device] = result;
                notify = statusWindow_;
            }
        }
        // The UI will ignore stale results and paint the hardware status.
        if (notify) PostMessageW(notify, WM_APP + 41, 0, 0);
        }
        // Cap update pressure while still allowing approximately 60 Hz
        // gamma requests. Faster pointer events overwrite queued work.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

bool Model::collectGammaResults() {
    std::map<std::wstring, int> results;
    {
        std::lock_guard<std::mutex> guard(gammaMutex_);
        results.swap(gammaResults_);
    }
    bool changed = false;
    for (auto& monitor : monitors_) {
        auto result = results.find(monitor.device);
        if (result != results.end() && monitor.gammaStatus != result->second) {
            monitor.gammaStatus = result->second;
            changed = true;
        }
    }
    return changed;
}

void Model::stopGamma() {
    {
        std::lock_guard<std::mutex> guard(gammaMutex_);
        gammaStopping_ = true;
        statusWindow_ = nullptr;
        pendingGamma_.clear();
    }
    if (gammaEvent_) SetEvent(gammaEvent_); else gammaWake_.notify_all();
    if (gammaThread_.joinable()) gammaThread_.join();
    if (gammaEvent_) { CloseHandle(gammaEvent_); gammaEvent_=nullptr; }
}

bool Model::applyDisplay(Monitor& mon,GammaRefresh refresh) {
    // Capture the pre-NightLight ramp on this (infrequent) monitor-discovery
    // path. Never read or mutate originals_ from the gamma worker.
    const auto persistent=monitorProfileKey(mon);
    if (originals_.find(persistent) == originals_.end()) {
        HDC dc = CreateDCW(L"DISPLAY", mon.device.c_str(), nullptr, nullptr);
        if (!dc) { mon.gammaStatus = 0; return false; }
        adoptOriginal(persistent, dc);
        DeleteDC(dc);
    }
    const auto original = originals_.find(persistent);
    const Ramp* baseline = original != originals_.end() ? &original->second : nullptr;
    const Ramp ramp = buildAppliedRamp(mon.kelvin, mon.enabled && !mon.hardwareColor && !mon.nvidiaColor, baseline);
    mon.gammaStatus = -1;  // Worker publishes real hardware result later.
    queueGamma(mon,ramp,refresh);
    return true;
}
void Model::applyAll(GammaRefresh refresh) {
    for (auto& monitor : monitors_) applyDisplay(monitor,refresh);
}
bool Model::persistProfile(size_t index) {
    if (index >= monitors_.size() || !loaded_) return false;
    const auto& mon = monitors_[index];
    auto& profile = root_["screen"]["monitor_profiles"][toUtf8(monitorProfileKey(mon))];
    profile["enabled"] = mon.enabled;
    profile["kelvin"] = mon.kelvin;
    profile["custom_name"] = toUtf8(mon.alias);
    profile["hardware_color"] = mon.hardwareColor;
    profile["nvidia_color"] = mon.nvidiaColor;
    return save();
}
bool Model::previewKelvin(size_t index, int kelvin) {
    if (index >= monitors_.size() || !loaded_) return false;
    monitors_[index].kelvin = std::clamp(kelvin, 1000, 6500);
    root_["screen"]["kelvin"] = monitors_[index].kelvin;
    // Multi-monitor sessions keep a fixed independent backend. An untouched
    // display does not need DC creation or another queued job on every drag.
    applyDisplay(monitors_[index],GammaRefresh::Normal);
    return true;
}
bool Model::updateKelvin(size_t index, int kelvin) {
    if (!previewKelvin(index, kelvin)) return false;
    return persistProfile(index);
}
bool Model::updateEnabled(size_t index, bool enabled) {
    if (index >= monitors_.size() || !loaded_) return false;
    monitors_[index].enabled = enabled;
    root_["screen"]["kelvin"] = monitors_[index].kelvin;
    applyDisplay(monitors_[index],GammaRefresh::Normal);
    return persistProfile(index);
}
bool Model::updateColorBackend(size_t index,ColorBackend backend) {
    if (index>=monitors_.size() || !loaded_) return false;
    auto& mon=monitors_[index];
    const bool hardware=backend==ColorBackend::MonitorHardware;
    const bool nvidia=backend==ColorBackend::NvidiaCsc;
    if (mon.hardwareColor==hardware && mon.nvidiaColor==nvidia) return true;
    mon.hardwareColor=hardware;
    mon.nvidiaColor=nvidia;
    persistProfile(index);
    return applyDisplay(mon,GammaRefresh::Restore);
}
bool Model::renameMonitor(size_t index, const std::wstring& alias) {
    if (index >= monitors_.size() || !loaded_ || alias.size() > 64) return false;
    for (wchar_t c : alias) if (c < 0x20) return false;
    monitors_[index].alias = alias;
    return persistProfile(index);
}
void Model::updatePreferences(const Preferences& updated) {
    preferences_.locale = updated.locale == L"pt-PT" ? L"pt-PT" : L"en-US";
    preferences_.theme = updated.theme==L"light"?L"light":
        updated.theme==L"dark"?L"dark":L"system";
    preferences_.closeByClickingOutside = updated.closeByClickingOutside;
    save();
}
void Model::restoreOriginals() {
    // No queued/active gamma request may overwrite a restored calibration.
    stopGamma();
    // The GDI source name can point to a different physical display after
    // wake. Restore original gamma only to the currently matched physical
    // target, never to whichever panel received an old DISPLAYx number.
    for (const auto& mon:monitors_) {
        applyNvidiaColor(mon,false);
        applyHardwareColor(mon,false);
        if (!gammaTouched_.count(monitorProfileKey(mon))) continue;
        const auto original=originals_.find(monitorProfileKey(mon));
        if (original==originals_.end()) continue;
        HDC dc=CreateDCW(L"DISPLAY",mon.device.c_str(),nullptr,nullptr);
        if (dc) {
            SetDeviceGammaRamp(dc,const_cast<Ramp*>(&original->second));
            DeleteDC(dc);
        }
    }
    originals_.clear();
    gammaTouched_.clear();
}
void Model::migrateAutorunEntry() const {
    // Older NightLight builds created a Run key with a hardcoded
    // --minimized/--tray arguments. Strip legacy mode flags from an existing
    // autorun entry; fresh launches now always start in the tray.
    HKEY key = nullptr;
    constexpr wchar_t subkey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, L"NightLight", nullptr, &type, nullptr,
                        &bytes) == ERROR_SUCCESS && type == REG_SZ &&
        bytes > 0 && bytes < 32768) {
        std::wstring old(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, L"NightLight", nullptr, &type,
            reinterpret_cast<LPBYTE>(old.data()), &bytes) == ERROR_SUCCESS) {
            const bool legacy = old.find(L"--minimized") != std::wstring::npos ||
                                old.find(L"--tray") != std::wstring::npos ||
                                old.find(L"--normal") != std::wstring::npos;
            if (legacy && old.find(L"NightLight") != std::wstring::npos) {
                const std::wstring exe = (directory_ / L"NightLight.exe").wstring();
                if (fs::exists(exe)) {
                    const std::wstring command = L"\"" + exe + L"\"";
                    RegSetValueExW(key, L"NightLight", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(command.c_str()),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
                }
            }
        }
    }
    RegCloseKey(key);
}
} // namespace nightlight
