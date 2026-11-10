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
};
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

// All potentially slow monitor I/O runs on one worker. No hardware writes occur
// until a known saved setting is restored or the user moves an available slider. Readback is not a prerequisite for
// a direct hardware write (matching upstream NightLight). Writes coalesce by display
// and feature, so changing tabs cannot redirect a queued request to another monitor.
class DdcController {
public:
    DdcController();
    ~DdcController();
    void setWindow(HWND window);
    void seed(const std::wstring& device,int brightness,int contrast,bool savedBrightness=false,bool savedContrast=false);
    void restore(const std::wstring& device,int brightness,int contrast,bool savedBrightness,bool savedContrast);
    void refresh(const std::wstring& device);
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
    std::thread worker_;
};
}
