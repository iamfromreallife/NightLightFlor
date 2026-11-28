#include "core.hpp"
#include "localization.hpp"
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
static bool readWindowsDarkTheme(const wchar_t* valueName) {
    // The Windows *app* theme is independent of the taskbar/system theme.
    // A missing or unreadable value defaults to Windows' usual light mode.
    constexpr wchar_t key[]=
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD useLight=1, type=0, bytes=sizeof(useLight);
    HKEY handle=nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,key,0,KEY_QUERY_VALUE,&handle)!=ERROR_SUCCESS)
        return false;
    const LSTATUS result=RegQueryValueExW(handle,valueName,
        nullptr,&type,reinterpret_cast<LPBYTE>(&useLight),&bytes);
    RegCloseKey(handle);
    return result==ERROR_SUCCESS && type==REG_DWORD &&
        bytes==sizeof(useLight) && useLight==0;
}
bool systemAppsUseDarkTheme() { return readWindowsDarkTheme(L"AppsUseLightTheme"); }
bool systemTaskbarUsesDarkTheme() { return readWindowsDarkTheme(L"SystemUsesLightTheme"); }
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
    static const double d65 = (99.4708025861 * std::log(65.0) - 161.1195681661) / 255.0;
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
NvidiaTargetRamp buildNvidiaTargetRamp(int kelvin) {
    // Driver target LUT is sampled in encoded RGB.
    const auto g=kelvinGains(kelvin);
    NvidiaTargetRamp ramp{};
    for (size_t i=0;i<1024;++i) {
        const double x=double(i)/1023.0;
        ramp[3*i]=static_cast<float>(x*g.red);
        ramp[3*i+1]=static_cast<float>(x*g.green);
        ramp[3*i+2]=static_cast<float>(x*g.blue);
    }
    return ramp;
}
std::wstring monitorTooltip(const std::vector<Monitor>& monitors) {
    std::wstring text = L"NightLight";
    for (const auto& m : monitors) {
        text += L"\n" + m.displayName() + L": " + std::to_wstring(m.kelvin) + L" K";
        if (m.hardwareColor) text += L" [DDC/CI]";
        if (m.nvidiaTargetColor) text += L" [NVIDIA target gamma]";
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
bool legacySoftwareCursorOverride(const std::wstring& value) {
    // -1 was installed by the previous NightLight workaround, which asked
    // Windows to draw the mouse cursor in software. Normal mouse-trail
    // options (including accessibility preferences) are not changed.
    const auto first=value.find_first_not_of(L" \t\r\n");
    if (first==std::wstring::npos) return false;
    const auto last=value.find_last_not_of(L" \t\r\n");
    return value.substr(first,last-first+1)==L"-1";
}
bool clearLegacySoftwareCursorOverride() {
    wchar_t trails[64]{};
    DWORD type=0,bytes=sizeof(trails);
    // HKCU\\Control Panel\\Mouse is the user-specific mouse scheme.
    // Read first: do not silently override the user's normal trail setting.
    if (RegGetValueW(HKEY_CURRENT_USER,L"Control Panel\\Mouse",L"MouseTrails",
                     RRF_RT_REG_SZ,&type,trails,&bytes)!=ERROR_SUCCESS)
        return false;
    trails[std::size(trails)-1]=L'\0';
    if (!legacySoftwareCursorOverride(trails)) return false;
    // Disabling pointer trails removes the forced software cursor request.
    // This ALLOWS hardware rendering, but Windows/GPU decide which plane to
    // use; there is no documented public API to require a hardware cursor.
    return SystemParametersInfoW(SPI_SETMOUSETRAILS,0,nullptr,
               SPIF_UPDATEINIFILE|SPIF_SENDCHANGE)!=FALSE;
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
    // The neutral LUT is immutable and shared across all neutral requests.
    // Preserve the original calibration branch above and exact ramp math.
    if(!enabled || kelvin==6500) {
        static const Ramp neutral=buildKelvinRamp(6500);
        return neutral;
    }
    return buildKelvinRamp(kelvin);
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
    // Header actions do not consume space or cause an extra monitor row.
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
    inactiveWindowGeometry_.reset();settingsWindowPosition_.reset();
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
    preferences_.locale = languageAt(languageIndex(fromUtf8(lang))).code;
    const std::string appearance=general.get("appearance").str("system");
    preferences_.theme=appearance=="light"?L"light":
        appearance=="dark"?L"dark":L"system";
    preferences_.closeByClickingOutside = general.get("close_by_clicking_outside").boolean(true);
    preferences_.ddcControls=general.get("ddc_controls").boolean(true);
    preferences_.linkDisplays=general.get("link_displays").boolean(false);
    preferences_.trayWheelEnabled=general.get("tray_wheel_enabled").boolean(false);
    preferences_.trayWheelMode=std::clamp(general.get("tray_wheel_mode").integer(0),0,2);
    const auto& schedule=general.get("color_schedule");
    ColorSchedule loadedSchedule;
    loadedSchedule.enabled=schedule.get("enabled").boolean(false);
    loadedSchedule.transitionMinutes=schedule.get("transition_minutes").integer(30);
    loadedSchedule.locationName=fromUtf8(schedule.get("location_name").str());
    loadedSchedule.latitude=schedule.get("latitude").integer(0)/1000.0;
    loadedSchedule.longitude=schedule.get("longitude").integer(0)/1000.0;
    if(const auto* values=std::get_if<portable::Json::Array>(&schedule.get("points").value)) {
        loadedSchedule.points.clear();
        for(const auto& point:*values) {
            if(loadedSchedule.points.size()>=kMaxSchedulePoints)break;
            if(point.isObject())loadedSchedule.points.push_back({point.get("minute").integer(420),point.get("kelvin").integer(6500),static_cast<SchedulePointKind>(point.get("kind").integer(0)),point.get("custom_time").boolean(false)});
        }
        loadedSchedule.mode=static_cast<ScheduleMode>(schedule.get("mode").integer(0));
    } else if(schedule.isObject()) {
        // Keep the original day/night targets and timing on migration.
        // Adding a noon target would silently change an existing enabled schedule.
        loadedSchedule.points={{schedule.get("day_start").integer(420),schedule.get("day_kelvin").integer(6500),SchedulePointKind::Sunrise,true},
            {schedule.get("night_start").integer(1140),schedule.get("night_kelvin").integer(3500),SchedulePointKind::Sunset,true}};
        loadedSchedule.mode=ScheduleMode::Custom;
    }
    preferences_.colorSchedule=normalizedSchedule(std::move(loadedSchedule));
    preferences_.hotkeys.clear();
    if(const auto* groups=std::get_if<portable::Json::Object>(&general.get("hotkeys").value)) {
        for(const auto& item:*groups) {
            if(preferences_.hotkeys.size()>=256 || item.first.size()>2048)break;
            if(const auto* values=std::get_if<portable::Json::Array>(&item.second.value)) {
                if(values->size()!=4)continue;
                std::array<int,4> bindings{};
                for(size_t i=0;i<4;++i) {
                    const int value=(*values)[i].integer();
                    if(validHotkey(value))bindings[i]=value;
                }
                if(std::any_of(bindings.begin(),bindings.end(),[](int v){return v!=0;}))
                    preferences_.hotkeys[fromUtf8(item.first)]=bindings;
            }
        }
    }
    preferences_.hotkeySteps.clear();
    if(const auto* groups=std::get_if<portable::Json::Object>(&general.get("hotkey_steps").value))for(const auto& item:*groups){
        if(preferences_.hotkeySteps.size()>=256 || item.first.size()>2048)break;
        if(const auto* values=std::get_if<portable::Json::Array>(&item.second.value);values && values->size()==4){
            auto steps=kDefaultHotkeySteps;
            for(int i=0;i<4;++i){const int v=(*values)[i].integer();if(validHotkeyStep(i,v))steps[i]=v;}
            preferences_.hotkeySteps[fromUtf8(item.first)]=steps;
        }
    }
    preferences_.dimInactive=general.get("dim_inactive").boolean(false);
    preferences_.dimAmount=std::clamp(general.get("dim_amount").integer(90),1,99);
    preferences_.dimSeconds=normalizeDimSeconds(general.get("dim_seconds").integer(legacyDimSeconds(general.get("dim_minutes").integer(5))));
    preferences_.dimAll=general.get("dim_all").boolean(true);
    preferences_.dimDisplays.clear();
    if(const auto* values=std::get_if<portable::Json::Array>(&general.get("dim_displays").value))
        for(const auto& item:*values) {
            const auto key=fromUtf8(item.str());
            if(!key.empty()) preferences_.dimDisplays.insert(key);
        }
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
    const auto& settingsPosition=general.get("settings_window_position");
    if(settingsPosition.isObject()) {
        const int x=settingsPosition.get("x").integer(INT_MIN),y=settingsPosition.get("y").integer(INT_MIN);
        if(x>=-1000000 && x<=1000000 && y>=-1000000 && y<=1000000)
            settingsWindowPosition_=POINT{x,y};
    }
    const auto& inactivePosition=general.get("inactive_window_position");
    if(inactivePosition.isObject()) {
        const int x=inactivePosition.get("x").integer(INT_MIN);
        const int y=inactivePosition.get("y").integer(INT_MIN);
        const int width=inactivePosition.get("width").integer(-1);
        const int height=inactivePosition.get("height").integer(-1);
        if(x>=-1000000 && x<=1000000 && y>=-1000000 && y<=1000000 &&
            width>=200 && width<=10000 && height>=260 && height<=10000)
            inactiveWindowGeometry_=RECT{x,y,x+width,y+height};
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
bool Model::rememberSettingsWindowPosition(int x,int y) {
    if(!loaded_ || x < -1000000 || x > 1000000 || y < -1000000 || y > 1000000) return false;
    if(settingsWindowPosition_ && settingsWindowPosition_->x==x && settingsWindowPosition_->y==y) return false;
    settingsWindowPosition_=POINT{x,y};return true;
}
bool Model::rememberInactiveWindowGeometry(const RECT& rect) {
    const auto width=static_cast<std::int64_t>(rect.right)-rect.left;
    const auto height=static_cast<std::int64_t>(rect.bottom)-rect.top;
    if(!loaded_ || rect.left < -1000000 || rect.left > 1000000 ||
       rect.top < -1000000 || rect.top > 1000000 ||
       width<200 || width>10000 || height<260 || height>10000) return false;
    if(inactiveWindowGeometry_ && EqualRect(&*inactiveWindowGeometry_,&rect)) return false;
    inactiveWindowGeometry_=rect;
    return true;
}
bool Model::save() {
    if (!loaded_) return false;
    root_["general"]["language"] = toUtf8(preferences_.locale);
    root_["general"]["appearance"] = toUtf8(preferences_.theme);
    root_["general"]["close_by_clicking_outside"] = preferences_.closeByClickingOutside;
    root_["general"]["ddc_controls"]=preferences_.ddcControls;
    root_["general"]["link_displays"]=preferences_.linkDisplays;
    root_["general"]["tray_wheel_enabled"]=preferences_.trayWheelEnabled;
    root_["general"]["tray_wheel_mode"]=preferences_.trayWheelMode;
    const auto& schedule=preferences_.colorSchedule;
    auto& scheduleJson=root_["general"]["color_schedule"];
    scheduleJson=portable::Json::object();
    scheduleJson["enabled"]=schedule.enabled;
    scheduleJson["mode"]=static_cast<int>(schedule.mode);
    scheduleJson["transition_minutes"]=schedule.transitionMinutes;
    scheduleJson["location_name"]=toUtf8(schedule.locationName);
    scheduleJson["latitude"]=static_cast<int>(std::lround(schedule.latitude*1000.0));
    scheduleJson["longitude"]=static_cast<int>(std::lround(schedule.longitude*1000.0));
    portable::Json::Array schedulePoints;
    schedulePoints.reserve(schedule.points.size());
    for(const auto& point:schedule.points){
        auto item=portable::Json::object();item["minute"]=point.minute;item["kelvin"]=point.kelvin;item["kind"]=static_cast<int>(point.kind);item["custom_time"]=point.customTime;
        schedulePoints.push_back(std::move(item));
    }
    scheduleJson["points"]=portable::Json(std::move(schedulePoints));
    portable::Json hotkeys=portable::Json::object();
    for(const auto& group:preferences_.hotkeys) {
        portable::Json::Array values;values.reserve(group.second.size());
        for(int value:group.second)values.emplace_back(value);
        hotkeys[toUtf8(group.first)]=portable::Json(std::move(values));
    }
    root_["general"]["hotkeys"]=std::move(hotkeys);
    portable::Json hotkeySteps=portable::Json::object();
    for(const auto& group:preferences_.hotkeySteps){portable::Json::Array values;values.reserve(group.second.size());for(int v:group.second)values.emplace_back(v);hotkeySteps[toUtf8(group.first)]=portable::Json(std::move(values));}
    root_["general"]["hotkey_steps"]=std::move(hotkeySteps);
    root_["general"]["dim_inactive"]=preferences_.dimInactive;
    root_["general"]["dim_amount"]=preferences_.dimAmount;
    root_["general"]["dim_seconds"]=preferences_.dimSeconds;
    // Retain a rounded-up minute value for older builds.
    root_["general"]["dim_minutes"]=legacyDimMinutes(preferences_.dimSeconds);
    root_["general"]["dim_all"]=preferences_.dimAll;
    portable::Json::Array dimDisplays;dimDisplays.reserve(preferences_.dimDisplays.size());
    for(const auto& key:preferences_.dimDisplays) dimDisplays.emplace_back(toUtf8(key));
    root_["general"]["dim_displays"]=portable::Json(std::move(dimDisplays));
    for (const auto& mon : monitors_) {
        auto& profile = root_["screen"]["monitor_profiles"][toUtf8(monitorProfileKey(mon))];
        profile["kelvin"] = mon.kelvin;
        profile["enabled"] = mon.enabled;
        profile["custom_name"] = toUtf8(mon.alias);
        if (mon.hardwareColor) profile["hardware_color"] = true;
        else profile["hardware_color"] = false;
        profile["nvidia_target_color"] = mon.nvidiaTargetColor;
    }
    // Persist only settings implemented by this version. Keep profiles for
    // disconnected displays so their current targets survive reconnects.
    auto clean=portable::Json::object();
    clean["screen"]["kelvin"]=std::clamp(root_.get("screen").get("kelvin").integer(5000),1000,6500);
    clean["general"]=portable::Json::object();
    for(const char* key:{"language","appearance","close_by_clicking_outside","dim_inactive","dim_amount","dim_minutes","dim_seconds","dim_all","dim_displays","ddc_controls","link_displays","tray_wheel_enabled","tray_wheel_mode","color_schedule","hotkeys","hotkey_steps"})
        clean["general"][key]=root_.get("general").get(key);
    if (windowPosition_) {
        clean["general"]["window_position"]["x"]=windowPosition_->x;
        clean["general"]["window_position"]["y"]=windowPosition_->y;
    }
    if (windowSize_) {
        clean["general"]["window_position"]["width"]=windowSize_->cx;
        clean["general"]["window_position"]["height"]=windowSize_->cy;
    }
    if(settingsWindowPosition_) {
        auto& pos=clean["general"]["settings_window_position"];
        pos["x"]=static_cast<int>(settingsWindowPosition_->x);
        pos["y"]=static_cast<int>(settingsWindowPosition_->y);
    }
    if(inactiveWindowGeometry_) {
        const auto& r=*inactiveWindowGeometry_;
        auto& pos=clean["general"]["inactive_window_position"];
        pos["x"]=static_cast<int>(r.left);pos["y"]=static_cast<int>(r.top);
        pos["width"]=static_cast<int>(r.right-r.left);
        pos["height"]=static_cast<int>(r.bottom-r.top);
    }
    auto profiles=portable::Json::object();
    if(auto values=std::get_if<portable::Json::Object>(&root_.get("screen").get("monitor_profiles").value))
        for(const auto& item:*values) {
            auto profile=portable::Json::object();
            for(const char* key:{"kelvin","enabled","custom_name","ddc_brightness","ddc_contrast","hardware_color","nvidia_target_color"}) {
                const auto& value=item.second.get(key);
                if(!std::holds_alternative<std::nullptr_t>(value.value))profile[key]=value;
            }
            if(!std::get<portable::Json::Object>(profile.value).empty())profiles[item.first]=std::move(profile);
        }
    clean["screen"]["monitor_profiles"]=std::move(profiles);
    if(root_.get("screen").get("monitor_order").isArray())
        clean["screen"]["monitor_order"]=root_.get("screen").get("monitor_order");
    root_=std::move(clean);
    std::string document=root_.dump();
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
    savedDocument_=std::move(document);
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
    m.nvidiaTargetColor=profile.get("nvidia_target_color").boolean(false);
    m.hardwareColor = !m.nvidiaTargetColor && profile.get("hardware_color").boolean(false);
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
void Model::pauseColorWrites() {
    if(colorWritesPaused_) return;
    colorWritesPaused_=true;
    std::lock_guard<std::mutex> guard(gammaMutex_);
    pendingGamma_.clear();
    gammaResults_.clear();
    for(auto& item:newestGeneration_) ++item.second;
    for(auto& monitor:monitors_) monitor.gammaStatus=0;
}
void Model::beginDisplayRecovery() {
    wakeCandidate_.clear();
    wakeConsistentSamples_=0;
    pauseColorWrites();
}
void Model::reassertWakeColors() {
    if(colorWritesPaused_) return;
    collectGammaResults();
    // Always force another write, even after a driver accepted the earlier
    // request. Do not supersede work still in flight on a slower display.
    for(auto& monitor:monitors_)
        if(monitor.gammaStatus!=-1) applyDisplay(monitor,GammaRefresh::Restore);
}
void Model::advanceStartupColorTransition(bool finish) {
    if(startupColorKeys_.empty()) return;
    collectGammaResults();
    const auto now=GetTickCount64();
    const auto elapsed=now>=startupColorStarted_?now-startupColorStarted_:0;
    const auto previous=startupColorElapsed_;
    startupColorElapsed_=finish?kStartupColorMs:std::min(elapsed,kStartupColorMs);
    if(startupColorElapsed_>=kStartupColorMs) {
        // Queue the exact final value even if an earlier sample remains
        // pending. Generation checks coalesce queued work and reject stale
        // completions. A paused topology is restored by normal wake recovery.
        const auto keys=std::move(startupColorKeys_);
        startupColorKeys_.clear();
        if(!colorWritesPaused_) for(auto& monitor:monitors_)
            if(keys.count(monitorProfileKey(monitor))) applyDisplay(monitor,GammaRefresh::Restore);
        return;
    }
    if(colorWritesPaused_) return;
    for(auto& monitor:monitors_) {
        if(!startupColorKeys_.count(monitorProfileKey(monitor)) || monitor.gammaStatus==-1) continue;
        if(monitor.gammaStatus==0 ||
           startupColorKelvin(monitor.kelvin,previous)!=startupColorKelvin(monitor.kelvin,startupColorElapsed_))
            applyDisplay(monitor,GammaRefresh::Restore);
    }
}
bool Model::wakeColorsPending() const {
    return std::any_of(monitors_.begin(),monitors_.end(),
        [](const Monitor& monitor){return monitor.gammaStatus==-1;});
}
bool Model::refreshMonitors(bool afterWake,bool* stable) {
    if (stable) *stable=false;
    const auto physical=queryPhysicalDisplayTargets();
    EnumerateContext context;
    context.targets=&physical;
    if (!EnumDisplayMonitors(nullptr,nullptr,enumerateMonitors,
                             reinterpret_cast<LPARAM>(&context)) ||
        context.entries.empty()) {
        if(afterWake) beginDisplayRecovery();
        return false;
    }
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
            beginDisplayRecovery();
            return false;
        }
        if (sameDisplaySnapshot(wakeCandidate_,entries))
            ++wakeConsistentSamples_;
        else {
            wakeCandidate_=entries;
            wakeConsistentSamples_=1;
        }
        // Two identical samples, on distinct timer ticks, are required.
        if (wakeConsistentSamples_<2) { pauseColorWrites(); return false; }
        colorWritesPaused_=false;
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
            mon.nvidiaTargetColor=old->nvidiaTargetColor;
        }
    }
    {
        std::lock_guard<std::mutex> guard(gammaMutex_);
        pendingGamma_.clear();
        gammaResults_.clear();
        for(auto& entry:newestGeneration_) ++entry.second;
    }
    monitors_=std::move(entries);
    if(startupColorArmed_) {
        startupColorArmed_=false;
        startupColorStarted_=GetTickCount64();
        startupColorElapsed_=0;
        startupColorKeys_.clear();
        for(const auto& mon:monitors_) {
            if(mon.enabled && mon.kelvin<6500 && !mon.hardwareColor)
                startupColorKeys_.insert(monitorProfileKey(mon));
        }
    }
    applyAll(GammaRefresh::Restore);
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
    return matchPhysicalTarget(topology,mon.device,mon.physicalId)!=nullptr;
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
    const auto physicalKey=monitorProfileKey(mon);
    if(!active && cleanHardware_.count(physicalKey)) return true;
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
    if (!saved && !active) { cleanHardware_.insert(physicalKey); return true; }
    cleanHardware_.erase(physicalKey);
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
        cleanHardware_.insert(physicalKey);
    }
    return true;
}

namespace {
// Target gamma wire ABI documented by independent reverse engineering:
// https://github.com/pbatard/nvBrightness (src/nvapi.h) and
// https://github.com/mgcarnevali/NvProfileSwitcher (Nvidia/NvApi.cs).
// Only the ABI facts are shared; NightLight generates its own Kelvin LUT.
struct NvidiaTargetV1 {
    std::uint32_t version=0x13008;
    NvidiaTargetRamp rgb{};
    std::uint32_t flags=1;
};
static_assert(sizeof(NvidiaTargetV1)==0x3008,"Target gamma V1 ABI");
struct NvidiaTargetApi {
    using Query=void* (__cdecl*)(std::uint32_t);
    using Init=int (__cdecl*)();
    using DisplayId=int (__cdecl*)(const char*,std::uint32_t*);
    using TargetGamma=int (__cdecl*)(std::uint32_t,NvidiaTargetV1*);
    HMODULE module=nullptr;
    Init unload=nullptr;
    DisplayId displayId=nullptr;
    TargetGamma setTarget=nullptr;
    bool initialized=false;
    NvidiaTargetApi() {
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
        setTarget=reinterpret_cast<TargetGamma>(query(0x7082a053));
    }
    ~NvidiaTargetApi() {
        if (initialized && unload) unload();
        if (module) FreeLibrary(module);
    }
    bool available() const { return initialized && displayId && setTarget; }
};
void logNvidiaStatus(const wchar_t* operation,int status) {
    const auto message=std::wstring(L"NightLight NVAPI ")+operation+L": "+std::to_wstring(status)+L"\n";
    OutputDebugStringW(message.c_str());
}
} // namespace
// Conservatively require a uniquely identified SDR output. Use the same
// fresh display topology to check physical identity AND Advanced Color.
// Retain both the preflight and final checks around journal/driver work.
static bool targetGammaSdr(const Monitor& mon) {
    if(mon.physicalId.empty()) return false;
    const auto topology=queryPhysicalDisplayTargets();
    const auto* target=matchPhysicalTarget(topology,mon.device,mon.physicalId);
    if(!target) return false;
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
    info.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size=sizeof(info);
    info.header.adapterId=target->adapterId;
    info.header.id=target->targetId;
    return DisplayConfigGetDeviceInfo(&info.header)==ERROR_SUCCESS &&
        !info.advancedColorEnabled;
}
bool Model::applyNvidiaTargetColor(const Monitor& mon,bool active,GammaRefresh refresh) {
    // No reliable target-gamma getter has been established. Only opt in
    // from user-confirmed default NVIDIA colour settings; restore identity
    // rather than claiming to capture an arbitrary existing calibration.
    const auto key=monitorProfileKey(mon);
    const int kelvin=active?mon.kelvin:6500;
    const auto last=targetGammaAccepted_.find(key);
    if(!active && cleanTarget_.count(key)) return true;
    if(active && refresh!=GammaRefresh::Restore &&
        last!=targetGammaAccepted_.end() && last->second==kelvin) return true;
    const auto path=directory_/L"nvidia-target-backup.json";
    portable::Json journal=portable::Json::object();
    std::string document;
    if(readWholeFile(path,document)) {
        try { journal=portable::Json::parse(document); } catch(...) { return false; }
        if(!journal.isObject()) return false;
    } else {
        const DWORD attrs=GetFileAttributesW(path.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) return false;
    }
    const auto jsonKey=toUtf8(key);
    const bool owned=journal.get(jsonKey).isObject();
    if(!owned && !active) { cleanTarget_.insert(key); return true; }
    cleanTarget_.erase(key);
    if(owned && !journal.get(jsonKey).get("restore_neutral").boolean(false)) return false;
    if(!targetGammaSdr(mon)) return false;
    static NvidiaTargetApi api;
    if(!api.available()) {
        logNvidiaStatus(L"target gamma unavailable",-104); return false;
    }
    std::uint32_t id=0;
    const auto name=toUtf8(mon.device);
    const int idStatus=api.displayId(name.c_str(),&id);
    if(idStatus!=0 || !id) { logNvidiaStatus(L"target gamma display ID",idStatus); return false; }
    if(!owned) {
        journal[jsonKey]["restore_neutral"]=true;
        if(!atomicWrite(path,journal.dump())) return false;
    }
    NvidiaTargetV1 target;
    target.rgb=buildNvidiaTargetRamp(kelvin);
    if(!targetGammaSdr(mon)) return false;
    const int status=api.setTarget(id,&target);
    logNvidiaStatus(L"SetTargetGammaCorrection",status);
    if(status!=0) { targetGammaAccepted_.erase(key); return false; }
    if(active) targetGammaAccepted_[key]=kelvin;
    else {
        targetGammaAccepted_.erase(key);
        std::get<portable::Json::Object>(journal.value).erase(jsonKey);
        if(!atomicWrite(path,journal.dump())) return false;
        cleanTarget_.insert(key);
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
                gammaWake_.wait(guard,[this]{return gammaStopping_ || !pendingGamma_.empty();});
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
        if (job.monitor.hardwareColor || job.monitor.nvidiaTargetColor) {
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
                if (job.monitor.nvidiaTargetColor) {
                    if (applyHardwareColor(job.monitor,false))
                        result=applyNvidiaTargetColor(job.monitor,active,job.refresh)?1:0;
                } else if (applyNvidiaTargetColor(job.monitor,false,job.refresh)) {
                    result=applyHardwareColor(job.monitor,active)?1:0;
                }
            }
        } else if ((job.refresh!=GammaRefresh::Restore || job.monitor.physicalId.empty() ||
                    uniquePhysicalTarget(job.monitor)) &&
                   applyNvidiaTargetColor(job.monitor,false,job.refresh) &&
                   applyHardwareColor(job.monitor,false)) {
            HDC dc=CreateDCW(L"DISPLAY",device.c_str(),nullptr,nullptr);
            if (dc) {
                result=updateGammaRamp(job.ramp,job.refresh,appliedRamps[device],
                    [&](Ramp& actual) { return GetDeviceGammaRamp(dc,&actual)!=FALSE; },
                    [&](const Ramp& desired) {
                        if(job.refresh==GammaRefresh::Restore && !job.monitor.physicalId.empty() &&
                           !uniquePhysicalTarget(job.monitor)) return false;
                        gammaTouched_.insert(key);
                        return SetDeviceGammaRamp(dc,const_cast<Ramp*>(&desired))!=FALSE;
                    })?1:0;
                DeleteDC(dc);
            }
        }
        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> guard(gammaMutex_);
            if (!gammaStopping_ && newestGeneration_[device] == job.generation &&
                result!=job.monitor.gammaStatus) {
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
    if(colorWritesPaused_) { mon.gammaStatus=0; return true; }
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
    // Only the worker snapshot receives the transitional Kelvin. The saved
    // profile, controls and configuration remain at the requested target.
    const int kelvin=startupColorKeys_.count(persistent)?
        startupColorKelvin(mon.kelvin,startupColorElapsed_):mon.kelvin;
    const Ramp ramp = buildAppliedRamp(kelvin, mon.enabled && !mon.hardwareColor && !mon.nvidiaTargetColor, baseline);
    // Retain the last verified status during an unchanged periodic check.
    // A new Kelvin/backend/enable request or wake recovery invalidates it.
    if (refresh!=GammaRefresh::Verify) mon.gammaStatus=-1;
    Monitor request=mon;
    request.kelvin=kelvin;
    queueGamma(request,ramp,refresh);
    return true;
}
void Model::applyAll(GammaRefresh refresh) {
    for (auto& monitor : monitors_) {
        if(refresh==GammaRefresh::Verify && !monitorNeedsColorWatchdog(monitor)) continue;
        applyDisplay(monitor,refresh);
    }
}
bool Model::needsColorWatchdog() const {
    return std::any_of(monitors_.begin(),monitors_.end(),monitorNeedsColorWatchdog);
}
void Model::retryFailedColors() {
    for(auto& monitor:monitors_)
        if(monitor.gammaStatus==0) applyDisplay(monitor,GammaRefresh::Restore);
}
bool Model::persistProfile(size_t index) {
    if (index >= monitors_.size() || !loaded_) return false;
    const auto& mon = monitors_[index];
    auto& profile = root_["screen"]["monitor_profiles"][toUtf8(monitorProfileKey(mon))];
    profile["enabled"] = mon.enabled;
    profile["kelvin"] = mon.kelvin;
    profile["custom_name"] = toUtf8(mon.alias);
    profile["hardware_color"] = mon.hardwareColor;
    profile["nvidia_target_color"] = mon.nvidiaTargetColor;
    return save();
}
void Model::previewScheduleColor(int kelvin) {
    // Use the existing backend request pipeline without changing a monitor
    // profile, root JSON, or the gamma ramp generation/validation algorithm.
    if(!loaded_ || colorWritesPaused_ || startupColorActive())return;
    for(const auto& monitor:monitors_)if(monitor.enabled){
        Monitor temporary=monitor;temporary.kelvin=std::clamp(kelvin,1000,6500);
        applyDisplay(temporary,GammaRefresh::Normal);
    }
}
bool Model::previewKelvin(size_t index, int kelvin) {
    if (index >= monitors_.size() || !loaded_) return false;
    auto& monitor=monitors_[index];
    const int target=std::clamp(kelvin,1000,6500);
    // Drag release persists the last preview without duplicating the driver call.
    const bool transitioning=startupColorKeys_.erase(monitorProfileKey(monitor))!=0;
    if(monitor.kelvin==target && monitor.gammaStatus!=0 && !transitioning) return true;
    monitor.kelvin=target;
    root_["screen"]["kelvin"] = monitor.kelvin;
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
    startupColorKeys_.erase(monitorProfileKey(monitors_[index]));
    monitors_[index].enabled = enabled;
    root_["screen"]["kelvin"] = monitors_[index].kelvin;
    applyDisplay(monitors_[index],GammaRefresh::Normal);
    return persistProfile(index);
}
bool Model::updateColorBackend(size_t index,ColorBackend backend) {
    if (index>=monitors_.size() || !loaded_) return false;
    auto& mon=monitors_[index];
    const bool hardware=backend==ColorBackend::MonitorHardware;
    const bool target=backend==ColorBackend::NvidiaTargetGamma;
    if (mon.hardwareColor==hardware && mon.nvidiaTargetColor==target) return true;
    startupColorKeys_.erase(monitorProfileKey(mon));
    mon.hardwareColor=hardware;
    mon.nvidiaTargetColor=target;
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
    preferences_.locale = languageAt(languageIndex(updated.locale)).code;
    preferences_.theme = updated.theme==L"light"?L"light":
        updated.theme==L"dark"?L"dark":L"system";
    preferences_.closeByClickingOutside = updated.closeByClickingOutside;
    preferences_.ddcControls=updated.ddcControls;
    preferences_.linkDisplays=updated.linkDisplays;
    preferences_.trayWheelEnabled=updated.trayWheelEnabled;
    preferences_.trayWheelMode=std::clamp(updated.trayWheelMode,0,2);
    preferences_.colorSchedule=normalizedSchedule(updated.colorSchedule);
    preferences_.hotkeys=updated.hotkeys;
    preferences_.hotkeySteps=updated.hotkeySteps;
    preferences_.dimInactive=updated.dimInactive;
    preferences_.dimAmount=std::clamp(updated.dimAmount,1,99);
    preferences_.dimSeconds=normalizeDimSeconds(updated.dimSeconds);
    preferences_.dimAll=updated.dimAll;
    preferences_.dimDisplays=updated.dimDisplays;
    save();
}
void Model::restoreOriginals() {
    // No queued/active gamma request may overwrite a restored calibration.
    stopGamma();
    // The GDI source name can point to a different physical display after
    // wake. Restore original gamma only to the currently matched physical
    // target, never to whichever panel received an old DISPLAYx number.
    for (const auto& mon:monitors_) {
        applyNvidiaTargetColor(mon,false,GammaRefresh::Restore);
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
