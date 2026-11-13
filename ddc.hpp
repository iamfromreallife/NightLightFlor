#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace nightlight {
constexpr UINT WM_DDC_COMPLETE = WM_APP + 42;
// Hardware can be changed by monitor OSD controls, other software or hotkeys.
// Poll only while the controls are visible; all I2C/WMI reads stay on the worker.
inline constexpr UINT kDdcReadbackIntervalMs = 3000;
inline constexpr std::uint64_t kDdcPostWriteGraceMs = 1500;
enum class DdcFeature { Brightness = 0, Contrast = 1 };
enum class DdcBackend { None, DdcCi, WindowsWmi };
struct DdcValue {
    bool supported = false;
    bool readable = false;
    bool hasValue = false;
    bool managed = false;
    bool pending = false;
    bool failed = false;
    int percent = 50;
    DdcBackend backend = DdcBackend::None;
    std::uint64_t generation = 0;
    std::uint64_t protectUntilTickMs = 0; // prevents stale reads right after our own write
};
// Merge a hardware observation without overwriting an in-flight local slider
// adjustment. A valid read is authoritative; an unsupported/unreadable read
// keeps the last displayed or stored percentage.
void applyDdcObservation(DdcValue& current,const DdcValue& observed,
                         bool protectUserValue);
struct DdcState {
    bool loading = false;
    DdcValue brightness;
    DdcValue contrast;
};
DWORD ddcRawValue(DWORD minimum, DWORD maximum, int percent);
int ddcPercentage(DWORD minimum, DWORD maximum, DWORD value);
using DdcWrite = std::function<bool(DWORD)>;
bool writeDdcValue(bool readable,DWORD minimum,DWORD maximum,int percent,
                   const DdcWrite& highLevel,const DdcWrite& vcp);

// Round-robin over coalesced device/feature jobs prevents a hot brightness
// slider from starving contrast or a different display.
template<class Map>
auto nextMonitorJob(Map& jobs,const typename Map::key_type& previous,bool first) -> typename Map::iterator {
    if (first || jobs.empty()) return jobs.begin();
    auto next=jobs.upper_bound(previous);
    return next==jobs.end()?jobs.begin():next;
}

// All potentially slow monitor I/O runs on one worker. Reading the real
// hardware value never writes to a display. Local slider writes coalesce by
// display and feature; unreadable devices retain their saved UI fallback.
class DdcController {
public:
    DdcController();
    ~DdcController();
    void setWindow(HWND window);
    void seed(const std::wstring& device,int brightness,int contrast,bool savedBrightness=false,bool savedContrast=false);
    void restore(const std::wstring& device,int brightness,int contrast,bool savedBrightness,bool savedContrast);
    void refresh(const std::wstring& device);
    // Discard stale device-specific jobs and cached values when GDI names may
    // have been reassigned to different physical panels.
    void invalidateTopology();
    void poll(const std::wstring& device); // silent read; does not disable live sliders
    bool set(const std::wstring& device, DdcFeature feature, int percent);
    DdcState state(const std::wstring& device);
    void stop();
private:
    struct Job { int percent = 0; std::uint64_t generation = 0; };
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::wstring,DdcState> states_;
    std::map<std::pair<std::wstring,int>,Job> jobs_;
    HWND window_ = nullptr;
    bool stopping_ = false;
    std::uint64_t topologyEpoch_=0;
    std::thread worker_;
};
}
