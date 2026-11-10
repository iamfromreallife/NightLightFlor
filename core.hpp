#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include "json.hpp"

namespace nightlight {
struct Ramp {
    WORD red[256] = {};
    WORD green[256] = {};
    WORD blue[256] = {};
};

struct Monitor {
    std::wstring device;
    std::wstring windowsName;
    std::wstring alias;
    bool primary = false;
    bool enabled = true;
    int kelvin = 5000;
    // -1 unknown, 0 not supported by this display/driver, 1 success
    int gammaStatus = -1;
    std::wstring displayName() const { return alias.empty() ? windowsName : alias; }
};

struct Preferences {
    std::wstring locale = L"en-US";
    std::wstring theme = L"dark";
    std::wstring startup = L"normal";
};

std::wstring fromUtf8(const std::string& input);
std::string toUtf8(const std::wstring& input);
std::filesystem::path executableDirectory();
std::wstring resolveStartup(const std::wstring& preference,
                            const std::wstring& argument, bool trayAvailable);
// 6500 K is an identity (neutral D65/sRGB-reference) LUT. Disabled filters
// and clean shutdown use the original captured monitor ramp instead.
Ramp buildKelvinRamp(int kelvin);
Ramp buildAppliedRamp(int kelvin, bool enabled, const Ramp* originalRamp);
bool sameRamp(const Ramp& a, const Ramp& b);

// A generous, DPI-scaled mouse target independent of the narrow visual rail.
int sliderPositionFromX(int x,int left,int right,int minimum,int maximum);
RECT sliderHitRect(RECT visualTrack, int horizontalPadding, int verticalPadding);

// Minimum dimensions are derived from the live monitor-tab wrapping and the
// visible sections. The computation is independent of the window manager so
// it can be verified by native regression tests.
struct WindowContentMinimum {
    int clientWidth = 0;
    int clientHeight = 0;
    int screenRows = 1;
};
WindowContentMinimum calculateWindowContentMinimum(int dpi, int clientWidth,
    int enableButtonWidth, const std::vector<int>& tabDesiredWidths,
    size_t ddcMonitorRows, bool gammaUnlocked, bool ddcVisible);

// Upstream NightLight's RGB factors; shared desktop matrix avoids GDI limits.
struct ColorGains { double red, green, blue; };
ColorGains kelvinGains(int kelvin);
int sharedMatrixKelvin(const std::vector<Monitor>& monitors);
std::wstring monitorTooltip(const std::vector<Monitor>& monitors);
bool gammaRangeUnlocked();
DWORD unlockGammaRange();

enum class GammaRefresh { Normal, Verify, Restore };
struct GammaRampState { Ramp ramp{}; bool valid=false; };
// Injectable transport keeps reset recovery and no-op suppression testable
// without changing the test machine's displays.
bool updateGammaRamp(const Ramp& target,GammaRefresh refresh,GammaRampState& state,
    const std::function<bool(Ramp&)>& read,const std::function<bool(const Ramp&)>& write);

class Model {
public:
    Model();
    explicit Model(std::filesystem::path directory);
    ~Model();

    bool load();
    bool save();
    bool refreshMonitors();
    bool moveMonitor(size_t from,size_t to);
    void applyAll(GammaRefresh refresh=GammaRefresh::Normal);
    void setStatusWindow(HWND window);
    bool collectGammaResults();
    bool previewKelvin(size_t index, int kelvin);
    bool updateKelvin(size_t index, int kelvin);
    bool updateEnabled(size_t index, bool enabled);
    bool renameMonitor(size_t index, const std::wstring& alias);
    void updatePreferences(const Preferences& updated);
    bool hasHardwarePercent(const std::wstring& device,bool contrast) const;
    int hardwarePercent(const std::wstring& device,bool contrast) const;
    bool rememberHardwarePercent(const std::wstring& device,bool contrast,int percent);
    // Physical desktop coordinates; keep negative values for monitors to the left.
    bool rememberWindowPosition(int x, int y);
    bool rememberWindowSize(int width, int height);
    std::optional<POINT> windowPosition() const { return windowPosition_; }
    std::optional<SIZE> windowSize() const { return windowSize_; }
    void restoreOriginals();
    void migrateAutorunEntry() const;

    const std::vector<Monitor>& monitors() const { return monitors_; }
    const Preferences& preferences() const { return preferences_; }
    const std::filesystem::path& configPath() const { return configPath_; }
    const std::wstring& lastError() const { return lastError_; }
    bool isReady() const { return loaded_; }
private:
    bool persistProfile(size_t index);
    void readProfile(Monitor& monitor) const;
    bool applyDisplay(Monitor& monitor,GammaRefresh refresh);
    void adoptOriginal(const std::wstring& device, HDC dc);
    void queueGamma(const std::wstring& device, const Ramp& ramp, int matrixKelvin, const Ramp& fallback,GammaRefresh refresh);
    void gammaLoop();
    void stopGamma();

    struct GammaJob {
        std::uint64_t generation = 0;
        Ramp ramp{};
        int matrixKelvin = 0;
        Ramp fallback{};
        GammaRefresh refresh=GammaRefresh::Normal;
    };
    std::mutex gammaMutex_;
    std::condition_variable gammaWake_;
    std::map<std::wstring, GammaJob> pendingGamma_;
    std::map<std::wstring, std::uint64_t> newestGeneration_;
    std::map<std::wstring, int> gammaResults_;
    HANDLE gammaEvent_=nullptr;
    std::thread gammaThread_;
    bool gammaStopping_ = false;
    HWND statusWindow_ = nullptr;

    std::filesystem::path directory_;
    std::filesystem::path configPath_;
    portable::Json root_ = portable::Json::object();
    Preferences preferences_;
    std::optional<POINT> windowPosition_;
    std::optional<SIZE> windowSize_;
    std::vector<Monitor> monitors_;
    std::map<std::wstring, Ramp> originals_;
    std::string savedDocument_;
    FILETIME savedStamp_{};
    bool savedStampValid_=false;
    bool loaded_ = false;
    std::wstring lastError_;
};
} // namespace nightlight
