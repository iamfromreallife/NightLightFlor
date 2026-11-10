#include "core.hpp"
#include <shlobj.h>
#include <magnification.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <climits>
#include <stdexcept>
#include <utility>

namespace nightlight {
namespace fs = std::filesystem;

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
fs::path executableDirectory() {
    std::vector<wchar_t> path(32768, 0);
    const DWORD written = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (!written || written >= path.size())
        throw std::runtime_error("Cannot discover executable folder");
    return fs::path(std::wstring(path.data(), written)).parent_path();
}
std::wstring resolveStartup(const std::wstring& pref,
    const std::wstring& argument, bool trayAvailable) {
    std::wstring mode = (pref == L"tray" || pref == L"minimized") ? pref : L"normal";
    if (argument == L"--tray") mode = L"tray";
    else if (argument == L"--minimized") mode = L"minimized";
    else if (argument == L"--normal") mode = L"normal";
    if (mode == L"tray" && !trayAvailable) return L"normal";
    return mode;
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
int sharedMatrixKelvin(const std::vector<Monitor>& monitors) {
    // Keep the backend stable throughout multi-monitor drags, including when
    // two temperatures briefly coincide. A global matrix cannot be independent.
    return monitors.size()==1 && monitors[0].enabled ? monitors[0].kelvin : 0;
}

std::wstring monitorTooltip(const std::vector<Monitor>& monitors) {
    std::wstring text = L"NightLight";
    for (const auto& m : monitors) {
        text += L"\n" + m.displayName() + L": " + std::to_wstring(m.kelvin) + L" K";
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
};
BOOL CALLBACK enumerateMonitors(HMONITOR hMon, HDC, LPRECT, LPARAM user) {
    auto* context = reinterpret_cast<EnumerateContext*>(user);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMon, &info)) return TRUE;
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

POINT ddcSliderIconCenter(int iconColumnCenterX,int sliderTop,int sliderHeight) {
    return POINT{iconColumnCenterX,sliderTop+sliderHeight/2};
}

int ddcSliderWidth(int dpi,int clientWidth) {
    const auto px=[dpi](int value) { return MulDiv(value,std::max(96,dpi),96); };
    // Preserve the DDC slider's in-card padding on both sides while
    // reclaiming the same four pixels at each outer edge.
    return std::max(px(16),clientWidth-
        px(2*kCardOuterMarginDip+2*kCardInnerInsetDip+84));
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
    preferences_.theme = general.get("appearance").str() == "light" ? L"light" : L"dark";
    const std::string startup = general.get("startup_mode").str("normal");
    preferences_.startup = (startup == "minimized" || startup == "tray")
        ? fromUtf8(startup) : L"normal";
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
bool Model::hasHardwarePercent(const std::wstring& device,bool contrast) const {
    return root_.get("screen").get("monitor_profiles").get(toUtf8(device))
        .get(contrast?"ddc_contrast":"ddc_brightness").integer(-1)>=0;
}
int Model::hardwarePercent(const std::wstring& device,bool contrast) const {
    return std::clamp(root_.get("screen").get("monitor_profiles").get(toUtf8(device))
        .get(contrast?"ddc_contrast":"ddc_brightness").integer(50),0,100);
}
bool Model::rememberHardwarePercent(const std::wstring& device,bool contrast,int percent) {
    if (!loaded_ || device.empty()) return false;
    percent=std::clamp(percent,0,100);
    auto& profile=root_["screen"]["monitor_profiles"][toUtf8(device)];
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
    root_["general"]["startup_mode"] = toUtf8(preferences_.startup);
    for (const auto& mon : monitors_) {
        auto& profile = root_["screen"]["monitor_profiles"][toUtf8(mon.device)];
        profile["kelvin"] = mon.kelvin;
        profile["enabled"] = mon.enabled;
        profile["custom_name"] = toUtf8(mon.alias);
    }
    // Persist only settings implemented by this version. Keep profiles for
    // disconnected displays so their current targets survive reconnects.
    auto clean=portable::Json::object();
    clean["screen"]["kelvin"]=std::clamp(root_.get("screen").get("kelvin").integer(5000),1000,6500);
    clean["general"]=portable::Json::object();
    for(const char* key:{"language","appearance","startup_mode"})
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
            for(const char* key:{"kelvin","enabled","custom_name","ddc_brightness","ddc_contrast"}) {
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
    const auto& profile = root_.get("screen").get("monitor_profiles").get(toUtf8(m.device));
    m.enabled = profile.get("enabled").boolean(true);
    m.kelvin = std::clamp(
        profile.get("kelvin").integer(profileDefaultKelvin(root_)), 1000, 6500);
    m.alias = fromUtf8(profile.get("custom_name").str());
}
bool Model::refreshMonitors() {
    EnumerateContext context;
    EnumDisplayMonitors(nullptr, nullptr, enumerateMonitors,
                        reinterpret_cast<LPARAM>(&context));
    auto& entries = context.entries;
    std::vector<std::wstring> order;
    if(auto saved=std::get_if<portable::Json::Array>(&root_.get("screen").get("monitor_order").value))
        for(const auto& value:*saved)order.push_back(fromUtf8(value.str()));
    std::sort(entries.begin(),entries.end(),[&](const Monitor& a,const Monitor& b) {
        const auto ai=std::find(order.begin(),order.end(),a.device),bi=std::find(order.begin(),order.end(),b.device);
        if(ai!=bi)return ai<bi;
        return a.device<b.device;
    });
    bool changed = entries.size() != monitors_.size();
    if (!changed) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].device != monitors_[i].device ||
                entries[i].windowsName != monitors_[i].windowsName ||
                entries[i].primary != monitors_[i].primary) {
                changed = true; break;
            }
        }
    }
    if (!changed) return false;
    for (auto& mon : entries) {
        const auto previous=std::find_if(monitors_.begin(),monitors_.end(),[&](const Monitor& old){return old.device==mon.device;});
        if (previous==monitors_.end()) readProfile(mon);
        else { mon.kelvin=previous->kelvin; mon.enabled=previous->enabled; mon.alias=previous->alias; }
    }
    monitors_ = std::move(entries);
    applyAll();
    return true;
}
bool Model::moveMonitor(size_t from,size_t to) {
    if(from>=monitors_.size() || to>=monitors_.size() || from==to)return false;
    Monitor moved=std::move(monitors_[from]);
    monitors_.erase(monitors_.begin()+from);
    monitors_.insert(monitors_.begin()+to,std::move(moved));
    auto order=portable::Json::array();
    auto& values=std::get<portable::Json::Array>(order.value);
    for(const auto& monitor:monitors_)values.emplace_back(toUtf8(monitor.device));
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

void Model::queueGamma(const std::wstring& device, const Ramp& ramp, int matrixKelvin, const Ramp& fallback,GammaRefresh refresh) {
    std::lock_guard<std::mutex> guard(gammaMutex_);
    if (gammaStopping_) return;
    const std::uint64_t generation = ++newestGeneration_[device];
    auto pending=pendingGamma_.find(device);
    if (pending!=pendingGamma_.end() && static_cast<int>(pending->second.refresh)>static_cast<int>(refresh))
        refresh=pending->second.refresh;
    pendingGamma_[device] = GammaJob{generation, ramp, matrixKelvin, fallback,refresh};
    if (gammaEvent_) SetEvent(gammaEvent_); else gammaWake_.notify_one();
}

void Model::gammaLoop() {
    // Resolve the system DLL explicitly; MinGW lacks these import declarations.
    HMODULE magLibrary = LoadLibraryExW(L"Magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Init = BOOL (WINAPI*)();
    using GetEffect = BOOL (WINAPI*)(PMAGCOLOREFFECT);
    const auto initialize = magLibrary ? reinterpret_cast<Init>(GetProcAddress(magLibrary,"MagInitialize")) : nullptr;
    const auto uninitialize = magLibrary ? reinterpret_cast<Init>(GetProcAddress(magLibrary,"MagUninitialize")) : nullptr;
    const auto getEffect = magLibrary ? reinterpret_cast<GetEffect>(GetProcAddress(magLibrary,"MagGetFullscreenColorEffect")) : nullptr;
    const auto setEffect = magLibrary ? reinterpret_cast<GetEffect>(GetProcAddress(magLibrary,"MagSetFullscreenColorEffect")) : nullptr;
    const bool magReady = initialize && uninitialize && getEffect && setEffect && initialize();
    MAGCOLOREFFECT original{};
    const bool captured = magReady && getEffect(&original);
    int activeMatrix = -1;
    bool matrixTouched=false;
    std::map<std::wstring,GammaRampState> appliedRamps;
    auto matrix = [&](int k,bool verify=false) {
        MAGCOLOREFFECT effect{};
        for (int i=0; i<5; ++i) effect.transform[i][i] = 1.0f;
        if (k) {
            const auto gains = kelvinGains(k);
            effect.transform[0][0] = static_cast<float>(gains.red);
            effect.transform[1][1] = static_cast<float>(gains.green);
            effect.transform[2][2] = static_cast<float>(gains.blue);
        }
        if (!magReady) return false;
        if (verify) {
            MAGCOLOREFFECT actual{};
            if (getEffect(&actual)) {
                bool equal=true;
                for (int i=0;i<5;++i) for (int j=0;j<5;++j)
                    if (std::abs(actual.transform[i][j]-effect.transform[i][j])>0.0001f) equal=false;
                if (equal) return true;
            }
        }
        const bool applied=setEffect(&effect)!=FALSE;
        matrixTouched=matrixTouched || applied;
        return applied;
    };
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
                // Magnification owns a message queue on this worker. Sleep
                // until work, shutdown or a Windows message actually arrives.
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
        bool matrixOK = activeMatrix == job.matrixKelvin;
        if (!matrixOK || job.refresh!=GammaRefresh::Normal) {
            matrixOK = matrix(job.matrixKelvin,job.refresh==GammaRefresh::Verify);
            if (matrixOK) activeMatrix = job.matrixKelvin;
        }
        if (job.matrixKelvin && !matrixOK) job.ramp = job.fallback;
        HDC dc = CreateDCW(L"DISPLAY", device.c_str(), nullptr, nullptr);
        int result = 0;
        if (dc) {
            result=updateGammaRamp(job.ramp,job.refresh,appliedRamps[device],
                [&](Ramp& actual) { return GetDeviceGammaRamp(dc,&actual)!=FALSE; },
                [&](const Ramp& desired) { return SetDeviceGammaRamp(dc,const_cast<Ramp*>(&desired))!=FALSE; })?1:0;
            DeleteDC(dc);
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
    if (magReady) {
        if (matrixTouched) {
            if (captured) setEffect(&original);
            else matrix(0);
        }
        uninitialize();
    }
    if (magLibrary) FreeLibrary(magLibrary);
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
    if (originals_.find(mon.device) == originals_.end()) {
        HDC dc = CreateDCW(L"DISPLAY", mon.device.c_str(), nullptr, nullptr);
        if (!dc) { mon.gammaStatus = 0; return false; }
        adoptOriginal(mon.device, dc);
        DeleteDC(dc);
    }
    const auto original = originals_.find(mon.device);
    const Ramp* baseline = original != originals_.end() ? &original->second : nullptr;
    const Ramp ramp = buildAppliedRamp(mon.kelvin, mon.enabled, baseline);
    mon.gammaStatus = -1;  // Worker publishes real hardware result later.
    const int shared = sharedMatrixKelvin(monitors_);
    queueGamma(mon.device, shared ? buildKelvinRamp(6500) : ramp, shared, ramp,refresh);
    return true;
}
void Model::applyAll(GammaRefresh refresh) {
    for (auto& monitor : monitors_) applyDisplay(monitor,refresh);
}
bool Model::persistProfile(size_t index) {
    if (index >= monitors_.size() || !loaded_) return false;
    const auto& mon = monitors_[index];
    auto& profile = root_["screen"]["monitor_profiles"][toUtf8(mon.device)];
    profile["enabled"] = mon.enabled;
    profile["kelvin"] = mon.kelvin;
    profile["custom_name"] = toUtf8(mon.alias);
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
bool Model::renameMonitor(size_t index, const std::wstring& alias) {
    if (index >= monitors_.size() || !loaded_ || alias.size() > 64) return false;
    for (wchar_t c : alias) if (c < 0x20) return false;
    monitors_[index].alias = alias;
    return persistProfile(index);
}
void Model::updatePreferences(const Preferences& updated) {
    preferences_.locale = updated.locale == L"pt-PT" ? L"pt-PT" : L"en-US";
    preferences_.theme = updated.theme == L"light" ? L"light" : L"dark";
    preferences_.startup = (updated.startup == L"tray" ||
        updated.startup == L"minimized") ? updated.startup : L"normal";
    save();
}
void Model::restoreOriginals() {
    // No queued/active gamma request may overwrite a restored calibration.
    stopGamma();
    for (const auto& item : originals_) {
        HDC dc = CreateDCW(L"DISPLAY", item.first.c_str(), nullptr, nullptr);
        if (dc) {
            SetDeviceGammaRamp(dc, const_cast<Ramp*>(&item.second));
            DeleteDC(dc);
        }
    }
    originals_.clear();
}
void Model::migrateAutorunEntry() const {
    // Older NightLight builds created a Run key with a hardcoded
    // --minimized argument. Keep the user's existing autostart preference,
    // but let the new saved startup_mode determine window behavior.
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
                                old.find(L"--tray") != std::wstring::npos;
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
